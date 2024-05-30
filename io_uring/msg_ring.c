// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/nospec.h>
#include <linux/io_uring.h>

#include <uapi/linux/io_uring.h>

#include "io_uring.h"
#include "rsrc.h"
#include "filetable.h"
#include "alloc_cache.h"
#include "msg_ring.h"


/* All valid masks for MSG_RING */
#define IORING_MSG_RING_MASK		(IORING_MSG_RING_CQE_SKIP | \
					IORING_MSG_RING_FLAGS_PASS)

struct io_msg {
	struct file			*file;
	struct file			*src_file;
	u64 user_data;
	u32 len;
	u32 cmd;
	u32 src_fd;
	union {
		u32 dst_fd;
		u32 cqe_flags;
	};
	u32 flags;
};

static int io_double_lock_ctx(struct io_ring_ctx *octx,
			      unsigned int issue_flags)
{
	/*
	 * To ensure proper ordering between the two ctxs, we can only
	 * attempt a trylock on the target. If that fails and we already have
	 * the source ctx lock, punt to io-wq.
	 */
	if (!(issue_flags & IO_URING_F_UNLOCKED)) {
		if (!mutex_trylock(&octx->uring_lock))
			return -EAGAIN;
		return 0;
	}
	mutex_lock(&octx->uring_lock);
	return 0;
}

void io_msg_ring_cleanup(struct io_kiocb *req)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);

	if (WARN_ON_ONCE(!msg->src_file))
		return;

	fput(msg->src_file);
	msg->src_file = NULL;
}

static struct io_overflow_cqe *io_alloc_overflow(struct io_ring_ctx *target_ctx)
	__acquires(&target_ctx->completion_lock)
{
	struct io_overflow_cqe *ocqe;

	spin_lock(&target_ctx->completion_lock);

	ocqe = io_alloc_cache_get(&target_ctx->msg_cache);
	if (!ocqe) {
		bool is_cqe32 = target_ctx->flags & IORING_SETUP_CQE32;
		size_t cqe_size = sizeof(struct io_overflow_cqe);

		if (is_cqe32)
			cqe_size += sizeof(struct io_uring_cqe);

		ocqe = kmalloc(cqe_size, GFP_ATOMIC | __GFP_ACCOUNT);
		if (!ocqe)
			return NULL;

		/* just init at alloc time, won't change */
		if (is_cqe32)
			ocqe->cqe.big_cqe[0] = ocqe->cqe.big_cqe[1] = 0;
	}

	return ocqe;
}

/*
 * Entered with the target uring_lock held, and will drop it before
 * returning. Adds a previously allocated ocqe to the overflow list on
 * the target, and marks it appropriately for flushing.
 */
static void io_msg_add_overflow(struct io_msg *msg,
				struct io_ring_ctx *target_ctx,
				struct io_overflow_cqe *ocqe, int ret,
				u32 flags)
	__releases(&target_ctx->completion_lock)
{
	unsigned nr_prev;

	if (list_empty(&target_ctx->cq_overflow_list))
		set_bit(IO_CHECK_CQ_OVERFLOW_BIT, &target_ctx->check_cq);

	ocqe->cqe.user_data = msg->user_data;
	ocqe->cqe.res = ret;
	ocqe->cqe.flags = flags;
	nr_prev = target_ctx->nr_overflow++;
	target_ctx->cq_extra++;
	list_add_tail(&ocqe->list, &target_ctx->cq_overflow_list);
	spin_unlock(&target_ctx->completion_lock);

	if (target_ctx->flags & IORING_SETUP_DEFER_TASKRUN) {
		unsigned nr_wait;

		rcu_read_lock();
		io_defer_tw_count(target_ctx, &nr_wait);
		nr_prev += nr_wait;
		io_defer_wake(target_ctx, nr_prev + 1, nr_prev);
		rcu_read_unlock();
	} else if (wq_has_sleeper(&target_ctx->cq_wait)) {
		wake_up(&target_ctx->cq_wait);
	}
}

static int io_msg_fill_remote(struct io_msg *msg, unsigned int issue_flags,
			      struct io_ring_ctx *target_ctx, u32 flags)
{
	struct io_overflow_cqe *ocqe;

	ocqe = io_alloc_overflow(target_ctx);
	if (ocqe) {
		io_msg_add_overflow(msg, target_ctx, ocqe, msg->len, flags);
		return 0;
	}

	spin_unlock(&target_ctx->completion_lock);
	return -ENOMEM;
}

static int io_msg_ring_data(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *target_ctx = req->file->private_data;
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	u32 flags = 0;

	if (msg->src_fd || msg->flags & ~IORING_MSG_RING_FLAGS_PASS)
		return -EINVAL;
	if (!(msg->flags & IORING_MSG_RING_FLAGS_PASS) && msg->dst_fd)
		return -EINVAL;
	if (target_ctx->flags & IORING_SETUP_R_DISABLED)
		return -EBADFD;

	if (msg->flags & IORING_MSG_RING_FLAGS_PASS)
		flags = msg->cqe_flags;

	return io_msg_fill_remote(msg, issue_flags, target_ctx, flags);
}

static struct file *io_msg_grab_file(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct io_ring_ctx *ctx = req->ctx;
	struct file *file = NULL;
	int idx = msg->src_fd;

	io_ring_submit_lock(ctx, issue_flags);
	if (likely(idx < ctx->nr_user_files)) {
		idx = array_index_nospec(idx, ctx->nr_user_files);
		file = io_file_from_index(&ctx->file_table, idx);
		if (file)
			get_file(file);
	}
	io_ring_submit_unlock(ctx, issue_flags);
	return file;
}

static int io_msg_install_remote(struct io_kiocb *req, unsigned int issue_flags,
				 struct io_ring_ctx *target_ctx)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct io_overflow_cqe *ocqe = NULL;
	int ret = -ENOMEM;

	if (unlikely(io_double_lock_ctx(target_ctx, issue_flags)))
		return -EAGAIN;

	if (!(msg->flags & IORING_MSG_RING_CQE_SKIP)) {
		ocqe = io_alloc_overflow(target_ctx);
		if (unlikely(!ocqe)) {
			mutex_unlock(&target_ctx->uring_lock);
			goto err;
		}
	}

	ret = __io_fixed_fd_install(target_ctx, msg->src_file, msg->dst_fd);
	mutex_unlock(&target_ctx->uring_lock);

	if (ret >= 0) {
		msg->src_file = NULL;
		req->flags &= ~REQ_F_NEED_CLEANUP;
		if (ocqe) {
			io_msg_add_overflow(msg, target_ctx, ocqe, ret, 0);
			return 0;
		}
	}
	if (ocqe) {
err:
		spin_unlock(&target_ctx->completion_lock);
		kfree(ocqe);
	}
	return ret;
}

static int io_msg_send_fd(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_ring_ctx *target_ctx = req->file->private_data;
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	struct io_ring_ctx *ctx = req->ctx;
	struct file *src_file = msg->src_file;

	if (msg->len)
		return -EINVAL;
	if (target_ctx == ctx)
		return -EINVAL;
	if (target_ctx->flags & IORING_SETUP_R_DISABLED)
		return -EBADFD;
	if (!src_file) {
		src_file = io_msg_grab_file(req, issue_flags);
		if (!src_file)
			return -EBADF;
		msg->src_file = src_file;
		req->flags |= REQ_F_NEED_CLEANUP;
	}

	return io_msg_install_remote(req, issue_flags, target_ctx);
}

int io_msg_ring_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);

	if (unlikely(sqe->buf_index || sqe->personality))
		return -EINVAL;

	msg->src_file = NULL;
	msg->user_data = READ_ONCE(sqe->off);
	msg->len = READ_ONCE(sqe->len);
	msg->cmd = READ_ONCE(sqe->addr);
	msg->src_fd = READ_ONCE(sqe->addr3);
	msg->dst_fd = READ_ONCE(sqe->file_index);
	msg->flags = READ_ONCE(sqe->msg_ring_flags);
	if (msg->flags & ~IORING_MSG_RING_MASK)
		return -EINVAL;

	return 0;
}

int io_msg_ring(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_msg *msg = io_kiocb_to_cmd(req, struct io_msg);
	int ret;

	ret = -EBADFD;
	if (!io_is_uring_fops(req->file))
		goto done;

	switch (msg->cmd) {
	case IORING_MSG_DATA:
		ret = io_msg_ring_data(req, issue_flags);
		break;
	case IORING_MSG_SEND_FD:
		ret = io_msg_send_fd(req, issue_flags);
		break;
	default:
		ret = -EINVAL;
		break;
	}

done:
	if (ret < 0) {
		if (ret == -EAGAIN || ret == IOU_ISSUE_SKIP_COMPLETE)
			return ret;
		req_set_fail(req);
	}
	io_req_set_res(req, ret, 0);
	return IOU_OK;
}

int io_msg_cache_init(struct io_ring_ctx *ctx)
{
	size_t size = sizeof(struct io_overflow_cqe);

	if (ctx->flags & IORING_SETUP_CQE32)
		size += sizeof(struct io_uring_cqe);

	return io_alloc_cache_init(&ctx->msg_cache, IO_ALLOC_CACHE_MAX, size);
}

void io_msg_cache_free(struct io_ring_ctx *ctx)
{
	io_alloc_cache_free(&ctx->msg_cache, kfree);
}
