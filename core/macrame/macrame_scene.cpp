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
#include "core/object/script_language.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"

#include "ts/access.h"
#include "ts/guarded.h"
#include "ts/task.h"

#include <atomic>
#include <memory>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define MACRAME_NO_INLINE __declspec(noinline)
#else
#define MACRAME_NO_INLINE [[gnu::noinline]]
#endif

namespace {

struct State {
	std::vector<std::unique_ptr<ts::Guarded<SceneShardToken>>> shards;
	std::vector<SceneShardToken *> shard_ptrs; // Stable addresses of the guarded tokens, for the harness.
	std::unique_ptr<ts::Guarded<SceneShardToken>> main_shard;
	SceneShardToken *main_ptr = nullptr;
	std::atomic<int> next_shard{ 0 };
};
State *state = nullptr;

thread_local int tls_current_shard = -1;
thread_local bool tls_in_shard_task = false;

MACRAME_NO_INLINE void set_context(int p_shard, bool p_in_task) {
	tls_current_shard = p_shard;
	tls_in_shard_task = p_in_task;
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
	print_verbose(vformat("Macrame: %d scene shards.", SHARD_COUNT));
}

void MacrameScene::finish() {
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

MACRAME_NO_INLINE int MacrameScene::current_shard() {
	return tls_current_shard;
}

MACRAME_NO_INLINE bool MacrameScene::in_shard_task() {
	return tls_in_shard_task;
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

	std::vector<ts::Task<void>> tasks;
	tasks.reserve(SHARD_COUNT);
	for (int s = 0; s < SHARD_COUNT; s++) {
		if (buckets[s].empty()) {
			continue;
		}
		std::vector<void *> groups = buckets[s];
		tasks.push_back(ts::async(
				[p_tree, groups, p_physics, s](SceneShardToken &, const SceneShardToken &, const PhysicsGrantToken &) {
					ScriptServer::thread_enter();
					set_context(s, true);
					for (void *g : groups) {
						p_tree->macrame_process_group(g, p_physics);
					}
					set_context(-1, false);
				},
				*state->shards[s], *state->main_shard, *space));
	}
	for (ts::Task<void> &t : tasks) {
		t.sync();
	}
}

#else

void MacrameScene::init() {}
void MacrameScene::finish() {}
bool MacrameScene::is_enabled() { return false; }
int MacrameScene::assign_shard() { return -1; }
int MacrameScene::current_shard() { return -1; }
bool MacrameScene::in_shard_task() { return false; }
void MacrameScene::check_write(const Node *) {}
void MacrameScene::check_read(const Node *) {}
void MacrameScene::check_main(const Node *) {}
void MacrameScene::run_groups(SceneTree *, void **, int, bool) {}

#endif // MACRAME_ENABLED
