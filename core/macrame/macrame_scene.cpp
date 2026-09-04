/**************************************************************************/
/*  macrame_scene.cpp                                                     */
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

#include "macrame_scene.h"

#ifdef MACRAME_ENABLED

#include "core/macrame/macrame_render_grant.h"
#include "core/macrame/macrame_render_outputs.h"
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/profiling/profiling.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "servers/navigation_3d/navigation_server_3d.h"
#include "servers/physics_3d/physics_server_3d_wrap_mt.h"

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/static_task_graph.h"
#include "ts/task.h"

#include "graph_trace.h" // Macrame tools: the aggregated runtime trace of a static graph.

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define MACRAME_NO_INLINE __declspec(noinline)
#else
#define MACRAME_NO_INLINE [[gnu::noinline]]
#endif

namespace {

// Experiment: a scene phase (the physics tick's shards, or the frame's process shards) as a
// compiled `Static_task_graph` - one node per shard, edges derived from the declared access -
// with Macrame's aggregated runtime trace attached. The per-run inputs (which groups each
// shard runs, for which tree, which phase) are plain state the node bodies read: a graph is
// build-once and its bodies take no run arguments. MACRAME_STATIC_GRAPH=0 selects the
// dynamic `ts::async` fan-out instead; MACRAME_TRACE_DIR=<dir> writes the DOT dump at
// compile and the average-run SVGs at shutdown.
struct PhaseGraph {
	ts::Static_task_graph graph;
	ts::tools::Graph_trace trace;
	std::vector<std::vector<void *>> buckets;
	SceneTree *tree = nullptr;
	bool physics = false;
	bool built = false;
	uint64_t runs = 0;
	bool written = false;
	std::string names[MacrameScene::SHARD_COUNT]; // `ts::Named` keeps the pointer: stable storage.
};

struct NavGrantToken {
	int unused = 0; // The navigation server's guarded identity (its update node writes, shards read).
};

// The whole frame as one graph: 32 tick shards, the step, navigation, 32 frame shards.
struct FrameGraph {
	ts::Static_task_graph graph;
	ts::tools::Graph_trace trace;
	std::vector<std::vector<void *>> tick_buckets;
	std::vector<std::vector<void *>> frame_buckets;
	SceneTree *tree = nullptr;
	bool capturing = false;
	bool tick_pending = false;
	double tick_step = 0.0;
	bool built = false;
	uint64_t runs = 0;
	bool written = false;
	std::string tick_names[MacrameScene::SHARD_COUNT];
	std::string frame_names[MacrameScene::SHARD_COUNT];
};

struct State {
	std::vector<std::unique_ptr<ts::Guarded<SceneShardToken>>> shards;
	std::unique_ptr<ts::Guarded<NavGrantToken>> nav_guard;
	FrameGraph frame;
	PhaseGraph physics_graph;
	PhaseGraph process_graph;
	std::vector<SceneShardToken *> shard_ptrs; // Stable addresses of the guarded tokens, for the harness.
	std::unique_ptr<ts::Guarded<SceneShardToken>> main_shard;
	SceneShardToken *main_ptr = nullptr;
	std::atomic<int> next_shard{ 0 };
};
State *state = nullptr;
void _write_phase_trace(PhaseGraph &pg);
void _write_frame_trace(FrameGraph &fg);

} // namespace

thread_local int macrame_tls_current_shard = -1;
thread_local bool macrame_tls_in_shard_task = false;

namespace {
MACRAME_NO_INLINE void set_context(int p_shard, bool p_in_task) {
	macrame_tls_current_shard = p_shard;
	macrame_tls_in_shard_task = p_in_task;
}
} // namespace

void MacrameScene::init() {
	if (state) {
		return;
	}
	state = new State();
	static const char *shard_name = "scene_shard";
	for (int i = 0; i < SHARD_COUNT; i++) {
		state->shards.emplace_back(new ts::Guarded<SceneShardToken>(ts::Named{ shard_name }));
		SceneShardToken *ptr = nullptr;
		state->shards.back()->access([&](SceneShardToken &t) { t.index = i; ptr = &t; }).sync();
		state->shard_ptrs.push_back(ptr);
	}
	state->main_shard.reset(new ts::Guarded<SceneShardToken>(ts::Named{ "scene_main_shard" }));
	state->main_shard->access([&](SceneShardToken &t) { state->main_ptr = &t; }).sync();
	state->nav_guard.reset(new ts::Guarded<NavGrantToken>(ts::Named{ "navigation" }));
	print_verbose(vformat("Macrame: %d scene shards.", SHARD_COUNT));
}

void MacrameScene::finish() {
	if (state) {
		const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
		if (!dir.is_empty()) {
			if (state->frame.built && !state->frame.written) {
				_write_frame_trace(state->frame);
			}
			for (PhaseGraph *pg : { &state->physics_graph, &state->process_graph }) {
				if (pg->built && !pg->written) {
					_write_phase_trace(*pg);
				}
			}
		}
	}
	delete state;
	state = nullptr;
}

bool MacrameScene::is_enabled() {
	return state != nullptr;
}

namespace {
int active_shard_count() {
	// MACRAME_SHARDS=<n> caps how many of the shards receive groups (scaling experiments).
	static int n = [] {
		String v = OS::get_singleton()->get_environment("MACRAME_SHARDS");
		int c = v.is_empty() ? MacrameScene::SHARD_COUNT : v.to_int();
		return CLAMP(c, 1, MacrameScene::SHARD_COUNT);
	}();
	return n;
}
} // namespace

int MacrameScene::assign_shard() {
	return state ? state->next_shard.fetch_add(1) % active_shard_count() : -1;
}

void MacrameScene::check_write(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	const int s = SceneTree::macrame_shard_of(p_node);
	ts::access_check(s < 0 ? state->main_ptr : state->shard_ptrs[s]);
}

void MacrameScene::check_read(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	const int s = SceneTree::macrame_shard_of(p_node);
	const SceneShardToken *ptr = s < 0 ? state->main_ptr : state->shard_ptrs[s];
	ts::access_check(ptr);
}

void MacrameScene::check_main(const Node *p_node) {
	if (!in_shard_task() || !p_node->is_inside_tree()) {
		return;
	}
	ts::access_check(state->main_ptr);
}

namespace {
void _write_phase_trace(PhaseGraph &pg) {
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	if (dir.is_empty() || !pg.built) {
		return;
	}
	const String path = dir + (pg.physics ? "/macrame_physics_phase_avg.svg" : "/macrame_process_phase_avg.svg");
	const bool ok = pg.trace.write_SVG(path.utf8().get_data());
	pg.written = true;
	print_line(vformat("Macrame: trace of %d runs %s -> %s", (int)pg.runs, ok ? "written" : "FAILED", path));
	fflush(stdout);
}

void _build_phase_graph(PhaseGraph &pg, bool p_physics, ts::Guarded<PhysicsGrantToken> *p_space) {
	pg.physics = p_physics;
	pg.buckets.resize(MacrameScene::SHARD_COUNT);
	for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
		pg.names[s] = std::string(p_physics ? "physics shard " : "process shard ") + std::to_string(s);
		auto body = [&pg, s](const MacrameRenderOutputs &p_render_outputs) {
			ScriptServer::thread_enter();
			set_context(s, true);
			MacrameRenderSnapshot::set_current(&p_render_outputs);
			for (void *g : pg.buckets[s]) {
				pg.tree->macrame_process_group(g, pg.physics);
			}
			MacrameRenderSnapshot::set_current(nullptr);
			set_context(-1, false);
		};
		if (p_physics) {
			pg.graph.add_node(ts::Named{ pg.names[s].c_str() },
					[body](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const MacrameRenderOutputs &p_render_outputs) { body(p_render_outputs); },
					*state->shards[s], *state->main_shard, *p_space, MacrameRenderSnapshot::front());
		} else {
			pg.graph.add_node(ts::Named{ pg.names[s].c_str() },
					[body](SceneShardToken &, const SceneShardToken &, const MacrameRenderOutputs &p_render_outputs) { body(p_render_outputs); },
					*state->shards[s], *state->main_shard, MacrameRenderSnapshot::front());
		}
	}
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	const String dot = dir.is_empty() ? String() : dir + (p_physics ? "/macrame_physics_phase.dot" : "/macrame_process_phase.dot");
	pg.graph.compile(dot.is_empty() ? nullptr : dot.utf8().get_data());
	pg.graph.set_trace(&pg.trace);
	pg.built = true;
	print_verbose(vformat("Macrame: %s phase compiled as a static graph (%d nodes).", p_physics ? "physics" : "process", pg.graph.node_count()));
}
} // namespace

void MacrameScene::run_groups(SceneTree *p_tree, void **p_groups, int p_group_count, bool p_physics) {
	ERR_FAIL_NULL(state);
	ts::Guarded<PhysicsGrantToken> *space = MacramePhysics::get_guarded();
	ERR_FAIL_NULL_MSG(space, "Macrame: the physics server did not register its guarded space.");

	// Bucket the groups by shard.
	std::vector<std::vector<void *>> buckets(SHARD_COUNT);
	for (int i = 0; i < p_group_count; i++) {
		const int s = p_tree->macrame_shard_of_group(p_groups[i]);
		if (s < 0) {
			// A sub-thread group with no shard yet: run it on the blue thread.
			p_tree->macrame_process_group(p_groups[i], p_physics);
			continue;
		}
		buckets[s].push_back(p_groups[i]);
	}

	if (state->frame.capturing) {
		// Frame-graph mode: the graph runs this batch; groups without a shard already ran above.
		std::vector<std::vector<void *>> &dst = p_physics ? state->frame.tick_buckets : state->frame.frame_buckets;
		dst.swap(buckets);
		state->frame.tree = p_tree;
		return;
	}

	static const bool use_graph = OS::get_singleton()->get_environment("MACRAME_STATIC_GRAPH") != "0";
	if (use_graph) {
		PhaseGraph &pg = p_physics ? state->physics_graph : state->process_graph;
		if (!pg.built) {
			_build_phase_graph(pg, p_physics, space);
		}
		pg.tree = p_tree;
		pg.physics = p_physics;
		pg.buckets.swap(buckets);
		pg.graph.execute().sync(); // One run at a time; the phases are sequential on the main thread.
		if (++pg.runs == 3000 && !pg.written) {
			// The average run so far, written mid-run (the benchmark harness kills the process; the
			// shutdown path is not reached).
			_write_phase_trace(pg);
		}
		return;
	}

	std::vector<ts::Task<void>> tasks;
	tasks.reserve(SHARD_COUNT);
	for (int s = 0; s < SHARD_COUNT; s++) {
		if (buckets[s].empty()) {
			continue;
		}
		std::vector<void *> groups = buckets[s];
		tasks.push_back(ts::async(
				[p_tree, groups, p_physics, s](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const MacrameRenderOutputs &p_render_outputs) {
					ScriptServer::thread_enter();
					set_context(s, true);
					MacrameRenderSnapshot::set_current(&p_render_outputs);
					for (void *g : groups) {
						p_tree->macrame_process_group(g, p_physics);
					}
					MacrameRenderSnapshot::set_current(nullptr);
					set_context(-1, false);
				},
				*state->shards[s], *state->main_shard, *space, MacrameRenderSnapshot::front()));
	}
	for (ts::Task<void> &t : tasks) {
		t.sync();
	}
}

namespace {

void _write_frame_trace(FrameGraph &fg) {
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	if (dir.is_empty() || !fg.built) {
		return;
	}
	const String path = dir + "/macrame_frame_avg.svg";
	const bool ok = fg.trace.write_SVG(path.utf8().get_data());
	fg.written = true;
	print_line(vformat("Macrame: frame trace of %d runs %s -> %s", (int)fg.runs, ok ? "written" : "FAILED", path));
	fflush(stdout);
}

void _run_bucket(FrameGraph &fg, const std::vector<void *> &p_groups, int p_shard, bool p_physics, const MacrameRenderOutputs &p_outputs) {
	if (p_groups.empty()) {
		return;
	}
	ScriptServer::thread_enter();
	set_context(p_shard, true);
	MacrameRenderSnapshot::set_current(&p_outputs);
	for (void *g : p_groups) {
		fg.tree->macrame_process_group(g, p_physics);
	}
	MacrameRenderSnapshot::set_current(nullptr);
	set_context(-1, false);
}

void _build_frame_graph(FrameGraph &fg, ts::Guarded<PhysicsGrantToken> *p_space) {
	fg.tick_buckets.resize(MacrameScene::SHARD_COUNT);
	fg.frame_buckets.resize(MacrameScene::SHARD_COUNT);
	std::vector<ts::Graph_node> tick_nodes;
	tick_nodes.reserve(MacrameScene::SHARD_COUNT);
	for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
		fg.tick_names[s] = "tick shard " + std::to_string(s);
		tick_nodes.push_back(fg.graph.add_node(ts::Named{ fg.tick_names[s].c_str() },
				[&fg, s](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &, const NavGrantToken &, const MacrameRenderOutputs &p_outputs) {
					if (fg.tick_pending) {
						_run_bucket(fg, fg.tick_buckets[s], s, true, p_outputs);
					}
				},
				*state->shards[s], *state->main_shard, *p_space, *state->nav_guard, MacrameRenderSnapshot::front()));
	}
	// The step writes the space every tick shard read: compile() derives the edges. It also
	// applies the tick's staged body writes first (upstream FIFO semantics).
	fg.graph.add_node("physics step", [&fg](PhysicsGrantToken &) {
		if (fg.tick_pending) {
			static_cast<PhysicsServer3DWrapMT *>(PhysicsServer3D::get_singleton())->macrame_step_under_grant(fg.tick_step);
		}
	}, *p_space).set_priority(ts::Priority::high);
	fg.graph.add_node("navigation", [&fg](NavGrantToken &) {
		if (fg.tick_pending) {
			NavigationServer3D::get_singleton()->physics_process(fg.tick_step);
		}
	}, *state->nav_guard).set_priority(ts::Priority::high);
	for (int s = 0; s < MacrameScene::SHARD_COUNT; s++) {
		fg.frame_names[s] = "frame shard " + std::to_string(s);
		// A shard's process phase follows its own physics phase (the derived write-write edge; the
		// explicit `after` fixes the direction) and nothing else: it overlaps the tick's tail, the
		// step and navigation. Physics queries from _process are refused by the wrapper's guard.
		fg.graph.add_node(ts::Named{ fg.frame_names[s].c_str() },
				[&fg, s](SceneShardToken &, const SceneShardToken &, const MacrameRenderOutputs &p_outputs) {
					_run_bucket(fg, fg.frame_buckets[s], s, false, p_outputs);
				},
				*state->shards[s], *state->main_shard, MacrameRenderSnapshot::front()).after(tick_nodes[s]);
	}
	const String dir = OS::get_singleton()->get_environment("MACRAME_TRACE_DIR");
	const String dot = dir.is_empty() ? String() : dir + "/macrame_frame.dot";
	fg.graph.compile(dot.is_empty() ? nullptr : dot.utf8().get_data());
	fg.graph.set_trace(&fg.trace);
	fg.built = true;
	print_verbose(vformat("Macrame: frame compiled as a static graph (%d nodes).", fg.graph.node_count()));
}

} // namespace

bool MacrameScene::frame_graph_enabled() {
	static const bool enabled = OS::get_singleton()->get_environment("MACRAME_FRAME_GRAPH") != "0";
	return enabled && state != nullptr && MacramePhysics::get_guarded() != nullptr;
}

void MacrameScene::frame_set_capturing(bool p_capturing) {
	ERR_FAIL_NULL(state);
	state->frame.capturing = p_capturing;
}

void MacrameScene::frame_set_tick(double p_step) {
	ERR_FAIL_NULL(state);
	state->frame.tick_pending = true;
	state->frame.tick_step = p_step;
}

void MacrameScene::frame_execute(SceneTree *p_tree) {
	ERR_FAIL_NULL(state);
	FrameGraph &fg = state->frame;
	fg.capturing = false;
	if (!fg.built) {
		_build_frame_graph(fg, MacramePhysics::get_guarded());
	}
	fg.tree = p_tree;
	{
		GodotProfileZone("Macrame: frame graph");
		fg.graph.execute().sync();
	}
	fg.tick_pending = false;
	for (auto &b : fg.tick_buckets) {
		b.clear();
	}
	for (auto &b : fg.frame_buckets) {
		b.clear();
	}
	if (++fg.runs == 3000 && !fg.written) {
		_write_frame_trace(fg);
	}
	p_tree->macrame_post_shards();
}

#else

void MacrameScene::init() {}
void MacrameScene::finish() {}
bool MacrameScene::is_enabled() { return false; }
int MacrameScene::assign_shard() { return -1; }
thread_local int macrame_tls_current_shard = -1;
thread_local bool macrame_tls_in_shard_task = false;
void MacrameScene::check_write(const Node *) {}
void MacrameScene::check_read(const Node *) {}
void MacrameScene::check_main(const Node *) {}
void MacrameScene::run_groups(SceneTree *, void **, int, bool) {}
bool MacrameScene::frame_graph_enabled() { return false; }
void MacrameScene::frame_set_capturing(bool) {}
void MacrameScene::frame_set_tick(double) {}
void MacrameScene::frame_execute(SceneTree *) {}

#endif // MACRAME_ENABLED
