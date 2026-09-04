/**************************************************************************/
/*  macrame_runtime.cpp                                                   */
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

#include "macrame_runtime.h"

#include "core/macrame/macrame_render_outputs.h"
#include "core/macrame/macrame_scene.h"

#include "core/string/print_string.h"
#include "core/variant/variant.h"

#ifdef MACRAME_ENABLED
#include "ts/scheduler.h"

#include <thread>
#endif

#ifdef WINDOWS_ENABLED
#include <windows.h>

#include <cstdlib>
#endif

namespace {
// Windows 11 on a hybrid CPU (P/E cores) schedules a process that is not the foreground window with
// a "prefer efficient cores" policy: ETW context-switch data showed the draw task and even the main
// thread spending most of their time on E-cores. Opting the process out of power throttling (EcoQoS)
// asks for the high-QoS policy instead; this is a scheduling hint, not a pin (MACRAME_QOS=0 disables).
void request_high_qos() {
#ifdef WINDOWS_ENABLED
	const char *env = std::getenv("MACRAME_QOS");
	if (env && env[0] == '0') {
		return;
	}
	PROCESS_POWER_THROTTLING_STATE state = {};
	state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
	state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
	state.StateMask = 0; // Controlled and off: never throttle.
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
#endif
}
} // namespace

void MacrameRuntime::init(int p_workers) {
#ifdef MACRAME_ENABLED
	ts::Scheduler_config cfg;
	if (p_workers > 0) {
		cfg.num_workers = p_workers;
	}
	request_high_qos();
	ts::create_scheduler(cfg);
	MacrameScene::init();
	MacrameRenderSnapshot::init();
	print_verbose(vformat("Macrame: scheduler up, %d workers.", get_worker_count()));
#else
	(void)p_workers;
#endif
}

void MacrameRuntime::finish() {
#ifdef MACRAME_ENABLED
	MacrameRenderSnapshot::finish();
	MacrameScene::finish();
	if (ts::scheduler_running()) {
		ts::destroy_scheduler();
	}
#endif
}

bool MacrameRuntime::is_enabled() {
#ifdef MACRAME_ENABLED
	return ts::scheduler_running();
#else
	return false;
#endif
}

int MacrameRuntime::get_worker_count() {
#ifdef MACRAME_ENABLED
	if (!ts::scheduler_running()) {
		return (int)std::thread::hardware_concurrency(); // Before init: what the default config will use.
	}
	const uint32_t n = ts::current_scheduler_config().num_workers;
	return (int)(n == 0 ? std::thread::hardware_concurrency() : n);
#else
	return 0;
#endif
}
