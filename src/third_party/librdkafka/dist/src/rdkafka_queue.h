/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2016-2022, Magnus Edenhill,
 *               2023, Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDKAFKA_QUEUE_H_
#define _RDKAFKA_QUEUE_H_

#include "rdkafka_op.h"
#include "rdkafka_int.h"

#ifdef _WIN32
#include <io.h> /* for _write() */
#endif

/** @brief Queueing strategy */
#define RD_KAFKA_QUEUE_FIFO 0
#define RD_KAFKA_QUEUE_LIFO 1

TAILQ_HEAD(rd_kafka_op_tailq, rd_kafka_op_s);

/**
 * @struct Queue for rd_kafka_op_t*.
 *
 * @remark All readers of the queue must call rd_kafka_q_mark_served()
 *         after reading the queue (while still holding the queue lock) to
 *         clear the wakeup-sent flag.
 */
struct rd_kafka_q_s {
        mtx_t rkq_lock;
        cnd_t rkq_cond;
        struct rd_kafka_q_s *rkq_fwdq; /* Forwarded/Routed queue.
                                        * Used in place of this queue
                                        * for all operations. */

        struct rd_kafka_op_tailq rkq_q; /* TAILQ_HEAD(, rd_kafka_op_s) */
        int rkq_qlen;                   /* Number of entries in queue */
        int64_t rkq_qsize;              /* Size of all entries in queue */
        int rkq_refcnt;
        int rkq_flags;
#define RD_KAFKA_Q_F_ALLOCATED 0x1 /* Allocated: rd_free on destroy */
#define RD_KAFKA_Q_F_READY                                                     \
        0x2 /* Queue is ready to be used.                                      \
             * Flag is cleared on destroy */
#define RD_KAFKA_Q_F_FWD_APP                                                   \
        0x4 /* Queue is being forwarded by a call                              \
             * to rd_kafka_queue_forward. */
#define RD_KAFKA_Q_F_YIELD                                                     \
        0x8 /* Have waiters return even if                                     \
             * no rko was enqueued.                                            \
             * This is used to wake up a waiter                                \
             * by triggering the cond-var                                      \
             * but without having to enqueue                                   \
             * an op. */
#define RD_KAFKA_Q_F_CONSUMER                                                  \
        0x10 /* If this flag is set, this queue might contain fetched messages \
                from partitions. Polling this queue will reset the             \
                max.poll.interval.ms timer. Once set, this flag is never       \
                reset. */

        rd_kafka_t *rkq_rk;
        struct rd_kafka_q_io *rkq_qio; /* FD-based application signalling */

        /* Op serve callback (optional).
         * Mainly used for forwarded queues to use the original queue's
         * serve function from the forwarded position.
         * Shall return 1 if op was handled, else 0. */
        rd_kafka_q_serve_cb_t *rkq_serve;
        void *rkq_opaque;

#if ENABLE_DEVEL
        char rkq_name[64]; /* Debugging: queue name (FUNC:LINE) */
#else
        const char *rkq_name; /* Debugging: queue name (FUNC) */
#endif
};


/* Application signalling state holder. */
struct rd_kafka_q_io {
        /* For FD-based signalling */
        rd_socket_t fd;
        void *payload;
        size_t size;
        rd_bool_t sent; /**< Wake-up has been sent.
                         *   This field is reset to false by the queue
                         *   reader, allowing a new wake-up to be sent by a
                         *   subsequent writer. */
        /* For callback-based signalling */
        void (*event_cb)(rd_kafka_t *rk, void *opaque);
        void *event_cb_opaque;
};



/**
 * @return true if queue is ready/enabled, else false.
 * @remark queue luck must be held by caller (if applicable)
 */
static RD_INLINE RD_UNUSED int rd_kafka_q_ready(rd_kafka_q_t *rkq) {
        return rkq->rkq_flags & RD_KAFKA_Q_F_READY;
}



void rd_kafka_q_init0(rd_kafka_q_t *rkq,
                      rd_kafka_t *rk,
                      rd_bool_t for_consume,
                      const char *func,
                      int line);
#define rd_kafka_q_init(rkq, rk)                                               \
        rd_kafka_q_init0(rkq, rk, rd_false, __FUNCTION__, __LINE__)
#define rd_kafka_consume_q_init(rkq, rk)                                       \
        rd_kafka_q_init0(rkq, rk, rd_true, __FUNCTION__, __LINE__)
rd_kafka_q_t *rd_kafka_q_new0(rd_kafka_t *rk,
                              rd_bool_t for_consume,
                              const char *func,
                              int line);
#define rd_kafka_q_new(rk) rd_kafka_q_new0(rk, rd_false, __FUNCTION__, __LINE__)
#define rd_kafka_consume_q_new(rk)                                             \
        rd_kafka_q_new0(rk, rd_true, __FUNCTION__, __LINE__)
void rd_kafka_q_destroy_final(rd_kafka_q_t *rkq);

#define rd_kafka_q_lock(rkqu)   mtx_lock(&(rkqu)->rkq_lock)
#define rd_kafka_q_unlock(rkqu) mtx_unlock(&(rkqu)->rkq_lock)

static RD_INLINE RD_UNUSED rd_kafka_q_t *rd_kafka_q_keep(rd_kafka_q_t *rkq) {
        mtx_lock(&rkq->rkq_lock);
        rkq->rkq_refcnt++;
        mtx_unlock(&rkq->rkq_lock);
        return rkq;
}

static RD_INLINE RD_UNUSED rd_kafka_q_t *
rd_kafka_q_keep_nolock(rd_kafka_q_t *rkq) {
        rkq->rkq_refcnt++;
        return rkq;
}


/**
 * @returns the queue's name (used for debugging)
 */
static RD_INLINE RD_UNUSED const char *rd_kafka_q_name(rd_kafka_q_t *rkq) {
        return rkq->rkq_name;
}

/**
 * @returns the final destination queue name (after forwarding)
 * @remark rkq MUST NOT be locked
 */
static RD_INLINE RD_UNUSED const char *rd_kafka_q_dest_name(rd_kafka_q_t *rkq) {
        const char *ret;
        mtx_lock(&rkq->rkq_lock);
        if (rkq->rkq_fwdq)
                ret = rd_kafka_q_dest_name(rkq->rkq_fwdq);
        else
                ret = rd_kafka_q_name(rkq);
        mtx_unlock(&rkq->rkq_lock);
        return ret;
}

/**
 * @brief Disable a queue.
 *        Attempting to enqueue ops to the queue will destroy the ops.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_disable0(rd_kafka_q_t *rkq,
                                                    int do_lock) {
        if (do_lock)
                mtx_lock(&rkq->rkq_lock);
        rkq->rkq_flags &= ~RD_KAFKA_Q_F_READY;
        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);
}
#define rd_kafka_q_disable(rkq) rd_kafka_q_disable0(rkq, 1 /*lock*/)

int rd_kafka_q_purge0(rd_kafka_q_t *rkq, int do_lock);
#define rd_kafka_q_purge(rkq) rd_kafka_q_purge0(rkq, 1 /*lock*/)
void rd_kafka_q_purge_toppar_version(rd_kafka_q_t *rkq,
                                     rd_kafka_toppar_t *rktp,
                                     int version);

/**
 * @brief Loose reference to queue, when refcount reaches 0 the queue
 *        will be destroyed.
 *
 * @param disable Also disable the queue, to be used by owner of the queue.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_destroy0(rd_kafka_q_t *rkq,
                                                    int disable) {
        int do_delete = 0;

        if (disable) {
                /* To avoid recursive locking (from ops being purged
                 * that reference this queue somehow),
                 * we disable the queue and purge it with individual
                 * locking. */
                rd_kafka_q_disable0(rkq, 1 /*lock*/);
                rd_kafka_q_purge0(rkq, 1 /*lock*/);
        }

        mtx_lock(&rkq->rkq_lock);
        rd_kafka_assert(NULL, rkq->rkq_refcnt > 0);
        do_delete = !--rkq->rkq_refcnt;
        mtx_unlock(&rkq->rkq_lock);

        if (unlikely(do_delete))
                rd_kafka_q_destroy_final(rkq);
}

#define rd_kafka_q_destroy(rkq) rd_kafka_q_destroy0(rkq, 0 /*dont-disable*/)

/**
 * @brief Queue destroy method to be used by the owner (poller) of
 *        the queue. The only difference to q_destroy() is that this
 *        method also disables the queue so that any q_enq() operations
 *        will fail.
 *        Failure to disable a queue on the poller when it destroys its
 *        queue reference results in ops being enqueued on the queue
 *        but there is noone left to poll it, possibly resulting in a
 *        hang on termination due to refcounts held by the op.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_destroy_owner(rd_kafka_q_t *rkq) {
        rd_kafka_q_destroy0(rkq, 1 /*disable*/);
}


/**
 * Reset a queue.
 * WARNING: All messages will be lost and leaked.
 * NOTE: No locking is performed.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_reset(rd_kafka_q_t *rkq) {
        TAILQ_INIT(&rkq->rkq_q);
        rd_dassert(TAILQ_EMPTY(&rkq->rkq_q));
        rkq->rkq_qlen  = 0;
        rkq->rkq_qsize = 0;
}



/**
 * Forward 'srcq' to 'destq'
 */
void rd_kafka_q_fwd_set0(rd_kafka_q_t *srcq,
                         rd_kafka_q_t *destq,
                         int do_lock,
                         int fwd_app);
#define rd_kafka_q_fwd_set(S, D)                                               \
        rd_kafka_q_fwd_set0(S, D, 1 /*lock*/, 0 /*no fwd_app*/)

/**
 * @returns the forward queue (if any) with its refcount increased.
 * @locks rd_kafka_q_lock(rkq) == !do_lock
 */
static RD_INLINE RD_UNUSED rd_kafka_q_t *rd_kafka_q_fwd_get(rd_kafka_q_t *rkq,
                                                            int do_lock) {
        rd_kafka_q_t *fwdq;
        if (do_lock)
                mtx_lock(&rkq->rkq_lock);

        if ((fwdq = rkq->rkq_fwdq))
                rd_kafka_q_keep(fwdq);

        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);

        return fwdq;
}


/**
 * @returns true if queue is forwarded, else false.
 *
 * @remark Thread-safe.
 */
static RD_INLINE RD_UNUSED int rd_kafka_q_is_fwded(rd_kafka_q_t *rkq) {
        int r;
        mtx_lock(&rkq->rkq_lock);
        r = rkq->rkq_fwdq ? 1 : 0;
        mtx_unlock(&rkq->rkq_lock);
        return r;
}



/**
 * @brief Trigger an IO event for this queue.
 *
 * @remark Queue MUST be locked
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_io_event(rd_kafka_q_t *rkq) {

        if (likely(!rkq->rkq_qio))
                return;

        if (rkq->rkq_qio->event_cb) {
                rkq->rkq_qio->event_cb(rkq->rkq_rk,
                                       rkq->rkq_qio->event_cb_opaque);
                return;
        }


        /* Only one wake-up event should be sent per non-polling period.
         * As the queue reader calls poll/reads the channel it calls to
         * rd_kafka_q_mark_served() to reset the wakeup sent flag, allowing
         * further wakeups in the next non-polling period. */
        if (rkq->rkq_qio->sent)
                return; /* Wake-up event already written */

        rkq->rkq_qio->sent = rd_true;

        /* Write wake-up event to socket.
         * Ignore errors, not much to do anyway. */
        if (rd_socket_write(rkq->rkq_qio->fd, rkq->rkq_qio->payload,
                            (int)rkq->rkq_qio->size) == -1)
                ;
}


/**
 * @brief rko->rko_prio comparator
 * @remark: descending order: higher priority takes preceedence.
 */
static RD_INLINE RD_UNUSED int rd_kafka_op_cmp_prio(const void *_a,
                                                    const void *_b) {
        const rd_kafka_op_t *a = _a, *b = _b;

        return RD_CMP(b->rko_prio, a->rko_prio);
}


/**
 * @brief Wake up waiters without enqueuing an op.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_yield(rd_kafka_q_t *rkq) {
        rd_kafka_q_t *fwdq;

        mtx_lock(&rkq->rkq_lock);

        rd_dassert(rkq->rkq_refcnt > 0);

        if (unlikely(!(rkq->rkq_flags & RD_KAFKA_Q_F_READY))) {
                /* Queue has been disabled */
                mtx_unlock(&rkq->rkq_lock);
                return;
        }

        if (!(fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                rkq->rkq_flags |= RD_KAFKA_Q_F_YIELD;
                cnd_broadcast(&rkq->rkq_cond);
                if (rkq->rkq_qlen == 0)
                        rd_kafka_q_io_event(rkq);

                mtx_unlock(&rkq->rkq_lock);
        } else {
                mtx_unlock(&rkq->rkq_lock);
                rd_kafka_q_yield(fwdq);
                rd_kafka_q_destroy(fwdq);
        }
}

/**
 * @brief Low-level unprotected enqueue that only performs
 *        the actual queue enqueue and counter updates.
 * @remark Will not perform locking, signaling, fwdq, READY checking, etc.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_q_enq0(rd_kafka_q_t *rkq, rd_kafka_op_t *rko, int at_head) {
        if (likely(!rko->rko_prio))
                TAILQ_INSERT_TAIL(&rkq->rkq_q, rko, rko_link);
        else if (at_head)
                TAILQ_INSERT_HEAD(&rkq->rkq_q, rko, rko_link);
        else
                TAILQ_INSERT_SORTED(&rkq->rkq_q, rko, rd_kafka_op_t *, rko_link,
                                    rd_kafka_op_cmp_prio);
        rkq->rkq_qlen++;
        rkq->rkq_qsize += rko->rko_len;
}


/**
 * @brief Enqueue \p rko either at head or tail of \p rkq.
 *
 * The provided \p rko is either enqueued or destroyed.
 *
 * \p orig_destq is the original (outermost) dest queue for which
 * this op was enqueued, before any queue forwarding has kicked in.
 * The rko_serve callback from the orig_destq will be set on the rko
 * if there is no rko_serve callback already set, and the \p rko isn't
 * failed because the final queue is disabled.
 *
 * @returns 1 if op was enqueued or 0 if queue is disabled and
 * there was no replyq to enqueue on in which case the rko is destroyed.
 *
 * @locality any thread.
 */
static RD_INLINE RD_UNUSED int rd_kafka_q_enq1(rd_kafka_q_t *rkq,
                                               rd_kafka_op_t *rko,
                                               rd_kafka_q_t *orig_destq,
                                               int at_head,
                                               int do_lock) {
        rd_kafka_q_t *fwdq;

        if (do_lock)
                mtx_lock(&rkq->rkq_lock);

        rd_dassert(rkq->rkq_refcnt > 0);

        if (unlikely(!(rkq->rkq_flags & RD_KAFKA_Q_F_READY))) {
                /* Queue has been disabled, reply to and fail the rko. */
                if (do_lock)
                        mtx_unlock(&rkq->rkq_lock);

                return rd_kafka_op_reply(rko, RD_KAFKA_RESP_ERR__DESTROY);
        }

        if (!(fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                if (!rko->rko_serve && orig_destq->rkq_serve) {
                        /* Store original queue's serve callback and opaque
                         * prior to forwarding. */
                        rko->rko_serve        = orig_destq->rkq_serve;
                        rko->rko_serve_opaque = orig_destq->rkq_opaque;
                }

                rd_kafka_q_enq0(rkq, rko, at_head);
                cnd_signal(&rkq->rkq_cond);
                if (rkq->rkq_qlen == 1)
                        rd_kafka_q_io_event(rkq);

                if (do_lock)
                        mtx_unlock(&rkq->rkq_lock);
        } else {
                if (do_lock)
                        mtx_unlock(&rkq->rkq_lock);
                rd_kafka_q_enq1(fwdq, rko, orig_destq, at_head, 1 /*do lock*/);
                rd_kafka_q_destroy(fwdq);
        }

        return 1;
}

/**
 * @brief Enqueue the 'rko' op at the tail of the queue 'rkq'.
 *
 * The provided 'rko' is either enqueued or destroyed.
 *
 * @returns 1 if op was enqueued or 0 if queue is disabled and
 * there was no replyq to enqueue on in which case the rko is destroyed.
 *
 * @locality any thread.
 * @locks rkq MUST NOT be locked
 */
static RD_INLINE RD_UNUSED int rd_kafka_q_enq(rd_kafka_q_t *rkq,
                                              rd_kafka_op_t *rko) {
        return rd_kafka_q_enq1(rkq, rko, rkq, 0 /*at tail*/, 1 /*do lock*/);
}


/**
 * @brief Re-enqueue rko at head of rkq.
 *
 * The provided 'rko' is either enqueued or destroyed.
 *
 * @returns 1 if op was enqueued or 0 if queue is disabled and
 * there was no replyq to enqueue on in which case the rko is destroyed.
 *
 * @locality any thread
 * @locks rkq MUST BE locked
 */
static RD_INLINE RD_UNUSED int rd_kafka_q_reenq(rd_kafka_q_t *rkq,
                                                rd_kafka_op_t *rko) {
        return rd_kafka_q_enq1(rkq, rko, rkq, 1 /*at head*/, 0 /*don't lock*/);
}


/**
 * Dequeue 'rko' from queue 'rkq'.
 *
 * NOTE: rkq_lock MUST be held
 * Locality: any thread
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_deq0(rd_kafka_q_t *rkq,
                                                rd_kafka_op_t *rko) {
        rd_dassert(rkq->rkq_qlen > 0 &&
                   rkq->rkq_qsize >= (int64_t)rko->rko_len);

        TAILQ_REMOVE(&rkq->rkq_q, rko, rko_link);
        rkq->rkq_qlen--;
        rkq->rkq_qsize -= rko->rko_len;
}


/**
 * @brief Mark queue as served / read.
 *
 * This is currently used by the queue reader side to reset the io-event
 * wakeup flag.
 *
 * Should be called by all queue readers.
 *
 * @locks_required rkq must be locked.
 */
static RD_INLINE RD_UNUSED void rd_kafka_q_mark_served(rd_kafka_q_t *rkq) {
        if (rkq->rkq_qio)
                rkq->rkq_qio->sent = rd_false;
}


/**
 * Concat all elements of 'srcq' onto tail of 'rkq'.
 * 'rkq' will be be locked (if 'do_lock'==1), but 'srcq' will not.
 * NOTE: 'srcq' will be reset.
 *
 * Locality: any thread.
 *
 * @returns 0 if operation was performed or -1 if rkq is disabled.
 */
static RD_INLINE RD_UNUSED int
rd_kafka_q_concat0(rd_kafka_q_t *rkq, rd_kafka_q_t *srcq, int do_lock) {
        int r = 0;

        while (srcq->rkq_fwdq) /* Resolve source queue */
                srcq = srcq->rkq_fwdq;
        if (unlikely(srcq->rkq_qlen == 0))
                return 0; /* Don't do anything if source queue is empty */

        if (do_lock)
                mtx_lock(&rkq->rkq_lock);
        if (!rkq->rkq_fwdq) {
                rd_kafka_op_t *rko;

                rd_dassert(TAILQ_EMPTY(&srcq->rkq_q) || srcq->rkq_qlen > 0);
                if (unlikely(!(rkq->rkq_flags & RD_KAFKA_Q_F_READY))) {
                        if (do_lock)
                                mtx_unlock(&rkq->rkq_lock);
                        return -1;
                }
                /* First insert any prioritized ops from srcq
                 * in the right position in rkq. */
                while ((rko = TAILQ_FIRST(&srcq->rkq_q)) && rko->rko_prio > 0) {
                        TAILQ_REMOVE(&srcq->rkq_q, rko, rko_link);
                        TAILQ_INSERT_SORTED(&rkq->rkq_q, rko, rd_kafka_op_t *,
                                            rko_link, rd_kafka_op_cmp_prio);
                }

                TAILQ_CONCAT(&rkq->rkq_q, &srcq->rkq_q, rko_link);
                if (rkq->rkq_qlen == 0)
                        rd_kafka_q_io_event(rkq);
                rkq->rkq_qlen += srcq->rkq_qlen;
                rkq->rkq_qsize += srcq->rkq_qsize;
                cnd_signal(&rkq->rkq_cond);

                rd_kafka_q_mark_served(srcq);
                rd_kafka_q_reset(srcq);
        } else
                r = rd_kafka_q_concat0(rkq->rkq_fwdq ? rkq->rkq_fwdq : rkq,
                                       srcq, rkq->rkq_fwdq ? do_lock : 0);
        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);

        return r;
}

#define rd_kafka_q_concat(dstq, srcq) rd_kafka_q_concat0(dstq, srcq, 1 /*lock*/)


/**
 * @brief Prepend all elements of 'srcq' onto head of 'rkq'.
 * 'rkq' will be be locked (if 'do_lock'==1), but 'srcq' will not.
 * 'srcq' will be reset.
 *
 * @remark Will not respect priority of ops, srcq will be prepended in its
 *         original form to rkq.
 *
 * @locality any thread.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_q_prepend0(rd_kafka_q_t *rkq, rd_kafka_q_t *srcq, int do_lock) {
        if (do_lock)
                mtx_lock(&rkq->rkq_lock);
        if (!rkq->rkq_fwdq && !srcq->rkq_fwdq) {
                /* FIXME: prio-aware */
                /* Concat rkq on srcq */
                TAILQ_CONCAT(&srcq->rkq_q, &rkq->rkq_q, rko_link);
                /* Move srcq to rkq */
                TAILQ_MOVE(&rkq->rkq_q, &srcq->rkq_q, rko_link);
                if (rkq->rkq_qlen == 0 && srcq->rkq_qlen > 0)
                        rd_kafka_q_io_event(rkq);
                rkq->rkq_qlen += srcq->rkq_qlen;
                rkq->rkq_qsize += srcq->rkq_qsize;

                rd_kafka_q_mark_served(srcq);
                rd_kafka_q_reset(srcq);
        } else
                rd_kafka_q_prepend0(rkq->rkq_fwdq ? rkq->rkq_fwdq : rkq,
                                    srcq->rkq_fwdq ? srcq->rkq_fwdq : srcq,
                                    rkq->rkq_fwdq ? do_lock : 0);
        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);
}

#define rd_kafka_q_prepend(dstq, srcq)                                         \
        rd_kafka_q_prepend0(dstq, srcq, 1 /*lock*/)


/* Returns the number of elements in the queue */
static RD_INLINE RD_UNUSED int rd_kafka_q_len(rd_kafka_q_t *rkq) {
        int qlen;
        rd_kafka_q_t *fwdq;
        mtx_lock(&rkq->rkq_lock);
        if (!(fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                qlen = rkq->rkq_qlen;
                mtx_unlock(&rkq->rkq_lock);
        } else {
                mtx_unlock(&rkq->rkq_lock);
                qlen = rd_kafka_q_len(fwdq);
                rd_kafka_q_destroy(fwdq);
        }
        return qlen;
}

/* Returns the total size of elements in the queue */
static RD_INLINE RD_UNUSED uint64_t rd_kafka_q_size(rd_kafka_q_t *rkq) {
        uint64_t sz;
        rd_kafka_q_t *fwdq;
        mtx_lock(&rkq->rkq_lock);
        if (!(fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                sz = rkq->rkq_qsize;
                mtx_unlock(&rkq->rkq_lock);
        } else {
                mtx_unlock(&rkq->rkq_lock);
                sz = rd_kafka_q_size(fwdq);
                rd_kafka_q_destroy(fwdq);
        }
        return sz;
}

/**
 * @brief Construct a temporary on-stack replyq with increased
 *        \p rkq refcount (unless NULL), version, and debug id.
 */
static RD_INLINE RD_UNUSED rd_kafka_replyq_t
rd_kafka_replyq_make(rd_kafka_q_t *rkq, int version, const char *id) {
        rd_kafka_replyq_t replyq = RD_ZERO_INIT;

        if (rkq) {
                replyq.q       = rd_kafka_q_keep(rkq);
                replyq.version = version;
#if ENABLE_DEVEL
                replyq._id = rd_strdup(id);
#endif
        }

        return replyq;
}

/* Construct temporary on-stack replyq with increased Q refcount and
 * optional VERSION. */
#define RD_KAFKA_REPLYQ(Q, VERSION)                                            \
        rd_kafka_replyq_make(Q, VERSION, __FUNCTION__)

/* Construct temporary on-stack replyq for indicating no replyq. */
#if ENABLE_DEVEL
#define RD_KAFKA_NO_REPLYQ                                                     \
        (rd_kafka_replyq_t) {                                                  \
                NULL, 0, NULL                                                  \
        }
#else
#define RD_KAFKA_NO_REPLYQ                                                     \
        (rd_kafka_replyq_t) {                                                  \
                NULL, 0                                                        \
        }
#endif


/**
 * @returns true if the replyq is valid, else false.
 */
static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_replyq_is_valid(rd_kafka_replyq_t *replyq) {
        rd_bool_t valid = rd_true;

        if (!replyq->q)
                return rd_false;

        rd_kafka_q_lock(replyq->q);
        valid = rd_kafka_q_ready(replyq->q);
        rd_kafka_q_unlock(replyq->q);

        return valid;
}



/**
 * Set up replyq.
 * Q refcnt is increased.
 */
static RD_INLINE RD_UNUSED void rd_kafka_set_replyq(rd_kafka_replyq_t *replyq,
                                                    rd_kafka_q_t *rkq,
                                                    int32_t version) {
        replyq->q       = rkq ? rd_kafka_q_keep(rkq) : NULL;
        replyq->version = version;
#if ENABLE_DEVEL
        replyq->_id = rd_strdup(__FUNCTION__);
#endif
}

/**
 * Set rko's replyq with an optional version (versionptr != NULL).
 * Q refcnt is increased.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_op_set_replyq(rd_kafka_op_t *rko,
                       rd_kafka_q_t *rkq,
                       rd_atomic32_t *versionptr) {
        rd_kafka_set_replyq(&rko->rko_replyq, rkq,
                            versionptr ? rd_atomic32_get(versionptr) : 0);
}

/* Set reply rko's version from replyq's version */
#define rd_kafka_op_get_reply_version(REPLY_RKO, ORIG_RKO)                     \
        do {                                                                   \
                (REPLY_RKO)->rko_version = (ORIG_RKO)->rko_replyq.version;     \
        } while (0)


/* Clear replyq holder without decreasing any .q references. */
static RD_INLINE RD_UNUSED void
rd_kafka_replyq_clear(rd_kafka_replyq_t *replyq) {
        memset(replyq, 0, sizeof(*replyq));
}

/**
 * @brief Make a copy of \p src in \p dst, with its own queue reference
 */
static RD_INLINE RD_UNUSED void rd_kafka_replyq_copy(rd_kafka_replyq_t *dst,
                                                     rd_kafka_replyq_t *src) {
        dst->version = src->version;
        dst->q       = src->q;
        if (dst->q)
                rd_kafka_q_keep(dst->q);
#if ENABLE_DEVEL
        if (src->_id)
                dst->_id = rd_strdup(src->_id);
        else
                dst->_id = NULL;
#endif
}


/**
 * Clear replyq holder and destroy any .q references.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_replyq_destroy(rd_kafka_replyq_t *replyq) {
        if (replyq->q)
                rd_kafka_q_destroy(replyq->q);
#if ENABLE_DEVEL
        if (replyq->_id) {
                rd_free(replyq->_id);
                replyq->_id = NULL;
        }
#endif
        rd_kafka_replyq_clear(replyq);
}


/**
 * @brief Wrapper for rd_kafka_q_enq() that takes a replyq,
 *        steals its queue reference, enqueues the op with the replyq version,
 *        and then destroys the queue reference.
 *
 *        If \p version is non-zero it will be updated, else replyq->version.
 *
 * @returns Same as rd_kafka_q_enq()
 */
static RD_INLINE RD_UNUSED int rd_kafka_replyq_enq(rd_kafka_replyq_t *replyq,
                                                   rd_kafka_op_t *rko,
                                                   int version) {
        rd_kafka_q_t *rkq = replyq->q;
        int r;

        if (version)
                rko->rko_version = version;
        else
                rko->rko_version = replyq->version;

        /* The replyq queue reference is done after we've enqueued the rko
         * so clear it here. */
        replyq->q = NULL; /* destroyed separately below */

#if ENABLE_DEVEL
        if (replyq->_id) {
                rd_free(replyq->_id);
                replyq->_id = NULL;
        }
#endif

        /* Retain replyq->version since it is used by buf_callback
         * when dispatching the callback. */

        r = rd_kafka_q_enq(rkq, rko);

        rd_kafka_q_destroy(rkq);

        return r;
}



rd_kafka_op_t *rd_kafka_q_pop_serve(rd_kafka_q_t *rkq,
                                    rd_ts_t timeout_us,
                                    int32_t version,
                                    rd_kafka_q_cb_type_t cb_type,
                                    rd_kafka_q_serve_cb_t *callback,
                                    void *opaque);
rd_kafka_op_t *
rd_kafka_q_pop(rd_kafka_q_t *rkq, rd_ts_t timeout_us, int32_t version);
int rd_kafka_q_serve(rd_kafka_q_t *rkq,
                     int timeout_ms,
                     int max_cnt,
                     rd_kafka_q_cb_type_t cb_type,
                     rd_kafka_q_serve_cb_t *callback,
                     void *opaque);


int rd_kafka_q_move_cnt(rd_kafka_q_t *dstq,
                        rd_kafka_q_t *srcq,
                        int cnt,
                        int do_locks);

int rd_kafka_q_serve_rkmessages(rd_kafka_q_t *rkq,
                                int timeout_ms,
                                rd_kafka_message_t **rkmessages,
                                size_t rkmessages_size);
rd_kafka_resp_err_t rd_kafka_q_wait_result(rd_kafka_q_t *rkq, int timeout_ms);

int rd_kafka_q_apply(rd_kafka_q_t *rkq,
                     int (*callback)(rd_kafka_q_t *rkq,
                                     rd_kafka_op_t *rko,
                                     void *opaque),
                     void *opaque);

void rd_kafka_q_fix_offsets(rd_kafka_q_t *rkq,
                            int64_t min_offset,
                            int64_t base_offset);

/**
 * @returns the last op in the queue matching \p op_type and \p allow_err (bool)
 * @remark The \p rkq must be properly locked before this call, the returned rko
 *         is not removed from the queue and may thus not be held for longer
 *         than the lock is held.
 */
static RD_INLINE RD_UNUSED rd_kafka_op_t *
rd_kafka_q_last(rd_kafka_q_t *rkq, rd_kafka_op_type_t op_type, int allow_err) {
        rd_kafka_op_t *rko;
        TAILQ_FOREACH_REVERSE(rko, &rkq->rkq_q, rd_kafka_op_tailq, rko_link) {
                if (rko->rko_type == op_type && (allow_err || !rko->rko_err))
                        return rko;
        }

        return NULL;
}

void rd_kafka_q_io_event_enable(rd_kafka_q_t *rkq,
                                rd_socket_t fd,
                                const void *payload,
                                size_t size);

/* Public interface */
struct rd_kafka_queue_s {
        rd_kafka_q_t *rkqu_q;
        rd_kafka_t *rkqu_rk;
        int rkqu_is_owner; /**< Is owner/creator of rkqu_q */
};


rd_kafka_queue_t *rd_kafka_queue_new0(rd_kafka_t *rk, rd_kafka_q_t *rkq);

void rd_kafka_q_dump(FILE *fp, rd_kafka_q_t *rkq);

extern int RD_TLS rd_kafka_yield_thread;



/**
 * @name Enqueue op once
 * @{
 */

/**
 * @brief Minimal rd_kafka_op_t wrapper that ensures that
 *        the op is only enqueued on the provided queue once.
 *
 * Typical use-case is for an op to be triggered from multiple sources,
 * but at most once, such as from a timer and some other source.
 */
typedef struct rd_kafka_enq_once_s {
        mtx_t lock;
        int refcnt;
        rd_kafka_op_t *rko;
        rd_kafka_replyq_t replyq;
} rd_kafka_enq_once_t;


/**
 * @brief Allocate and set up a new eonce and set the initial refcount to 1.
 * @remark This is to be called by the owner of the rko.
 */
static RD_INLINE RD_UNUSED rd_kafka_enq_once_t *
rd_kafka_enq_once_new(rd_kafka_op_t *rko, rd_kafka_replyq_t replyq) {
        rd_kafka_enq_once_t *eonce = rd_calloc(1, sizeof(*eonce));
        mtx_init(&eonce->lock, mtx_plain);
        eonce->rko    = rko;
        eonce->replyq = replyq; /* struct copy */
        eonce->refcnt = 1;
        return eonce;
}

/**
 * @brief Re-enable triggering of a eonce even after it has been triggered
 *        once.
 *
 * @remark This is to be called by the owner.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_reenable(rd_kafka_enq_once_t *eonce,
                           rd_kafka_op_t *rko,
                           rd_kafka_replyq_t replyq) {
        mtx_lock(&eonce->lock);
        eonce->rko = rko;
        rd_kafka_replyq_destroy(&eonce->replyq);
        eonce->replyq = replyq; /* struct copy */
        mtx_unlock(&eonce->lock);
}


/**
 * @brief Free eonce and its resources. Must only be called with refcnt==0
 *        and eonce->lock NOT held.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_destroy0(rd_kafka_enq_once_t *eonce) {
        /* This must not be called with the rko or replyq still set, which would
         * indicate that no enqueueing was performed and that the owner
         * did not clean up, which is a bug. */
        rd_assert(!eonce->rko);
        rd_assert(!eonce->replyq.q);
#if ENABLE_DEVEL
        rd_assert(!eonce->replyq._id);
#endif
        rd_assert(eonce->refcnt == 0);

        mtx_destroy(&eonce->lock);
        rd_free(eonce);
}


/**
 * @brief Increment refcount for source (non-owner), such as a timer.
 *
 * @param srcdesc a human-readable descriptive string of the source.
 *                May be used for future debugging.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_add_source(rd_kafka_enq_once_t *eonce, const char *srcdesc) {
        mtx_lock(&eonce->lock);
        eonce->refcnt++;
        mtx_unlock(&eonce->lock);
}


/**
 * @brief Decrement refcount for source (non-owner), such as a timer.
 *
 * @param srcdesc a human-readable descriptive string of the source.
 *                May be used for future debugging.
 *
 * @remark Must only be called from the owner with the owner
 *         still holding its own refcount.
 *         This API is used to undo an add_source() from the
 *         same code.
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_del_source(rd_kafka_enq_once_t *eonce, const char *srcdesc) {
        int do_destroy;

        mtx_lock(&eonce->lock);
        rd_assert(eonce->refcnt > 0);
        eonce->refcnt--;
        do_destroy = eonce->refcnt == 0;
        mtx_unlock(&eonce->lock);

        if (do_destroy) {
                /* We're the last refcount holder, clean up eonce. */
                rd_kafka_enq_once_destroy0(eonce);
        }
}

/**
 * @brief Trigger a source's reference where the eonce resides on
 *        an rd_list_t. This is typically used as a free_cb for
 *        rd_list_destroy() and the trigger error code is
 *        always RD_KAFKA_RESP_ERR__DESTROY.
 */
void rd_kafka_enq_once_trigger_destroy(void *ptr);


/**
 * @brief Decrement refcount for source (non-owner) and return the rko
 *        if still set.
 *
 * @remark Must only be called by sources (non-owner) but only on the
 *         the owner's thread to make sure the rko is not freed.
 *
 * @remark The rko remains set on the eonce.
 */
static RD_INLINE RD_UNUSED rd_kafka_op_t *
rd_kafka_enq_once_del_source_return(rd_kafka_enq_once_t *eonce,
                                    const char *srcdesc) {
        rd_bool_t do_destroy;
        rd_kafka_op_t *rko;

        mtx_lock(&eonce->lock);

        rd_assert(eonce->refcnt > 0);
        /* Owner must still hold a eonce reference, or the eonce must
         * have been disabled by the owner (no rko) */
        rd_assert(eonce->refcnt > 1 || !eonce->rko);
        eonce->refcnt--;
        do_destroy = eonce->refcnt == 0;

        rko = eonce->rko;
        mtx_unlock(&eonce->lock);

        if (do_destroy) {
                /* We're the last refcount holder, clean up eonce. */
                rd_kafka_enq_once_destroy0(eonce);
        }

        return rko;
}

/**
 * @brief Trigger enqueuing of the rko (unless already enqueued)
 *        and drops the source's refcount.
 *
 * @remark Must only be called by sources (non-owner).
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_trigger(rd_kafka_enq_once_t *eonce,
                          rd_kafka_resp_err_t err,
                          const char *srcdesc) {
        int do_destroy;
        rd_kafka_op_t *rko       = NULL;
        rd_kafka_replyq_t replyq = RD_ZERO_INIT;

        mtx_lock(&eonce->lock);

        rd_assert(eonce->refcnt > 0);
        eonce->refcnt--;
        do_destroy = eonce->refcnt == 0;

        if (eonce->rko) {
                /* Not already enqueued, do it.
                 * Detach the rko and replyq from the eonce and unlock the eonce
                 * before enqueuing rko on reply to avoid recursive locks
                 * if the replyq has been disabled and the ops
                 * destructor is called (which might then access the eonce
                 * to clean up). */
                rko    = eonce->rko;
                replyq = eonce->replyq;

                eonce->rko = NULL;
                rd_kafka_replyq_clear(&eonce->replyq);

                /* Reply is enqueued at the end of this function */
        }
        mtx_unlock(&eonce->lock);

        if (do_destroy) {
                /* We're the last refcount holder, clean up eonce. */
                rd_kafka_enq_once_destroy0(eonce);
        }

        if (rko) {
                rko->rko_err = err;
                rd_kafka_replyq_enq(&replyq, rko, replyq.version);
                rd_kafka_replyq_destroy(&replyq);
        }
}

/**
 * @brief Destroy eonce, must only be called by the owner.
 *        There may be outstanding refcounts by non-owners after this call
 */
static RD_INLINE RD_UNUSED void
rd_kafka_enq_once_destroy(rd_kafka_enq_once_t *eonce) {
        int do_destroy;

        mtx_lock(&eonce->lock);
        rd_assert(eonce->refcnt > 0);
        eonce->refcnt--;
        do_destroy = eonce->refcnt == 0;

        eonce->rko = NULL;
        rd_kafka_replyq_destroy(&eonce->replyq);

        mtx_unlock(&eonce->lock);

        if (do_destroy) {
                /* We're the last refcount holder, clean up eonce. */
                rd_kafka_enq_once_destroy0(eonce);
        }
}


/**
 * @brief Disable the owner's eonce, extracting, resetting and returning
 *        the \c rko object.
 *
 *        This is the same as rd_kafka_enq_once_destroy() but returning
 *        the rko.
 *
 *        Use this for owner-thread triggering where the enqueuing of the
 *        rko on the replyq is not necessary.
 *
 * @returns the eonce's rko object, if still available, else NULL.
 */
static RD_INLINE RD_UNUSED rd_kafka_op_t *
rd_kafka_enq_once_disable(rd_kafka_enq_once_t *eonce) {
        int do_destroy;
        rd_kafka_op_t *rko;

        mtx_lock(&eonce->lock);
        rd_assert(eonce->refcnt > 0);
        eonce->refcnt--;
        do_destroy = eonce->refcnt == 0;

        /* May be NULL */
        rko        = eonce->rko;
        eonce->rko = NULL;
        rd_kafka_replyq_destroy(&eonce->replyq);

        mtx_unlock(&eonce->lock);

        if (do_destroy) {
                /* We're the last refcount holder, clean up eonce. */
                rd_kafka_enq_once_destroy0(eonce);
        }

        return rko;
}

/**
 * @brief Returns true if the queue can contain fetched messages.
 *
 * @locks rd_kafka_q_lock(rkq) if do_lock is set.
 */
static RD_INLINE RD_UNUSED rd_bool_t
rd_kafka_q_can_contain_fetched_msgs(rd_kafka_q_t *rkq, rd_bool_t do_lock) {
        rd_bool_t val;
        if (do_lock)
                mtx_lock(&rkq->rkq_lock);
        val = rkq->rkq_flags & RD_KAFKA_Q_F_CONSUMER;
        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);
        return val;
}


/**@}*/


#endif /* _RDKAFKA_QUEUE_H_ */
