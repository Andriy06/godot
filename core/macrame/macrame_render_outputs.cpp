/**************************************************************************/
/*  macrame_render_outputs.cpp                                            */
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

#include "macrame_render_outputs.h"

#ifdef MACRAME_ENABLED

#include "core/error/error_macros.h"

#include "ts/recorder.h"
#include "ts/scheduler.h"
#include "ts/versioned.h"

namespace {

struct State {
	// `overwrite`: every frame's staged command replaces the whole struct, so no replay or copy
	// is needed to keep the two replicas equivalent.
	ts::Versioned<MacrameRenderOutputs> versioned{ ts::Named{ "render_outputs" }, ts::Resync::overwrite };
	ts::Recorder<MacrameRenderOutputs> recorder = versioned.recorder(); // One producer: the draw task, frame after frame.
};

State *state = nullptr;
thread_local const MacrameRenderOutputs *tls_current = nullptr;

} // namespace

void MacrameRenderSnapshot::init() {
	ERR_FAIL_COND(state != nullptr);
	state = new State();
}

void MacrameRenderSnapshot::finish() {
	if (!state) {
		return;
	}
	state->versioned.publish().sync(); // Staged-but-unpublished commands at destruction are fatal lost writes.
	delete state;
	state = nullptr;
}

void MacrameRenderSnapshot::stage(MacrameRenderOutputs &&p_outputs) {
	ERR_FAIL_NULL(state);
	state->recorder.stage([outputs = std::move(p_outputs)](MacrameRenderOutputs &p_state) {
		p_state = outputs;
	});
}

void MacrameRenderSnapshot::publish() {
	ERR_FAIL_NULL(state);
	state->versioned.publish().sync(); // A blue thread; an empty journal is a no-op.
}

bool MacrameRenderSnapshot::read(void (*p_fn)(const MacrameRenderOutputs &, void *), void *p_userdata) {
	if (tls_current) {
		p_fn(*tls_current, p_userdata); // A shard task: the read grant was declared at launch.
		return true;
	}
	if (!state) {
		return false;
	}
	if (ts::current_worker_index() >= 0) {
		ERR_PRINT_ONCE("Macrame: render outputs read from a worker task that did not declare the grant; returning nothing.");
		return false;
	}
	state->versioned.read([&](const MacrameRenderOutputs &p_outputs) {
		p_fn(p_outputs, p_userdata);
	}).sync();
	return true;
}

ts::Guarded<MacrameRenderOutputs> &MacrameRenderSnapshot::front() {
	CRASH_COND(!state);
	return state->versioned.state();
}

void MacrameRenderSnapshot::set_current(const MacrameRenderOutputs *p_current) {
	tls_current = p_current;
}

#else

void MacrameRenderSnapshot::init() {}
void MacrameRenderSnapshot::finish() {}
void MacrameRenderSnapshot::stage(MacrameRenderOutputs &&) {}
void MacrameRenderSnapshot::publish() {}
bool MacrameRenderSnapshot::read(void (*)(const MacrameRenderOutputs &, void *), void *) {
	return false;
}

#endif // MACRAME_ENABLED
