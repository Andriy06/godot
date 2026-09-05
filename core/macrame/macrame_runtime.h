/**************************************************************************/
/*  macrame_runtime.h                                                     */
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

// Lifecycle of the Macrame scheduler inside the engine. Phase 0 of the conversion: the
// scheduler exists and is torn down cleanly, and nothing else uses it yet. Compiled to
// no-ops when the build has no `macrame_path`.

class MacrameRuntime {
public:
	// Brings the process-wide scheduler up. Called once from Main::setup after the worker
	// pool exists and before any server is created. `p_workers` <= 0 means hardware
	// concurrency.
	static void init(int p_workers);
	// The main loop is gone, so no frame graph will run again: destroy the compiled graphs while
	// the guarded objects they name (the physics space, the render outputs) are still alive.
	// `Main::cleanup` frees those objects well before `finish()`.
	static void finish_graphs();
	static void finish();
	static bool is_enabled();
	static int get_worker_count();
	// A long serial task (the frame's draw, the physics step) shares the machine with many short
	// shard tasks; this asks the OS to prefer it when they compete for a fast core. A priority
	// hint on the calling worker thread, never a pin. MACRAME_TASK_PRIORITY=0 disables.
	static void long_task_begin();
	static void long_task_end();
};
