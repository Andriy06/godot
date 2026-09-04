/**************************************************************************/
/*  macrame_command_queue.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#ifdef MACRAME_ENABLED

// A drop-in for `CommandQueueMT` backed by Macrame's staged-write primitives. Phase 1 of the
// conversion: the render server's state is one guarded object, callers that do not hold its
// grant stage their commands into a `Deferred` journal instead of pushing into a mutex-guarded
// ring, `sync()` applies the batch as one write, and `launch()` runs a body (the frame's
// `_draw`) as an asynchronous write task that overlaps the next frame's simulation.
//
// The interface mirrors the subset of `CommandQueueMT` the `FUNC*` wrapper macros use, so
// `rendering_server_default.h` needs no macro changes beyond the async condition.

#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/recorder.h"
#include "ts/scheduler.h"
#include "ts/task.h"

#include "core/macrame/macrame_scene.h"
#include "core/profiling/profiling.h"

#include <atomic>
#include <utility>
#include <vector>

template <typename Token>
class MacrameCommandQueue {
	ts::Guarded<Token> guarded;
	ts::Deferred<Token> staged;
	ts::Recorder<Token> recorder; // The blue thread's recorder.
	std::vector<ts::Recorder<Token>> shard_recorders; // One per scene shard: FIFO per producer, deterministic apply order.
	ts::Task<void> in_flight;
	bool has_in_flight = false;
	bool (*holds_grant)() = nullptr;
	std::atomic<uint64_t> staged_count{ 0 };

	void _wait_in_flight() {
		if (has_in_flight) {
			GodotProfileZone("MacrameCommandQueue: wait for in-flight draw");
			in_flight.sync();
			has_in_flight = false;
		}
	}

public:
	// `p_holds_grant` reports whether the calling thread is running the body that holds the
	// object's grant (the launched draw or step); it is the one caller allowed to touch the
	// object directly while a task is in flight.
	MacrameCommandQueue(const char *p_name, bool (*p_holds_grant)()) :
			guarded(ts::Named{ p_name }), staged(guarded), recorder(staged.recorder()), holds_grant(p_holds_grant) {
		shard_recorders.reserve(MacrameScene::SHARD_COUNT);
		for (int i = 0; i < MacrameScene::SHARD_COUNT; i++) {
			shard_recorders.push_back(staged.recorder());
		}
	}

	~MacrameCommandQueue() {
		_wait_in_flight();
		staged.discard();
	}

	MacrameCommandQueue(const MacrameCommandQueue &) = delete;
	MacrameCommandQueue &operator=(const MacrameCommandQueue &) = delete;

	ts::Guarded<Token> &get_guarded() { return guarded; }

	// Record a member call for the next apply. Arguments are copied (decayed) like
	// `CommandQueueMT` does, so RIDs, Variants and containers are safe to hand over.
	template <typename T, typename M, typename... Args>
	void push(T *p_instance, M p_method, Args... p_args) {
		const int shard = MacrameScene::current_shard();
		ts::Recorder<Token> &rec = shard < 0 ? recorder : shard_recorders[shard];
		rec.stage([=](Token &) { (p_instance->*p_method)(p_args...); });
		staged_count.fetch_add(1, std::memory_order_relaxed);
	}

	template <typename T, typename M, typename... Args>
	void push_and_sync(T *p_instance, M p_method, Args... p_args) {
		push(p_instance, p_method, p_args...);
		sync();
	}

	// A synchronous round trip: drain everything before it, then run the getter under the
	// grant on the calling (blue) thread. Same cost profile as the separate-thread mode's
	// `push_and_ret`: it serializes against the in-flight draw.
	template <typename T, typename M, typename R, typename... Args>
	void push_and_ret(T *p_instance, M p_method, R *r_ret, Args... p_args) {
		GodotProfileZone("MacrameCommandQueue: synchronous getter");
		if (MacrameScene::in_shard_task()) {
			// A synchronous server getter from a shard would need the server's write grant while
			// the shard holds a read grant on it (or none): a certain deadlock, and a design smell.
			// Read published state instead. Report and return the default.
			ERR_PRINT_ONCE("Macrame: synchronous server getter called from a scene shard; returning a default value. Read published state instead.");
			return;
		}
		_wait_in_flight();
		guarded.access([&](Token &) {
			(void)staged.commit(); // Inline apply: this body holds the write grant.
			*r_ret = (p_instance->*p_method)(p_args...);
		}).sync();
	}

	// Whether the caller may touch the object directly: it holds the grant, or it is a blue
	// thread (not a worker) with no task in flight. Wrappers stage otherwise.
	bool may_call_direct() const {
		return holds_grant() || (ts::current_worker_index() < 0 && !has_in_flight && !MacrameScene::in_shard_task());
	}

	// Before a direct call from a blue thread: join the in-flight task and apply what was
	// staged, so the direct call observes every earlier command in order. Under the grant
	// this is a no-op (the batch was applied before the grant was handed out).
	void flush_if_pending() {
		if (holds_grant() || MacrameScene::in_shard_task()) {
			return; // Shards read the state that was current when the phase began.
		}
		if (has_in_flight || staged_count.load(std::memory_order_relaxed) != 0) {
			sync();
		}
	}

	// Apply every staged command as one write on the calling thread.
	void flush_all() { sync(); }

	void sync() {
		GodotProfileZone("MacrameCommandQueue: sync + apply batch");
		_wait_in_flight();
		if (staged_count.load(std::memory_order_relaxed) == 0) {
			return;
		}
		guarded.access([&](Token &) {
			(void)staged.commit(); // Inline apply: this body holds the write grant.
		}).sync();
		staged_count.store(0, std::memory_order_relaxed);
	}

	// Run `p_body(Token&)` as an asynchronous write task holding the grant. The caller must
	// have called `sync()` first (the frame loop does). Returns immediately.
	template <typename Fn>
	void launch(Fn &&p_body, const char *p_name) {
		_wait_in_flight();
		in_flight = guarded.async(std::forward<Fn>(p_body), { .name = p_name });
		has_in_flight = true;
	}

	// Interface parity with CommandQueueMT for the pump-task branch that Macrame mode never takes.
	template <typename TaskID>
	void set_pump_task_id(TaskID) {}

	bool is_in_flight() const { return has_in_flight; }
	void wait() { _wait_in_flight(); }
};

#endif // MACRAME_ENABLED
