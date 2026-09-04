/**************************************************************************/
/*  macrame_scene.h                                                       */
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

// Phase 2b of the Macrame conversion: scene shards.
//
// The scene tree is cut into a fixed number of shards. Every process group that a node
// opts into with `process_thread_group = SUB_THREAD` is assigned to one shard (round
// robin); everything else belongs to the main shard, which only the blue thread runs.
// A shard is one `Guarded` object. Processing a shard is one task with a write grant on
// that shard and read grants on the main shard and on the physics space, so shards run
// in parallel with no locks: by declaration they cannot touch each other's nodes.
//
// The harness sits at the node guard macros (`scene/main/node.h`): inside a shard task a
// node write checks the write grant on the node's shard, a node read checks a read grant,
// and a main-thread-only method checks a write grant on the main shard, which a shard
// never holds. Outside shard tasks (the blue thread, single-threaded by construction)
// the checks are skipped. Cross-shard writes go through the existing deferred paths
// (`call_deferred_thread_group`, `set_deferred_thread_group`); cross-shard reads fault.

class Node;

extern thread_local int macrame_tls_current_shard;
extern thread_local bool macrame_tls_in_shard_task;
class SceneTree;

struct SceneShardToken {
	int index = -1; // -1 is the main shard.
};

namespace MacrameScene {
constexpr int SHARD_COUNT = 32; // Fixed. MACRAME_SHARDS caps how many receive groups. Measured at 500 NPCs on a 22-thread laptop: 16 and 32 within noise, 64 worse (per-call cost grows with concurrency).

void init();
void finish();
bool is_enabled();

// Round-robin shard assignment for a new sub-thread process group.
int assign_shard();

// Thread-local task context (out-of-line accessors, see macrame_render_grant.h).
inline int current_shard() {
	return macrame_tls_current_shard; // -1 when not inside a shard task.
}
inline bool in_shard_task() {
	return macrame_tls_in_shard_task;
}

// Harness entry points used by the node guard macros.
void check_write(const Node *p_node);
void check_read(const Node *p_node);
void check_main(const Node *p_node);

// Run every group in `p_groups` (a null-terminated array is not used; count given) as
// shard tasks and join them. Called from SceneTree::_process for a threaded batch.
void run_groups(SceneTree *p_tree, void **p_groups, int p_group_count, bool p_physics);
} // namespace MacrameScene
