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
// Two journals, double buffered: `launch_pipelined()` enqueues the current journal's commit
// as an async write on the object (FIFO behind the running task), enqueues the body behind
// it, and switches staging to the other journal, so the main thread can run a whole frame
// ahead of the renderer without waiting. At most one task is queued behind the running one;
// a third launch joins the oldest first.
//
// The interface mirrors the subset of `CommandQueueMT` the `FUNC*` wrapper macros use, so
// `rendering_server_default.h` needs no macro changes beyond the async condition.

#include "ts/deferred.h"
#include "ts/guarded.h"
#include "ts/priority.h"
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
	struct Journal {
		ts::Deferred<Token> staged;
		ts::Recorder<Token> recorder; // The blue thread's recorder.
		std::vector<ts::Recorder<Token>> shard_recorders; // One per scene shard: FIFO per producer, deterministic apply order.
		std::atomic<uint64_t> count{ 0 };
		ts::Task<void> commit_task; // The enqueued commit of a pipelined launch; settled before the task launched after it runs.
		bool has_commit_task = false;

		explicit Journal(ts::Guarded<Token> &p_guarded) :
				staged(p_guarded), recorder(staged.recorder()) {
			shard_recorders.reserve(MacrameScene::SHARD_COUNT);
			for (int i = 0; i < MacrameScene::SHARD_COUNT; i++) {
				shard_recorders.push_back(staged.recorder());
			}
		}
	};

	ts::Guarded<Token> guarded;
	Journal journal_a;
	Journal journal_b;
	Journal *cur = &journal_a; // Where new commands are staged.
	ts::Task<void> in_flight; // The oldest task on the object.
	bool has_in_flight = false;
	ts::Task<void> queued; // A younger task, FIFO behind `in_flight` (pipelined launches only).
	bool has_queued = false;
	bool (*holds_grant)() = nullptr;
	// The split draw launches a second task (the device submit) from inside the body this queue
	// runs. That task is not on this object, so the queue has to be told how to join it and how to
	// ask whether it is still running, or a blue thread would take the direct path while the
	// device task is still submitting.
	void (*downstream_join)() = nullptr;
	bool (*downstream_busy)() = nullptr;

	void _settle_commits() {
		for (Journal *j : { &journal_a, &journal_b }) {
			if (j->has_commit_task) {
				j->commit_task.sync(); // FIFO before the task launched after it, so already settled.
				j->has_commit_task = false;
			}
		}
	}

	// Join everything on the object.
	void _wait_in_flight() {
		if (has_in_flight || has_queued) {
			GodotProfileZone("MacrameCommandQueue: wait for in-flight draw");
			if (has_in_flight) {
				in_flight.sync();
				has_in_flight = false;
			}
			if (has_queued) {
				queued.sync();
				has_queued = false;
			}
			_settle_commits();
		}
		if (downstream_join) {
			downstream_join(); // Every body has run, so every downstream task has been launched.
		}
	}

	// Leave at most one task on the object: join the oldest if a younger one is queued.
	void _wait_oldest() {
		if (has_queued) {
			GodotProfileZone("MacrameCommandQueue: wait for oldest draw");
			in_flight.sync();
			in_flight = std::move(queued);
			has_queued = false;
		}
	}

	// This body holds the write grant: apply both journals inline (the one not being staged
	// into first; a journal whose commit was enqueued is empty by now).
	void _commit_inline() {
		for (Journal *j : { cur == &journal_a ? &journal_b : &journal_a, cur }) {
			if (j->count.load(std::memory_order_relaxed) != 0) {
				(void)j->staged.commit();
				j->count.store(0, std::memory_order_relaxed);
			}
		}
	}

	bool _has_staged() const {
		return journal_a.count.load(std::memory_order_relaxed) != 0 || journal_b.count.load(std::memory_order_relaxed) != 0;
	}

public:
	// `p_holds_grant` reports whether the calling thread is running the body that holds the
	// object's grant (the launched draw or step); it is the one caller allowed to touch the
	// object directly while a task is in flight.
	MacrameCommandQueue(const char *p_name, bool (*p_holds_grant)()) :
			guarded(ts::Named{ p_name }), journal_a(guarded), journal_b(guarded), holds_grant(p_holds_grant) {}

	~MacrameCommandQueue() {
		_wait_in_flight();
		journal_a.staged.discard();
		journal_b.staged.discard();
	}

	MacrameCommandQueue(const MacrameCommandQueue &) = delete;
	MacrameCommandQueue &operator=(const MacrameCommandQueue &) = delete;

	ts::Guarded<Token> &get_guarded() { return guarded; }

	// See `downstream_join` / `downstream_busy`.
	void set_downstream(void (*p_join)(), bool (*p_busy)()) {
		downstream_join = p_join;
		downstream_busy = p_busy;
	}

	// Leave at most one body outstanding on this object, so the caller of the next launch knows
	// the body two launches back has finished (and therefore published whatever it staged).
	void wait_oldest() { _wait_oldest(); }

	// Record a member call for the next apply. Arguments are copied (decayed) like
	// `CommandQueueMT` does, so RIDs, Variants and containers are safe to hand over.
	template <typename T, typename M, typename... Args>
	void push(T *p_instance, M p_method, Args... p_args) {
		const int shard = MacrameScene::current_shard();
		Journal &j = *cur;
		ts::Recorder<Token> &rec = shard < 0 ? j.recorder : j.shard_recorders[shard];
		rec.stage([=](Token &) { (p_instance->*p_method)(p_args...); });
		j.count.fetch_add(1, std::memory_order_relaxed);
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
			_commit_inline();
			*r_ret = (p_instance->*p_method)(p_args...);
		}).sync();
	}

	// Whether the caller may touch the object directly: it holds the grant, or it is a blue
	// thread (not a worker) with no task in flight. Wrappers stage otherwise.
	bool may_call_direct() const {
		if (holds_grant()) {
			return true;
		}
		if (downstream_busy && downstream_busy()) {
			return false;
		}
		return ts::current_worker_index() < 0 && !has_in_flight && !has_queued && !MacrameScene::in_shard_task();
	}

	// Before a direct call from a blue thread: join the in-flight task and apply what was
	// staged, so the direct call observes every earlier command in order. Under the grant
	// this is a no-op (the batch was applied before the grant was handed out).
	void flush_if_pending() {
		if (holds_grant() || MacrameScene::in_shard_task()) {
			return; // Shards read the state that was current when the phase began.
		}
		if (has_in_flight || has_queued || _has_staged()) {
			sync();
		}
	}

	// Apply every staged command as one write on the calling thread.
	void flush_all() { sync(); }

	// The caller holds the object's write grant (a graph node, a launched body): apply the staged
	// journals inline.
	void commit_under_grant() { _commit_inline(); }

	void sync() {
		GodotProfileZone("MacrameCommandQueue: sync + apply batch");
		_wait_in_flight();
		if (!_has_staged()) {
			return;
		}
		guarded.access([&](Token &) {
			_commit_inline();
		}).sync();
	}

	// Run `p_body(Token&)` as an asynchronous write task holding the grant. The caller must
	// have called `sync()` first (the physics tick does). Returns immediately.
	template <typename Fn>
	void launch(Fn &&p_body, const char *p_name) {
		_wait_in_flight();
		in_flight = guarded.async(std::forward<Fn>(p_body), { .name = p_name });
		has_in_flight = true;
	}

	// Pipelined form: enqueue this frame's batch (the current journal) as an async write
	// behind the running task, then `p_body` behind it, and stage the next frame into the
	// other journal. Waits only if a task is already queued behind the running one, so the
	// caller runs at most one frame ahead of the object.
	// `p_priority`: the render draw is the frame's longest serial task and queues behind the
	// shard nodes for a worker at equal priority; high puts it first in the ready queues.
	template <typename Fn>
	void launch_pipelined(Fn &&p_body, const char *p_name, ts::Priority p_priority = ts::Priority::normal) {
		_wait_oldest();
		Journal &j = *cur;
		if (j.has_commit_task) {
			j.commit_task.sync(); // Two launches ago; settled long since.
			j.has_commit_task = false;
		}
		if (j.count.load(std::memory_order_relaxed) != 0) {
			j.commit_task = j.staged.commit({ .priority = p_priority }); // Enqueued: one write, cut when it runs, FIFO on the object.
			j.has_commit_task = true;
			j.count.store(0, std::memory_order_relaxed);
		}
		ts::Task<void> t = guarded.async(std::forward<Fn>(p_body), { .priority = p_priority, .name = p_name });
		if (has_in_flight) {
			queued = std::move(t);
			has_queued = true;
		} else {
			in_flight = std::move(t);
			has_in_flight = true;
		}
		cur = (cur == &journal_a) ? &journal_b : &journal_a;
	}

	// Interface parity with CommandQueueMT for the pump-task branch that Macrame mode never takes.
	template <typename TaskID>
	void set_pump_task_id(TaskID) {}

	bool is_in_flight() const { return has_in_flight || has_queued || (downstream_busy && downstream_busy()); }
	void wait() { _wait_in_flight(); }
};

#endif // MACRAME_ENABLED
