/**************************************************************************/
/*  macrame_render_grant.cpp                                              */
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

#include "macrame_render_grant.h"

#ifdef MACRAME_ENABLED
#include "ts/access.h"
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define MACRAME_NO_INLINE __declspec(noinline)
#else
#define MACRAME_NO_INLINE [[gnu::noinline]]
#endif

thread_local bool macrame_tls_holds_render_grant = false;
thread_local bool macrame_tls_holds_render_device_grant = false;
thread_local bool macrame_tls_holds_physics_grant = false;

MACRAME_NO_INLINE void MacramePhysics::set_holds_grant(bool p_holds) {
	macrame_tls_holds_physics_grant = p_holds;
}

namespace {
ts::Guarded<PhysicsGrantToken> *physics_guarded = nullptr;
} // namespace

void MacramePhysics::set_guarded(ts::Guarded<PhysicsGrantToken> *p_guarded) {
	physics_guarded = p_guarded;
}

ts::Guarded<PhysicsGrantToken> *MacramePhysics::get_guarded() {
	return physics_guarded;
}

MACRAME_NO_INLINE void MacrameRender::set_holds_grant(bool p_holds) {
	macrame_tls_holds_render_grant = p_holds;
}

MACRAME_NO_INLINE void MacrameRenderDevice::set_holds_grant(bool p_holds) {
	macrame_tls_holds_render_device_grant = p_holds;
}

namespace {
bool (*render_access_query)() = nullptr;
RenderGrantToken *render_token = nullptr;
} // namespace

void MacrameRender::set_access_query(bool (*p_query)()) {
	render_access_query = p_query;
}

void MacrameRender::set_token(RenderGrantToken *p_token) {
	render_token = p_token;
}

bool MacrameRender::check_access() {
	if (macrame_tls_holds_render_grant || macrame_tls_holds_render_device_grant) {
		return true;
	}
	if (!render_access_query) {
		// Before the render server registers (RenderingDevice::initialize runs first, on the main
		// thread) there is no draw to conflict with: a blue thread may call directly.
		return true;
	}
	if (render_access_query()) {
		return true; // Direct mode: a blue thread, nothing in flight.
	}
#ifdef MACRAME_ENABLED
	if (render_token) {
		ts::access_check(render_token); // Fatal under TS_SAFETY_CHECKS unless the running task declared the grant.
		return true;
	}
#endif
	return false;
}
