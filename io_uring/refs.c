// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/percpu.h>
#include <linux/io_uring.h>

#include "refs.h"

int io_ring_ref_init(struct io_ring_ctx *ctx)
{
	size_t align = max_t(size_t, 1 << __PERCPU_REF_FLAG_BITS,
				__alignof__(local_t));

	ctx->ref_ptr = (unsigned long) __alloc_percpu(sizeof(local_t), align);
	if (ctx->ref_ptr)
		return 0;

	return -ENOMEM;
}

void io_ring_ref_free(struct io_ring_ctx *ctx)
{
	local_t __percpu *refs = io_ring_ref(ctx);

	free_percpu(refs);
	ctx->ref_ptr = 0;
}

/*
 * Checks if all references are gone, completes if so.
 */
void __cold io_ring_ref_maybe_done(struct io_ring_ctx *ctx)
{
	local_t __percpu *refs = io_ring_ref(ctx);
	long sum = 0;
	int cpu;

	preempt_disable();
	for_each_possible_cpu(cpu)
		sum += local_read(per_cpu_ptr(refs, cpu));
	preempt_enable();

	if (!sum)
		complete(&ctx->ref_comp);
}

/*
 * Mark the reference killed. This grabs a reference which the caller must
 * drop.
 */
void io_ring_ref_kill(struct io_ring_ctx *ctx)
{
	io_ring_ref_get(ctx);
	set_bit(CTX_REF_DEAD_BIT, &ctx->ref_ptr);
	io_ring_ref_maybe_done(ctx);
}
