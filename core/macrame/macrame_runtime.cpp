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

#include "core/string/print_string.h"
#include "core/variant/variant.h"

#ifdef MACRAME_ENABLED
#include "ts/scheduler.h"

#include <thread>
#endif

void MacrameRuntime::init(int p_workers) {
#ifdef MACRAME_ENABLED
	ts::Scheduler_config cfg;
	if (p_workers > 0) {
		cfg.num_workers = p_workers;
	}
	ts::create_scheduler(cfg);
	print_verbose(vformat("Macrame: scheduler up, %d workers.", get_worker_count()));
#else
	(void)p_workers;
#endif
}

void MacrameRuntime::finish() {
#ifdef MACRAME_ENABLED
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
	const uint32_t n = ts::current_scheduler_config().num_workers;
	return (int)(n == 0 ? std::thread::hardware_concurrency() : n);
#else
	return 0;
#endif
}
