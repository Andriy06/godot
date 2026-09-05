/**************************************************************************/
/*  rendering_device_submit.h                                             */
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

#include "servers/rendering/rendering_device_graph.h"

#include <atomic>

#ifdef MACRAME_ENABLED
#include "ts/access.h"
#endif

class RenderingDevice;

// The submission half of the rendering device, as its own object.
//
// `RenderingDevice` is one pile of members that a recording pass and a submitting pass both used
// to touch. With the split draw those two passes are two tasks on two guarded objects, so the
// state has to be split as well or the harness cannot tell them apart: this class holds
// everything `_end_frame` / `_execute_frame` / present touch, it is the payload of the device
// `Guarded` (not a token), and every method opens with `TS_CHECK_ACCESS()`. A caller without the
// device grant - the render task, a stray blue thread - faults on the first method, naming this
// object and the mode it lacked.
//
// The recording side keeps its own state in `RenderingDevice` (the `frames` ring's recording
// fields, the graphs, the open draw/compute lists, the `frame` slot index) under the render
// grant, and reaches this object only through `Staged`: one value, produced by the render task
// at the hand-off and consumed by the device task. Nothing here reads a recording member, and
// the recording side never writes a member of this class outside `configure()` /
// `create_slots()`, which run at initialization before any device task exists.
class RenderingDeviceSubmit {
public:
	using RDD = RenderingDeviceDriver;
	using RDG = RenderingDeviceGraph;

	// The hand-off. Everything the device task needs that the recording pass produced, by value:
	// which slot, which graph, the command buffer that was opened for it, the fence it must
	// signal, and the swap chains the frame acquired and must present. The record lives in a ring
	// indexed by frame slot on the recording side; a slot's record is only rewritten when that
	// slot is reopened for recording, which is after its device task has been joined.
	struct Staged {
		uint32_t slot = 0;
		RenderingDeviceGraph *graph = nullptr;
		RDD::CommandBufferID command_buffer;
		RDD::FenceID fence;
		// The recording side's "a submission is outstanding on this slot" flag; set once the
		// submission has actually been handed to the queue. The only thing this side writes that
		// the other side owns, and it is one word carried in the value rather than a member
		// reached through a back-pointer.
		std::atomic<bool> *fence_signaled = nullptr;
		LocalVector<RDD::SwapChainID> swap_chains_to_present;
		bool present = false;
		bool reorder_commands = true;
		bool full_barriers = false;
	};

	// Set up once, from the thread that initializes the device, under this object's grant.
	void configure(RenderingDevice *p_device, RDD *p_driver, RDD::CommandQueueID p_main_queue, RDD::CommandQueueID p_present_queue);
	// One record per frame slot: the slot's secondary command buffer pool, the semaphore a
	// separate present queue would wait on, and the transfer workers' per-frame semaphores.
	bool create_slots(uint32_t p_slot_count, const LocalVector<RDD::CommandPoolID> &p_command_pools, uint32_t p_transfer_worker_count);
	void destroy_slots();

	// Replay the staged graph into the staged command buffer, submit it, present what it
	// acquired. This is the whole device task.
	void submit(Staged &p_staged);

	// Reported to the diagnostics that ask whether the split is set up at all.
	bool is_configured() const;

private:
	struct Slot {
		// Extra command buffers the graph replay may split the frame into (driver workarounds,
		// the swap-chain pass). Only the replay and the submit touch them.
		RDG::CommandBufferPool command_buffer_pool;
		// Signaled by the command buffer submission; a separate presentation queue waits on it.
		RDD::SemaphoreID semaphore;
		// Semaphores the transfer workers signal so this frame's command buffer can wait on them.
		TightLocalVector<RDD::SemaphoreID> transfer_worker_semaphores;
	};

	void _end_frame(Staged &p_staged);
	void _execute_frame(Staged &p_staged);
	void _execute_chained_cmds(Staged &p_staged, bool p_present_swap_chain, RDD::FenceID p_draw_fence, RDD::SemaphoreID p_dst_draw_semaphore_to_signal);

	RenderingDevice *device = nullptr;
	RDD *driver = nullptr;
	RDD::CommandQueueID main_queue;
	RDD::CommandQueueID present_queue;
	LocalVector<Slot> slots;

	// Scratch, owned by this object: the semaphores this submission must wait on (the transfer
	// workers add them while the frame is being closed) and the chained-submit working vectors.
	// Upstream kept the wait list in the `frames` ring, where the recording pass indexed it with
	// the recording slot while the submit read the submitting slot - with two frames live those
	// are different slots and the wait went to the wrong frame. It is device-side state, so it
	// lives here.
	LocalVector<RDD::SemaphoreID> wait_semaphores;
	LocalVector<RDD::SemaphoreID> chained_wait_semaphores;
	LocalVector<RDD::SwapChainID> chained_swap_chains;
};

#ifdef MACRAME_ENABLED
// Installs the harness hook `MacrameRenderDevice::check_access(instance)` calls: it faults unless
// the running task holds the device grant on that very submit object. Stateless, so every device
// (the main one and any local device, each with its own submit object and its own guarded queue)
// registers the same checker. See `RenderingDeviceGraph`'s owner check.
void macrame_register_device_submit_checker();
#endif
