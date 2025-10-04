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

#include "rdkafka_int.h"
#include "rdkafka_offset.h"
#include "rdkafka_topic.h"
#include "rdkafka_interceptor.h"

int RD_TLS rd_kafka_yield_thread = 0;

void rd_kafka_yield(rd_kafka_t *rk) {
        rd_kafka_yield_thread = 1;
}


/**
 * @brief Check and reset yield flag.
 * @returns rd_true if caller should yield, otherwise rd_false.
 * @remarks rkq_lock MUST be held
 */
static RD_INLINE rd_bool_t rd_kafka_q_check_yield(rd_kafka_q_t *rkq) {
        if (!(rkq->rkq_flags & RD_KAFKA_Q_F_YIELD))
                return rd_false;

        rkq->rkq_flags &= ~RD_KAFKA_Q_F_YIELD;
        return rd_true;
}
/**
 * Destroy a queue. refcnt must be at zero.
 */
void rd_kafka_q_destroy_final(rd_kafka_q_t *rkq) {

        mtx_lock(&rkq->rkq_lock);
        if (unlikely(rkq->rkq_qio != NULL)) {
                rd_free(rkq->rkq_qio);
                rkq->rkq_qio = NULL;
        }
        /* Queue must have been disabled prior to final destruction,
         * this is to catch the case where the queue owner/poll does not
         * use rd_kafka_q_destroy_owner(). */
        rd_dassert(!(rkq->rkq_flags & RD_KAFKA_Q_F_READY));
        rd_kafka_q_disable0(rkq, 0 /*no-lock*/); /* for the non-devel case */
        rd_kafka_q_fwd_set0(rkq, NULL, 0 /*no-lock*/, 0 /*no-fwd-app*/);
        rd_kafka_q_purge0(rkq, 0 /*no-lock*/);
        assert(!rkq->rkq_fwdq);
        mtx_unlock(&rkq->rkq_lock);
        mtx_destroy(&rkq->rkq_lock);
        cnd_destroy(&rkq->rkq_cond);

        if (rkq->rkq_flags & RD_KAFKA_Q_F_ALLOCATED)
                rd_free(rkq);
}



/**
 * Initialize a queue.
 */
void rd_kafka_q_init0(rd_kafka_q_t *rkq,
                      rd_kafka_t *rk,
                      rd_bool_t for_consume,
                      const char *func,
                      int line) {
        rd_kafka_q_reset(rkq);
        rkq->rkq_fwdq   = NULL;
        rkq->rkq_refcnt = 1;
        rkq->rkq_flags  = RD_KAFKA_Q_F_READY;
        if (for_consume)
                rkq->rkq_flags |= RD_KAFKA_Q_F_CONSUMER;
        rkq->rkq_rk     = rk;
        rkq->rkq_qio    = NULL;
        rkq->rkq_serve  = NULL;
        rkq->rkq_opaque = NULL;
        mtx_init(&rkq->rkq_lock, mtx_plain);
        cnd_init(&rkq->rkq_cond);
#if ENABLE_DEVEL
        rd_snprintf(rkq->rkq_name, sizeof(rkq->rkq_name), "%s:%d", func, line);
#else
        rkq->rkq_name = func;
#endif
}


/**
 * Allocate a new queue and initialize it.
 */
rd_kafka_q_t *rd_kafka_q_new0(rd_kafka_t *rk,
                              rd_bool_t for_consume,
                              const char *func,
                              int line) {
        rd_kafka_q_t *rkq = rd_malloc(sizeof(*rkq));
        if (!for_consume)
                rd_kafka_q_init(rkq, rk);
        else
                rd_kafka_consume_q_init(rkq, rk);
        rkq->rkq_flags |= RD_KAFKA_Q_F_ALLOCATED;
#if ENABLE_DEVEL
        rd_snprintf(rkq->rkq_name, sizeof(rkq->rkq_name), "%s:%d", func, line);
#else
        rkq->rkq_name = func;
#endif
        return rkq;
}

/*
 * Sets the flag RD_KAFKA_Q_F_CONSUMER for rkq, any queues it's being forwarded
 * to, recursively.
 * Setting this flag indicates that polling this queue is equivalent to calling
 * consumer poll, and will reset the max.poll.interval.ms timer. Only used
 * internally when forwarding queues.
 * @locks rd_kafka_q_lock(rkq)
 */
static void rd_kafka_q_consumer_propagate(rd_kafka_q_t *rkq) {
        mtx_lock(&rkq->rkq_lock);
        rkq->rkq_flags |= RD_KAFKA_Q_F_CONSUMER;

        if (!rkq->rkq_fwdq) {
                mtx_unlock(&rkq->rkq_lock);
                return;
        }

        /* Recursively propagate the flag to any queues rkq is already
         * forwarding to. There will be a deadlock here if the queues are being
         * forwarded circularly, but that is a user error. We can't resolve this
         * deadlock by unlocking before the recursive call, because that leads
         * to incorrectness if the rkq_fwdq is forwarded elsewhere and the old
         * one destroyed between recursive calls. */
        rd_kafka_q_consumer_propagate(rkq->rkq_fwdq);
        mtx_unlock(&rkq->rkq_lock);
}

/**
 * Set/clear forward queue.
 * Queue forwarding enables message routing inside rdkafka.
 * Typical use is to re-route all fetched messages for all partitions
 * to one single queue.
 *
 * All access to rkq_fwdq are protected by rkq_lock.
 */
void rd_kafka_q_fwd_set0(rd_kafka_q_t *srcq,
                         rd_kafka_q_t *destq,
                         int do_lock,
                         int fwd_app) {
        if (unlikely(srcq == destq))
                return;

        if (do_lock)
                mtx_lock(&srcq->rkq_lock);
        if (fwd_app)
                srcq->rkq_flags |= RD_KAFKA_Q_F_FWD_APP;
        if (srcq->rkq_fwdq) {
                rd_kafka_q_destroy(srcq->rkq_fwdq);
                srcq->rkq_fwdq = NULL;
        }
        if (destq) {
                rd_kafka_q_keep(destq);

                /* If rkq has ops in queue, append them to fwdq's queue.
                 * This is an irreversible operation. */
                if (srcq->rkq_qlen > 0) {
                        rd_dassert(destq->rkq_flags & RD_KAFKA_Q_F_READY);
                        rd_kafka_q_concat(destq, srcq);
                }

                srcq->rkq_fwdq = destq;

                if (srcq->rkq_flags & RD_KAFKA_Q_F_CONSUMER)
                        rd_kafka_q_consumer_propagate(destq);
        }
        if (do_lock)
                mtx_unlock(&srcq->rkq_lock);
}

/**
 * Purge all entries from a queue.
 */
int rd_kafka_q_purge0(rd_kafka_q_t *rkq, int do_lock) {
        rd_kafka_op_t *rko, *next;
        TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        rd_kafka_q_t *fwdq;
        int cnt = 0;

        if (do_lock)
                mtx_lock(&rkq->rkq_lock);

        if ((fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                if (do_lock)
                        mtx_unlock(&rkq->rkq_lock);
                cnt = rd_kafka_q_purge(fwdq);
                rd_kafka_q_destroy(fwdq);
                return cnt;
        }

        /* Move ops queue to tmpq to avoid lock-order issue
         * by locks taken from rd_kafka_op_destroy(). */
        TAILQ_MOVE(&tmpq, &rkq->rkq_q, rko_link);

        rd_kafka_q_mark_served(rkq);

        /* Zero out queue */
        rd_kafka_q_reset(rkq);

        if (do_lock)
                mtx_unlock(&rkq->rkq_lock);

        /* Destroy the ops */
        next = TAILQ_FIRST(&tmpq);
        while ((rko = next)) {
                next = TAILQ_NEXT(next, rko_link);
                rd_kafka_op_destroy(rko);
                cnt++;
        }

        return cnt;
}


/**
 * Purge all entries from a queue with a rktp version smaller than `version`
 * This shaves off the head of the queue, up until the first rko with
 * a non-matching rktp or version.
 */
void rd_kafka_q_purge_toppar_version(rd_kafka_q_t *rkq,
                                     rd_kafka_toppar_t *rktp,
                                     int version) {
        rd_kafka_op_t *rko, *next;
        TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        int32_t cnt  = 0;
        int64_t size = 0;
        rd_kafka_q_t *fwdq;

        mtx_lock(&rkq->rkq_lock);

        if ((fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                mtx_unlock(&rkq->rkq_lock);
                rd_kafka_q_purge_toppar_version(fwdq, rktp, version);
                rd_kafka_q_destroy(fwdq);
                return;
        }

        /* Move ops to temporary queue and then destroy them from there
         * without locks to avoid lock-ordering problems in op_destroy() */
        while ((rko = TAILQ_FIRST(&rkq->rkq_q)) && rko->rko_rktp &&
               rko->rko_rktp == rktp && rko->rko_version < version) {
                TAILQ_REMOVE(&rkq->rkq_q, rko, rko_link);
                TAILQ_INSERT_TAIL(&tmpq, rko, rko_link);
                cnt++;
                size += rko->rko_len;
        }

        rd_kafka_q_mark_served(rkq);

        rkq->rkq_qlen -= cnt;
        rkq->rkq_qsize -= size;
        mtx_unlock(&rkq->rkq_lock);

        next = TAILQ_FIRST(&tmpq);
        while ((rko = next)) {
                next = TAILQ_NEXT(next, rko_link);
                rd_kafka_op_destroy(rko);
        }
}


/**
 * Move 'cnt' entries from 'srcq' to 'dstq'.
 * If 'cnt' == -1 all entries will be moved.
 * Returns the number of entries moved.
 */
int rd_kafka_q_move_cnt(rd_kafka_q_t *dstq,
                        rd_kafka_q_t *srcq,
                        int cnt,
                        int do_locks) {
        rd_kafka_op_t *rko;
        int mcnt = 0;

        if (do_locks) {
                mtx_lock(&srcq->rkq_lock);
                mtx_lock(&dstq->rkq_lock);
        }

        if (!dstq->rkq_fwdq && !srcq->rkq_fwdq) {
                if (cnt > 0 && dstq->rkq_qlen == 0)
                        rd_kafka_q_io_event(dstq);

                /* Optimization, if 'cnt' is equal/larger than all
                 * items of 'srcq' we can move the entire queue. */
                if (cnt == -1 || cnt >= (int)srcq->rkq_qlen) {
                        mcnt = srcq->rkq_qlen;
                        rd_kafka_q_concat0(dstq, srcq, 0 /*no-lock*/);
                } else {
                        while (mcnt < cnt &&
                               (rko = TAILQ_FIRST(&srcq->rkq_q))) {
                                TAILQ_REMOVE(&srcq->rkq_q, rko, rko_link);
                                if (likely(!rko->rko_prio))
                                        TAILQ_INSERT_TAIL(&dstq->rkq_q, rko,
                                                          rko_link);
                                else
                                        TAILQ_INSERT_SORTED(
                                            &dstq->rkq_q, rko, rd_kafka_op_t *,
                                            rko_link, rd_kafka_op_cmp_prio);

                                srcq->rkq_qlen--;
                                dstq->rkq_qlen++;
                                srcq->rkq_qsize -= rko->rko_len;
                                dstq->rkq_qsize += rko->rko_len;
                                mcnt++;
                        }
                }

                rd_kafka_q_mark_served(srcq);

        } else
                mcnt = rd_kafka_q_move_cnt(
                    dstq->rkq_fwdq ? dstq->rkq_fwdq : dstq,
                    srcq->rkq_fwdq ? srcq->rkq_fwdq : srcq, cnt, do_locks);

        if (do_locks) {
                mtx_unlock(&dstq->rkq_lock);
                mtx_unlock(&srcq->rkq_lock);
        }

        return mcnt;
}


/**
 * Filters out outdated ops.
 */
static RD_INLINE rd_kafka_op_t *
rd_kafka_op_filter(rd_kafka_q_t *rkq, rd_kafka_op_t *rko, int version) {
        if (unlikely(!rko))
                return NULL;

        if (unlikely(rd_kafka_op_version_outdated(rko, version))) {
                rd_kafka_q_deq0(rkq, rko);
                rd_kafka_op_destroy(rko);
                return NULL;
        }

        return rko;
}



/**
 * Pop an op from a queue.
 *
 * Locality: any thread.
 */


/**
 * Serve q like rd_kafka_q_serve() until an op is found that can be returned
 * as an event to the application.
 *
 * @returns the first event:able op, or NULL on timeout.
 *
 * Locality: any thread
 */
rd_kafka_op_t *rd_kafka_q_pop_serve(rd_kafka_q_t *rkq,
                                    rd_ts_t timeout_us,
                                    int32_t version,
                                    rd_kafka_q_cb_type_t cb_type,
                                    rd_kafka_q_serve_cb_t *callback,
                                    void *opaque) {
        rd_kafka_op_t *rko;
        rd_kafka_q_t *fwdq;

        rd_dassert(cb_type);

        mtx_lock(&rkq->rkq_lock);

        rd_kafka_yield_thread = 0;
        if (!(fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                const rd_bool_t can_q_contain_fetched_msgs =
                    rd_kafka_q_can_contain_fetched_msgs(rkq, RD_DONT_LOCK);

                struct timespec timeout_tspec;

                rd_timeout_init_timespec_us(&timeout_tspec, timeout_us);

                if (can_q_contain_fetched_msgs)
                        rd_kafka_app_poll_start(rkq->rkq_rk, 0, timeout_us);

                while (1) {
                        rd_kafka_op_res_t res;
                        /* Keep track of current lock status to avoid
                         * unnecessary lock flapping in all the cases below. */
                        rd_bool_t is_locked = rd_true;

                        /* Filter out outdated ops */
                retry:
                        while ((rko = TAILQ_FIRST(&rkq->rkq_q)) &&
                               !(rko = rd_kafka_op_filter(rkq, rko, version)))
                                ;

                        rd_kafka_q_mark_served(rkq);

                        if (rko) {
                                /* Proper versioned op */
                                rd_kafka_q_deq0(rkq, rko);

                                /* Let op_handle() operate without lock
                                 * held to allow re-enqueuing, etc. */
                                mtx_unlock(&rkq->rkq_lock);
                                is_locked = rd_false;

                                /* Ops with callbacks are considered handled
                                 * and we move on to the next op, if any.
                                 * Ops w/o callbacks are returned immediately */
                                res = rd_kafka_op_handle(rkq->rkq_rk, rkq, rko,
                                                         cb_type, opaque,
                                                         callback);

                                if (res == RD_KAFKA_OP_RES_HANDLED ||
                                    res == RD_KAFKA_OP_RES_KEEP) {
                                        mtx_lock(&rkq->rkq_lock);
                                        is_locked = rd_true;
                                        goto retry; /* Next op */
                                } else if (unlikely(res ==
                                                    RD_KAFKA_OP_RES_YIELD)) {
                                        if (can_q_contain_fetched_msgs)
                                                rd_kafka_app_polled(
                                                    rkq->rkq_rk);
                                        /* Callback yielded, unroll */
                                        return NULL;
                                } else {
                                        if (can_q_contain_fetched_msgs)
                                                rd_kafka_app_polled(
                                                    rkq->rkq_rk);
                                        break; /* Proper op, handle below. */
                                }
                        }

                        if (unlikely(rd_kafka_q_check_yield(rkq))) {
                                if (is_locked)
                                        mtx_unlock(&rkq->rkq_lock);
                                if (can_q_contain_fetched_msgs)
                                        rd_kafka_app_polled(rkq->rkq_rk);
                                return NULL;
                        }

                        if (!is_locked)
                                mtx_lock(&rkq->rkq_lock);

                        if (cnd_timedwait_abs(&rkq->rkq_cond, &rkq->rkq_lock,
                                              &timeout_tspec) != thrd_success) {
                                mtx_unlock(&rkq->rkq_lock);
                                if (can_q_contain_fetched_msgs)
                                        rd_kafka_app_polled(rkq->rkq_rk);
                                return NULL;
                        }
                }

        } else {
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
                rko = rd_kafka_q_pop_serve(fwdq, timeout_us, version, cb_type,
                                           callback, opaque);
                rd_kafka_q_destroy(fwdq);
        }


        return rko;
}

rd_kafka_op_t *
rd_kafka_q_pop(rd_kafka_q_t *rkq, rd_ts_t timeout_us, int32_t version) {
        return rd_kafka_q_pop_serve(rkq, timeout_us, version,
                                    RD_KAFKA_Q_CB_RETURN, NULL, NULL);
}


/**
 * Pop all available ops from a queue and call the provided
 * callback for each op.
 * `max_cnt` limits the number of ops served, 0 = no limit.
 *
 * Returns the number of ops served.
 *
 * Locality: any thread.
 */
int rd_kafka_q_serve(rd_kafka_q_t *rkq,
                     int timeout_ms,
                     int max_cnt,
                     rd_kafka_q_cb_type_t cb_type,
                     rd_kafka_q_serve_cb_t *callback,
                     void *opaque) {
        rd_kafka_t *rk = rkq->rkq_rk;
        rd_kafka_op_t *rko;
        rd_kafka_q_t localq;
        rd_kafka_q_t *fwdq;
        int cnt = 0;
        struct timespec timeout_tspec;
        const rd_bool_t can_q_contain_fetched_msgs =
            rd_kafka_q_can_contain_fetched_msgs(rkq, RD_DONT_LOCK);

        rd_dassert(cb_type);

        mtx_lock(&rkq->rkq_lock);

        rd_dassert(TAILQ_EMPTY(&rkq->rkq_q) || rkq->rkq_qlen > 0);
        if ((fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                int ret;
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
                ret = rd_kafka_q_serve(fwdq, timeout_ms, max_cnt, cb_type,
                                       callback, opaque);
                rd_kafka_q_destroy(fwdq);
                return ret;
        }


        rd_timeout_init_timespec(&timeout_tspec, timeout_ms);

        if (can_q_contain_fetched_msgs)
                rd_kafka_app_poll_start(rk, 0, timeout_ms);

        /* Wait for op */
        while (!(rko = TAILQ_FIRST(&rkq->rkq_q)) &&
               !rd_kafka_q_check_yield(rkq) &&
               cnd_timedwait_abs(&rkq->rkq_cond, &rkq->rkq_lock,
                                 &timeout_tspec) == thrd_success)
                ;

        rd_kafka_q_mark_served(rkq);

        if (!rko) {
                mtx_unlock(&rkq->rkq_lock);
                if (can_q_contain_fetched_msgs)
                        rd_kafka_app_polled(rk);
                return 0;
        }

        /* Move the first `max_cnt` ops. */
        rd_kafka_q_init(&localq, rkq->rkq_rk);
        rd_kafka_q_move_cnt(&localq, rkq, max_cnt == 0 ? -1 /*all*/ : max_cnt,
                            0 /*no-locks*/);

        mtx_unlock(&rkq->rkq_lock);

        rd_kafka_yield_thread = 0;

        /* Call callback for each op */
        while ((rko = TAILQ_FIRST(&localq.rkq_q))) {
                rd_kafka_op_res_t res;

                rd_kafka_q_deq0(&localq, rko);
                res = rd_kafka_op_handle(rk, &localq, rko, cb_type, opaque,
                                         callback);
                /* op must have been handled */
                rd_kafka_assert(NULL, res != RD_KAFKA_OP_RES_PASS);
                cnt++;

                if (unlikely(res == RD_KAFKA_OP_RES_YIELD ||
                             rd_kafka_yield_thread)) {
                        /* Callback called rd_kafka_yield(), we must
                         * stop our callback dispatching and put the
                         * ops in localq back on the original queue head. */
                        if (!TAILQ_EMPTY(&localq.rkq_q))
                                rd_kafka_q_prepend(rkq, &localq);
                        break;
                }
        }

        if (can_q_contain_fetched_msgs)
                rd_kafka_app_polled(rk);

        rd_kafka_q_destroy_owner(&localq);

        return cnt;
}

/**
 * @brief Filter out and destroy outdated messages.
 *
 * @returns Returns the number of valid messages.
 *
 * @locality Any thread.
 */
static size_t
rd_kafka_purge_outdated_messages(rd_kafka_toppar_t *rktp,
                                 int32_t version,
                                 rd_kafka_message_t **rkmessages,
                                 size_t cnt,
                                 struct rd_kafka_op_tailq *ctrl_msg_q) {
        size_t valid_count = 0;
        size_t i;
        rd_kafka_op_t *rko, *next;

        for (i = 0; i < cnt; i++) {
                rko = rkmessages[i]->_private;
                if (rko->rko_rktp == rktp &&
                    rd_kafka_op_version_outdated(rko, version)) {
                        /* This also destroys the corresponding rkmessage. */
                        rd_kafka_op_destroy(rko);
                } else if (i > valid_count) {
                        rkmessages[valid_count++] = rkmessages[i];
                } else {
                        valid_count++;
                }
        }

        /* Discard outdated control msgs ops */
        next = TAILQ_FIRST(ctrl_msg_q);
        while (next) {
                rko  = next;
                next = TAILQ_NEXT(rko, rko_link);
                if (rko->rko_rktp == rktp &&
                    rd_kafka_op_version_outdated(rko, version)) {
                        TAILQ_REMOVE(ctrl_msg_q, rko, rko_link);
                        rd_kafka_op_destroy(rko);
                }
        }

        return valid_count;
}


/**
 * Populate 'rkmessages' array with messages from 'rkq'.
 * If 'auto_commit' is set, each message's offset will be committed
 * to the offset store for that toppar.
 *
 * Returns the number of messages added.
 */

int rd_kafka_q_serve_rkmessages(rd_kafka_q_t *rkq,
                                int timeout_ms,
                                rd_kafka_message_t **rkmessages,
                                size_t rkmessages_size) {
        unsigned int cnt = 0;
        TAILQ_HEAD(, rd_kafka_op_s) tmpq = TAILQ_HEAD_INITIALIZER(tmpq);
        struct rd_kafka_op_tailq ctrl_msg_q =
            TAILQ_HEAD_INITIALIZER(ctrl_msg_q);
        rd_kafka_op_t *rko, *next;
        rd_kafka_t *rk = rkq->rkq_rk;
        rd_kafka_q_t *fwdq;
        struct timespec timeout_tspec;
        int i;

        mtx_lock(&rkq->rkq_lock);
        if ((fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                /* Since the q_pop may block we need to release the parent
                 * queue's lock. */
                mtx_unlock(&rkq->rkq_lock);
                cnt = rd_kafka_q_serve_rkmessages(fwdq, timeout_ms, rkmessages,
                                                  rkmessages_size);
                rd_kafka_q_destroy(fwdq);
                return cnt;
        }

        mtx_unlock(&rkq->rkq_lock);

        rd_timeout_init_timespec(&timeout_tspec, timeout_ms);

        rd_kafka_app_poll_start(rk, 0, timeout_ms);

        rd_kafka_yield_thread = 0;
        while (cnt < rkmessages_size) {
                rd_kafka_op_res_t res;

                mtx_lock(&rkq->rkq_lock);

                while (!(rko = TAILQ_FIRST(&rkq->rkq_q)) &&
                       !rd_kafka_q_check_yield(rkq) &&
                       cnd_timedwait_abs(&rkq->rkq_cond, &rkq->rkq_lock,
                                         &timeout_tspec) == thrd_success)
                        ;

                rd_kafka_q_mark_served(rkq);

                if (!rko) {
                        mtx_unlock(&rkq->rkq_lock);
                        break; /* Timed out */
                }

                rd_kafka_q_deq0(rkq, rko);

                mtx_unlock(&rkq->rkq_lock);

                if (unlikely(rko->rko_type == RD_KAFKA_OP_BARRIER)) {
                        cnt = (unsigned int)rd_kafka_purge_outdated_messages(
                            rko->rko_rktp, rko->rko_version, rkmessages, cnt,
                            &ctrl_msg_q);
                        rd_kafka_op_destroy(rko);
                        continue;
                }

                if (rd_kafka_op_version_outdated(rko, 0)) {
                        /* Outdated op, put on discard queue */
                        TAILQ_INSERT_TAIL(&tmpq, rko, rko_link);
                        continue;
                }

                /* Serve non-FETCH callbacks */
                res =
                    rd_kafka_poll_cb(rk, rkq, rko, RD_KAFKA_Q_CB_RETURN, NULL);
                if (res == RD_KAFKA_OP_RES_KEEP ||
                    res == RD_KAFKA_OP_RES_HANDLED) {
                        /* Callback served, rko is destroyed (if HANDLED). */
                        continue;
                } else if (unlikely(res == RD_KAFKA_OP_RES_YIELD ||
                                    rd_kafka_yield_thread)) {
                        /* Yield. */
                        break;
                }
                rd_dassert(res == RD_KAFKA_OP_RES_PASS);

                /* If this is a control messages, don't return message to
                 * application. Add it to a tmp queue from where we can store
                 * the offset and destroy the op */
                if (unlikely(rd_kafka_op_is_ctrl_msg(rko))) {
                        TAILQ_INSERT_TAIL(&ctrl_msg_q, rko, rko_link);
                        continue;
                }

                /* Get rkmessage from rko and append to array. */
                rkmessages[cnt++] = rd_kafka_message_get(rko);
        }

        for (i = cnt - 1; i >= 0; i--) {
                rko = (rd_kafka_op_t *)rkmessages[i]->_private;
                rd_kafka_toppar_t *rktp = rko->rko_rktp;
                int64_t offset          = rkmessages[i]->offset + 1;
                if (unlikely(rktp && (rktp->rktp_app_pos.offset < offset)))
                        rd_kafka_update_app_pos(
                            rk, rktp,
                            RD_KAFKA_FETCH_POS(
                                offset,
                                rd_kafka_message_leader_epoch(rkmessages[i])),
                            RD_DO_LOCK);
        }

        /* Discard non-desired and already handled ops */
        next = TAILQ_FIRST(&tmpq);
        while (next) {
                rko  = next;
                next = TAILQ_NEXT(next, rko_link);
                rd_kafka_op_destroy(rko);
        }

        /* Discard ctrl msgs */
        next = TAILQ_FIRST(&ctrl_msg_q);
        while (next) {
                rko                     = next;
                next                    = TAILQ_NEXT(next, rko_link);
                rd_kafka_toppar_t *rktp = rko->rko_rktp;
                int64_t offset = rko->rko_u.fetch.rkm.rkm_rkmessage.offset + 1;
                if (rktp && (rktp->rktp_app_pos.offset < offset))
                        rd_kafka_update_app_pos(
                            rk, rktp,
                            RD_KAFKA_FETCH_POS(
                                offset,
                                rd_kafka_message_leader_epoch(
                                    &rko->rko_u.fetch.rkm.rkm_rkmessage)),
                            RD_DO_LOCK);
                rd_kafka_op_destroy(rko);
        }

        rd_kafka_app_polled(rk);

        return cnt;
}



void rd_kafka_queue_destroy(rd_kafka_queue_t *rkqu) {
        if (rkqu->rkqu_is_owner)
                rd_kafka_q_destroy_owner(rkqu->rkqu_q);
        else
                rd_kafka_q_destroy(rkqu->rkqu_q);
        rd_free(rkqu);
}

rd_kafka_queue_t *rd_kafka_queue_new0(rd_kafka_t *rk, rd_kafka_q_t *rkq) {
        rd_kafka_queue_t *rkqu;

        rkqu = rd_calloc(1, sizeof(*rkqu));

        rkqu->rkqu_q = rkq;
        rd_kafka_q_keep(rkq);

        rkqu->rkqu_rk = rk;

        return rkqu;
}


rd_kafka_queue_t *rd_kafka_queue_new(rd_kafka_t *rk) {
        rd_kafka_q_t *rkq;
        rd_kafka_queue_t *rkqu;

        rkq  = rd_kafka_q_new(rk);
        rkqu = rd_kafka_queue_new0(rk, rkq);
        rd_kafka_q_destroy(rkq); /* Loose refcount from q_new, one is held
                                  * by queue_new0 */
        rkqu->rkqu_is_owner = 1;
        return rkqu;
}


rd_kafka_queue_t *rd_kafka_queue_get_main(rd_kafka_t *rk) {
        return rd_kafka_queue_new0(rk, rk->rk_rep);
}


rd_kafka_queue_t *rd_kafka_queue_get_consumer(rd_kafka_t *rk) {
        if (!rk->rk_cgrp)
                return NULL;
        return rd_kafka_queue_new0(rk, rk->rk_cgrp->rkcg_q);
}

rd_kafka_queue_t *rd_kafka_queue_get_partition(rd_kafka_t *rk,
                                               const char *topic,
                                               int32_t partition) {
        rd_kafka_toppar_t *rktp;
        rd_kafka_queue_t *result;

        if (rk->rk_type == RD_KAFKA_PRODUCER)
                return NULL;

        rktp = rd_kafka_toppar_get2(rk, topic, partition, 0, /* no ua_on_miss */
                                    1 /* create_on_miss */);

        if (!rktp)
                return NULL;

        result = rd_kafka_queue_new0(rk, rktp->rktp_fetchq);
        rd_kafka_toppar_destroy(rktp);

        return result;
}

rd_kafka_queue_t *rd_kafka_queue_get_background(rd_kafka_t *rk) {
        rd_kafka_queue_t *rkqu;

        rd_kafka_wrlock(rk);
        if (!rk->rk_background.q) {
                char errstr[256];

                if (rd_kafka_background_thread_create(rk, errstr,
                                                      sizeof(errstr))) {
                        rd_kafka_log(rk, LOG_ERR, "BACKGROUND",
                                     "Failed to create background thread: %s",
                                     errstr);
                        rd_kafka_wrunlock(rk);
                        return NULL;
                }
        }

        rkqu = rd_kafka_queue_new0(rk, rk->rk_background.q);
        rd_kafka_wrunlock(rk);
        return rkqu;
}


rd_kafka_resp_err_t rd_kafka_set_log_queue(rd_kafka_t *rk,
                                           rd_kafka_queue_t *rkqu) {
        rd_kafka_q_t *rkq;

        if (!rk->rk_logq)
                return RD_KAFKA_RESP_ERR__NOT_CONFIGURED;

        if (!rkqu)
                rkq = rk->rk_rep;
        else
                rkq = rkqu->rkqu_q;
        rd_kafka_q_fwd_set(rk->rk_logq, rkq);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

void rd_kafka_queue_forward(rd_kafka_queue_t *src, rd_kafka_queue_t *dst) {
        rd_kafka_q_fwd_set0(src->rkqu_q, dst ? dst->rkqu_q : NULL,
                            1, /* do_lock */
                            1 /* fwd_app */);
}


size_t rd_kafka_queue_length(rd_kafka_queue_t *rkqu) {
        return (size_t)rd_kafka_q_len(rkqu->rkqu_q);
}

/**
 * @brief Enable or disable(fd==-1) fd-based wake-ups for queue
 */
void rd_kafka_q_io_event_enable(rd_kafka_q_t *rkq,
                                rd_socket_t fd,
                                const void *payload,
                                size_t size) {
        struct rd_kafka_q_io *qio = NULL;

        if (fd != -1) {
                qio                  = rd_malloc(sizeof(*qio) + size);
                qio->fd              = fd;
                qio->size            = size;
                qio->payload         = (void *)(qio + 1);
                qio->sent            = rd_false;
                qio->event_cb        = NULL;
                qio->event_cb_opaque = NULL;
                memcpy(qio->payload, payload, size);
        }

        mtx_lock(&rkq->rkq_lock);
        if (rkq->rkq_qio) {
                rd_free(rkq->rkq_qio);
                rkq->rkq_qio = NULL;
        }

        if (fd != -1) {
                rkq->rkq_qio = qio;
        }

        mtx_unlock(&rkq->rkq_lock);
}

void rd_kafka_queue_io_event_enable(rd_kafka_queue_t *rkqu,
                                    int fd,
                                    const void *payload,
                                    size_t size) {
        rd_kafka_q_io_event_enable(rkqu->rkqu_q, fd, payload, size);
}


void rd_kafka_queue_yield(rd_kafka_queue_t *rkqu) {
        rd_kafka_q_yield(rkqu->rkqu_q);
}


/**
 * @brief Enable or disable(event_cb==NULL) callback-based wake-ups for queue
 */
void rd_kafka_q_cb_event_enable(rd_kafka_q_t *rkq,
                                void (*event_cb)(rd_kafka_t *rk, void *opaque),
                                void *opaque) {
        struct rd_kafka_q_io *qio = NULL;

        if (event_cb) {
                qio                  = rd_malloc(sizeof(*qio));
                qio->fd              = -1;
                qio->size            = 0;
                qio->payload         = NULL;
                qio->event_cb        = event_cb;
                qio->event_cb_opaque = opaque;
        }

        mtx_lock(&rkq->rkq_lock);
        if (rkq->rkq_qio) {
                rd_free(rkq->rkq_qio);
                rkq->rkq_qio = NULL;
        }

        if (event_cb) {
                rkq->rkq_qio = qio;
        }

        mtx_unlock(&rkq->rkq_lock);
}

void rd_kafka_queue_cb_event_enable(rd_kafka_queue_t *rkqu,
                                    void (*event_cb)(rd_kafka_t *rk,
                                                     void *opaque),
                                    void *opaque) {
        rd_kafka_q_cb_event_enable(rkqu->rkqu_q, event_cb, opaque);
}


/**
 * Helper: wait for single op on 'rkq', and return its error,
 * or .._TIMED_OUT on timeout.
 */
rd_kafka_resp_err_t rd_kafka_q_wait_result(rd_kafka_q_t *rkq, int timeout_ms) {
        rd_kafka_op_t *rko;
        rd_kafka_resp_err_t err;

        rko = rd_kafka_q_pop(rkq, rd_timeout_us(timeout_ms), 0);
        if (!rko)
                err = RD_KAFKA_RESP_ERR__TIMED_OUT;
        else {
                err = rko->rko_err;
                rd_kafka_op_destroy(rko);
        }

        return err;
}


/**
 * Apply \p callback on each op in queue.
 * If the callback wishes to remove the rko it must do so using
 * using rd_kafka_op_deq0().
 *
 * @returns the sum of \p callback() return values.
 * @remark rkq will be locked, callers should take care not to
 *         interact with \p rkq through other means from the callback to avoid
 *         deadlocks.
 */
int rd_kafka_q_apply(rd_kafka_q_t *rkq,
                     int (*callback)(rd_kafka_q_t *rkq,
                                     rd_kafka_op_t *rko,
                                     void *opaque),
                     void *opaque) {
        rd_kafka_op_t *rko, *next;
        rd_kafka_q_t *fwdq;
        int cnt = 0;

        mtx_lock(&rkq->rkq_lock);
        if ((fwdq = rd_kafka_q_fwd_get(rkq, 0))) {
                mtx_unlock(&rkq->rkq_lock);
                cnt = rd_kafka_q_apply(fwdq, callback, opaque);
                rd_kafka_q_destroy(fwdq);
                return cnt;
        }

        next = TAILQ_FIRST(&rkq->rkq_q);
        while ((rko = next)) {
                next = TAILQ_NEXT(next, rko_link);
                cnt += callback(rkq, rko, opaque);
        }

        rd_kafka_q_mark_served(rkq);

        mtx_unlock(&rkq->rkq_lock);

        return cnt;
}

/**
 * @brief Convert relative to absolute offsets and also purge any messages
 *        that are older than \p min_offset.
 * @remark Error ops with ERR__NOT_IMPLEMENTED will not be purged since
 *         they are used to indicate unknnown compression codecs and compressed
 *         messagesets may have a starting offset lower than what we requested.
 * @remark \p rkq locking is not performed (caller's responsibility)
 * @remark Must NOT be used on fwdq.
 */
void rd_kafka_q_fix_offsets(rd_kafka_q_t *rkq,
                            int64_t min_offset,
                            int64_t base_offset) {
        rd_kafka_op_t *rko, *next;
        int adj_len      = 0;
        int64_t adj_size = 0;

        rd_kafka_assert(NULL, !rkq->rkq_fwdq);

        next = TAILQ_FIRST(&rkq->rkq_q);
        while ((rko = next)) {
                next = TAILQ_NEXT(next, rko_link);

                if (unlikely(rko->rko_type != RD_KAFKA_OP_FETCH))
                        continue;

                rko->rko_u.fetch.rkm.rkm_offset += base_offset;

                if (rko->rko_u.fetch.rkm.rkm_offset < min_offset &&
                    rko->rko_err != RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED) {
                        adj_len++;
                        adj_size += rko->rko_len;
                        TAILQ_REMOVE(&rkq->rkq_q, rko, rko_link);
                        rd_kafka_op_destroy(rko);
                        continue;
                }
        }


        rkq->rkq_qlen -= adj_len;
        rkq->rkq_qsize -= adj_size;
}


/**
 * @brief Print information and contents of queue
 */
void rd_kafka_q_dump(FILE *fp, rd_kafka_q_t *rkq) {
        mtx_lock(&rkq->rkq_lock);
        fprintf(fp,
                "Queue %p \"%s\" (refcnt %d, flags 0x%x, %d ops, "
                "%" PRId64 " bytes)\n",
                rkq, rkq->rkq_name, rkq->rkq_refcnt, rkq->rkq_flags,
                rkq->rkq_qlen, rkq->rkq_qsize);

        if (rkq->rkq_qio)
                fprintf(fp, " QIO fd %d\n", (int)rkq->rkq_qio->fd);
        if (rkq->rkq_serve)
                fprintf(fp, " Serve callback %p, opaque %p\n", rkq->rkq_serve,
                        rkq->rkq_opaque);

        if (rkq->rkq_fwdq) {
                fprintf(fp, " Forwarded ->\n");
                rd_kafka_q_dump(fp, rkq->rkq_fwdq);
        } else {
                rd_kafka_op_t *rko;

                if (!TAILQ_EMPTY(&rkq->rkq_q))
                        fprintf(fp, " Queued ops:\n");
                TAILQ_FOREACH(rko, &rkq->rkq_q, rko_link) {
                        fprintf(fp,
                                "  %p %s (v%" PRId32
                                ", flags 0x%x, "
                                "prio %d, len %" PRId32
                                ", source %s, "
                                "replyq %p)\n",
                                rko, rd_kafka_op2str(rko->rko_type),
                                rko->rko_version, rko->rko_flags, rko->rko_prio,
                                rko->rko_len,
#if ENABLE_DEVEL
                                rko->rko_source
#else
                                "-"
#endif
                                ,
                                rko->rko_replyq.q);
                }
        }

        mtx_unlock(&rkq->rkq_lock);
}


void rd_kafka_enq_once_trigger_destroy(void *ptr) {
        rd_kafka_enq_once_t *eonce = ptr;

        rd_kafka_enq_once_trigger(eonce, RD_KAFKA_RESP_ERR__DESTROY, "destroy");
}
