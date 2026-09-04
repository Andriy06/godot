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
#include "ts/task.h"

#include "core/profiling/profiling.h"

#include <utility>

template <typename Token>
class MacrameCommandQueue {
	ts::Guarded<Token> guarded;
	ts::Deferred<Token> staged;
	ts::Recorder<Token> recorder;
	ts::Task<void> in_flight;
	bool has_in_flight = false;

	void _wait_in_flight() {
		if (has_in_flight) {
			GodotProfileZone("MacrameCommandQueue: wait for in-flight draw");
			in_flight.sync();
			has_in_flight = false;
		}
	}

public:
	explicit MacrameCommandQueue(const char *p_name) :
			guarded(ts::Named{ p_name }), staged(guarded), recorder(staged.recorder()) {}

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
		recorder.stage([=](Token &) { (p_instance->*p_method)(p_args...); });
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
		_wait_in_flight();
		guarded.access([&](Token &) {
			(void)staged.commit(); // Inline apply: this body holds the write grant.
			*r_ret = (p_instance->*p_method)(p_args...);
		}).sync();
	}

	// The direct branch of the wrappers only runs under the grant, where nothing is pending
	// by construction (the batch was applied before the grant was handed out).
	void flush_if_pending() {}

	// Apply every staged command as one write on the calling thread.
	void flush_all() { sync(); }

	void sync() {
		GodotProfileZone("MacrameCommandQueue: sync + apply batch");
		_wait_in_flight();
		guarded.access([&](Token &) {
			(void)staged.commit(); // Inline apply: this body holds the write grant.
		}).sync();
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
