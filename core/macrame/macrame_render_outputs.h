/**************************************************************************/
/*  macrame_render_outputs.h                                              */
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

#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/templates/rid.h"

#include <cstdint>
#include <type_traits>

// The renderer's per-frame outputs that the scene reads back: measured render times, render
// info counters, particle activity. The draw runs as an asynchronous task a frame behind the
// simulation, so a synchronous getter would serialize the two (the separate-thread mode pays
// the same round trip). Instead the draw task stages this struct at the end of every frame
// and the main thread publishes it as one version once the draw is joined (`ts::Versioned`):
// readers always see the last completed frame, one version behind, without touching the
// renderer.
struct MacrameRenderOutputs {
	static constexpr int RENDER_INFO_TYPES = 3; // == RSE::VIEWPORT_RENDER_INFO_TYPE_MAX (checked where used).
	static constexpr int RENDER_INFOS = 3; // == RSE::VIEWPORT_RENDER_INFO_MAX.

	struct ViewportStats {
		double time_cpu = 0.0; // Milliseconds, as viewport_get_measured_render_time_cpu reports.
		double time_gpu = 0.0;
		int render_info[RENDER_INFO_TYPES][RENDER_INFOS] = {};
	};

	HashMap<RID, ViewportStats> viewports; // Every active viewport of the frame.
	HashSet<RID> inactive_particles; // Particle systems that finished (not emitting, inactive).
	uint64_t frame = 0;
};

#ifdef MACRAME_ENABLED
namespace ts {
template <typename T>
class Guarded;
}
#endif

class MacrameRenderSnapshot {
public:
	static void init();
	static void finish();

	// Producer: the draw task (holding the render grant) stages the frame's outputs.
	static void stage(MacrameRenderOutputs &&p_outputs);
	// A blue thread, once the draw is joined: the staged frame becomes the published version.
	static void publish();

	// Read the published version. Inside a scene shard the read goes through the grant the
	// shard task declared (`MacrameScene::run_groups`); on a blue thread it is a synced read
	// access. Returns false, without calling back, when nothing can be read (Macrame off, a
	// worker task without the grant).
	static bool read(void (*p_fn)(const MacrameRenderOutputs &, void *), void *p_userdata);

	template <typename Fn>
	static bool read(Fn &&p_fn) {
		using F = std::remove_reference_t<Fn>;
		return read([](const MacrameRenderOutputs &p_outputs, void *p_userdata) { (*static_cast<F *>(p_userdata))(p_outputs); }, &p_fn);
	}

#ifdef MACRAME_ENABLED
	// The front `Guarded`: what a task declares a read on.
	static ts::Guarded<MacrameRenderOutputs> &front();
	// Set for the duration of a shard task: the front's contents, reachable under that grant.
	static void set_current(const MacrameRenderOutputs *p_current);
#endif
};
