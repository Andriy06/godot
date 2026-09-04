/**************************************************************************/
/*  macrame_render_grant.h                                                */
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

namespace ts {
template <typename T>
class Guarded;
}

// Phase 1 of the Macrame conversion: the render server's state is one guarded object and the
// frame's draw runs as a write task on a worker. The wrapper macros in
// `rendering_server_default.h` need a cheap "am I running under that grant?" test to choose
// between the direct call and staging; this is it. The flag is set by the body that holds
// the grant and read through out-of-line accessors, per Macrame's thread-local guidance
// (a compiler may cache a thread-local's address across a suspension; these bodies never
// suspend, but the discipline costs nothing).

struct RenderGrantToken {
	// Placeholder for the renderer's guarded identity. Phase 2 moves `TS_CHECK_ACCESS()` into
	// the server entry points against this object.
	int unused = 0;
};

extern thread_local bool macrame_tls_holds_render_grant;
extern thread_local bool macrame_tls_holds_physics_grant;

namespace MacrameRender {
inline bool holds_grant() {
	return macrame_tls_holds_render_grant;
}
void set_holds_grant(bool p_holds);
} // namespace MacrameRender

struct PhysicsGrantToken {
	int unused = 0;
};

namespace MacramePhysics {
inline bool holds_grant() {
	return macrame_tls_holds_physics_grant;
}
void set_holds_grant(bool p_holds);
// The physics wrapper registers its guarded space here so scene shards can declare a read on it.
void set_guarded(ts::Guarded<PhysicsGrantToken> *p_guarded);
ts::Guarded<PhysicsGrantToken> *get_guarded();
} // namespace MacramePhysics
