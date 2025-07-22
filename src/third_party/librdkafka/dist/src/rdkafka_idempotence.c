/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
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

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_txnmgr.h"
#include "rdkafka_request.h"
#include "rdunittest.h"

#include <stdarg.h>

/**
 * @name Idempotent Producer logic
 *
 *
 * Unrecoverable idempotent producer errors that could jeopardize the
 * idempotency guarantees if the producer was to continue operating
 * are treated as fatal errors, unless the producer is transactional in which
 * case the current transaction will fail (also known as an abortable error)
 * but the producer will not raise a fatal error.
 *
 */

static void rd_kafka_idemp_pid_timer_restart(rd_kafka_t *rk,
                                             rd_bool_t immediate,
                                             const char *reason);


/**
 * @brief Set the producer's idempotence state.
 * @locks rd_kafka_wrlock() MUST be held
 */
void rd_kafka_idemp_set_state(rd_kafka_t *rk,
                              rd_kafka_idemp_state_t new_state) {

        if (rk->rk_eos.idemp_state == new_state)
                return;

        if (rd_kafka_fatal_error_code(rk) &&
            new_state != RD_KAFKA_IDEMP_STATE_FATAL_ERROR &&
            new_state != RD_KAFKA_IDEMP_STATE_TERM &&
            new_state != RD_KAFKA_IDEMP_STATE_DRAIN_RESET &&
            new_state != RD_KAFKA_IDEMP_STATE_DRAIN_BUMP) {
                rd_kafka_dbg(rk, EOS, "IDEMPSTATE",
                             "Denying state change %s -> %s since a "
                             "fatal error has been raised",
                             rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                             rd_kafka_idemp_state2str(new_state));
                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_FATAL_ERROR);
                return;
        }

        rd_kafka_dbg(rk, EOS, "IDEMPSTATE",
                     "Idempotent producer state change %s -> %s",
                     rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                     rd_kafka_idemp_state2str(new_state));

        rk->rk_eos.idemp_state    = new_state;
        rk->rk_eos.ts_idemp_state = rd_clock();

        /* Inform transaction manager of state change */
        if (rd_kafka_is_transactional(rk))
                rd_kafka_txn_idemp_state_change(rk, new_state);
}



/**
 * @brief Find a usable broker suitable for acquiring Pid
 *        or Coordinator query.
 *
 * @locks rd_kafka_wrlock() MUST be held
 *
 * @returns a broker with increased refcount, or NULL on error.
 */
rd_kafka_broker_t *rd_kafka_idemp_broker_any(rd_kafka_t *rk,
                                             rd_kafka_resp_err_t *errp,
                                             char *errstr,
                                             size_t errstr_size) {
        rd_kafka_broker_t *rkb;
        int up_cnt;

        rkb = rd_kafka_broker_any_up(rk, &up_cnt,
                                     rd_kafka_broker_filter_non_idempotent,
                                     NULL, "acquire ProducerID");
        if (rkb)
                return rkb;

        if (up_cnt > 0) {
                *errp = RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
                rd_snprintf(errstr, errstr_size,
                            "%s not supported by "
                            "any of the %d connected broker(s): requires "
                            "Apache Kafka broker version >= 0.11.0",
                            rd_kafka_is_transactional(rk)
                                ? "Transactions"
                                : "Idempotent producer",
                            up_cnt);
        } else {
                *errp = RD_KAFKA_RESP_ERR__TRANSPORT;
                rd_snprintf(errstr, errstr_size,
                            "No brokers available for %s (%d broker(s) known)",
                            rd_kafka_is_transactional(rk)
                                ? "Transactions"
                                : "Idempotent producer",
                            rd_atomic32_get(&rk->rk_broker_cnt));
        }

        rd_kafka_dbg(rk, EOS, "PIDBROKER", "%s", errstr);

        return NULL;
}



/**
 * @brief Check if an error needs special attention, possibly
 *        raising a fatal error.
 *
 * @param is_fatal if true, force fatal error regardless of error code.
 *
 * @returns rd_true if a fatal error was triggered, else rd_false.
 *
 * @locks rd_kafka_wrlock() MUST be held
 * @locality rdkafka main thread
 */
rd_bool_t rd_kafka_idemp_check_error(rd_kafka_t *rk,
                                     rd_kafka_resp_err_t err,
                                     const char *errstr,
                                     rd_bool_t is_fatal) {
        const char *preface = "";

        switch (err) {
        case RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE:
        case RD_KAFKA_RESP_ERR_INVALID_TRANSACTION_TIMEOUT:
        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
                is_fatal = rd_true;
                break;

        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
        case RD_KAFKA_RESP_ERR_PRODUCER_FENCED:
                is_fatal = rd_true;
                /* Normalize error */
                err     = RD_KAFKA_RESP_ERR__FENCED;
                preface = "Producer fenced by newer instance: ";
                break;

        default:
                break;
        }

        if (!is_fatal)
                return rd_false;

        if (rd_kafka_is_transactional(rk))
                rd_kafka_txn_set_fatal_error(rk, RD_DONT_LOCK, err, "%s%s",
                                             preface, errstr);
        else
                rd_kafka_set_fatal_error0(rk, RD_DONT_LOCK, err, "%s%s",
                                          preface, errstr);

        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_FATAL_ERROR);

        return rd_true;
}



/**
 * @brief State machine for PID acquisition for the idempotent
 *        and transactional producers.
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock() MUST be held.
 */
void rd_kafka_idemp_pid_fsm(rd_kafka_t *rk) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_broker_t *rkb;
        rd_bool_t is_fatal = rd_false;

        /* If a fatal error has been raised we do not
         * attempt to acquire a PID. */
        if (unlikely(rd_kafka_fatal_error_code(rk)))
                return;

redo:
        switch (rk->rk_eos.idemp_state) {
        case RD_KAFKA_IDEMP_STATE_INIT:
        case RD_KAFKA_IDEMP_STATE_TERM:
        case RD_KAFKA_IDEMP_STATE_FATAL_ERROR:
                break;

        case RD_KAFKA_IDEMP_STATE_REQ_PID:
                /* Request (new) PID */

                /* The idempotent producer may ask any broker for a PID,
                 * while the transactional producer needs to ask its
                 * transaction coordinator for a PID. */
                if (!rd_kafka_is_transactional(rk) ||
                    rk->rk_eos.txn_curr_coord) {
                        rd_kafka_idemp_set_state(
                            rk, RD_KAFKA_IDEMP_STATE_WAIT_TRANSPORT);
                        goto redo;
                }


                /*
                 * Look up transaction coordinator.
                 * When the coordinator is known this FSM will be called again.
                 */
                if (rd_kafka_txn_coord_query(rk, "Acquire PID"))
                        return; /* Fatal error */
                break;

        case RD_KAFKA_IDEMP_STATE_WAIT_TRANSPORT:
                /* Waiting for broker/coordinator to become available */
                if (rd_kafka_is_transactional(rk)) {
                        /* Check that a proper coordinator broker has
                         * been assigned by inspecting txn_curr_coord
                         * (the real broker) rather than txn_coord
                         * (the logical broker). */
                        if (!rk->rk_eos.txn_curr_coord) {
                                /*
                                 * Can happen if the coordinator wasn't set or
                                 * wasn't up initially and has been set to NULL
                                 * after a COORDINATOR_NOT_AVAILABLE error in
                                 * FindCoordinatorResponse. When the coordinator
                                 * is known this FSM will be called again.
                                 */
                                rd_kafka_txn_coord_query(
                                    rk, "Awaiting coordinator");
                                return;
                        }
                        rkb = rk->rk_eos.txn_coord;
                        rd_kafka_broker_keep(rkb);

                } else {
                        rkb = rd_kafka_idemp_broker_any(rk, &err, errstr,
                                                        sizeof(errstr));

                        if (!rkb && rd_kafka_idemp_check_error(rk, err, errstr,
                                                               rd_false))
                                return; /* Fatal error */
                }

                if (!rkb || !rd_kafka_broker_is_up(rkb)) {
                        /* The coordinator broker monitor will re-trigger
                         * the fsm sooner if txn_coord has a state change,
                         * else rely on the timer to retry. */
                        rd_kafka_idemp_pid_timer_restart(
                            rk, rd_false,
                            rkb ? "No broker available" : "Coordinator not up");

                        if (rkb)
                                rd_kafka_broker_destroy(rkb);
                        return;
                }

                if (rd_kafka_is_transactional(rk)) {
                        int err_of = 0;

                        /* If this is a transactional producer and the
                         * PID-epoch needs to be bumped we'll require KIP-360
                         * support on the broker, else raise a fatal error. */

                        if (rd_kafka_pid_valid(rk->rk_eos.pid)) {
                                rd_rkb_dbg(rkb, EOS, "GETPID",
                                           "Requesting ProducerId bump for %s",
                                           rd_kafka_pid2str(rk->rk_eos.pid));
                                err_of = rd_snprintf(errstr, sizeof(errstr),
                                                     "Failed to request "
                                                     "ProducerId bump: ");
                                rd_assert(err_of < 0 ||
                                          err_of < (int)sizeof(errstr));
                        } else {
                                rd_rkb_dbg(rkb, EOS, "GETPID",
                                           "Acquiring ProducerId");
                        }

                        err = rd_kafka_InitProducerIdRequest(
                            rkb, rk->rk_conf.eos.transactional_id,
                            rk->rk_conf.eos.transaction_timeout_ms,
                            rd_kafka_pid_valid(rk->rk_eos.pid) ? &rk->rk_eos.pid
                                                               : NULL,
                            errstr + err_of, sizeof(errstr) - err_of,
                            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                            rd_kafka_handle_InitProducerId, NULL);

                        if (err == RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE &&
                            rd_kafka_pid_valid(rk->rk_eos.pid))
                                is_fatal = rd_true;
                } else {
                        rd_rkb_dbg(rkb, EOS, "GETPID", "Acquiring ProducerId");

                        err = rd_kafka_InitProducerIdRequest(
                            rkb, NULL, -1, NULL, errstr, sizeof(errstr),
                            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                            rd_kafka_handle_InitProducerId, NULL);
                }

                if (err) {
                        rd_rkb_dbg(rkb, EOS, "GETPID",
                                   "Can't acquire ProducerId from "
                                   "this broker: %s",
                                   errstr);
                }

                rd_kafka_broker_destroy(rkb);

                if (err) {
                        if (rd_kafka_idemp_check_error(rk, err, errstr,
                                                       is_fatal))
                                return; /* Fatal error */

                        /* The coordinator broker monitor will re-trigger
                         * the fsm sooner if txn_coord has a state change,
                         * else rely on the timer to retry. */
                        rd_kafka_idemp_pid_timer_restart(rk, rd_false, errstr);
                        return;
                }

                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_WAIT_PID);
                break;

        case RD_KAFKA_IDEMP_STATE_WAIT_PID:
                /* PID requested, waiting for reply */
                break;

        case RD_KAFKA_IDEMP_STATE_ASSIGNED:
                /* New PID assigned */
                break;

        case RD_KAFKA_IDEMP_STATE_DRAIN_RESET:
                /* Wait for outstanding ProduceRequests to finish
                 * before resetting and re-requesting a new PID. */
                break;

        case RD_KAFKA_IDEMP_STATE_DRAIN_BUMP:
                /* Wait for outstanding ProduceRequests to finish
                 * before bumping the current epoch. */
                break;

        case RD_KAFKA_IDEMP_STATE_WAIT_TXN_ABORT:
                /* Wait for txnmgr to abort its current transaction
                 * and then trigger a drain & reset or bump. */
                break;
        }
}


/**
 * @brief Timed PID retrieval timer callback.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_idemp_pid_timer_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_wrlock(rk);
        rd_kafka_idemp_pid_fsm(rk);
        rd_kafka_wrunlock(rk);
}


/**
 * @brief Restart the pid retrieval timer.
 *
 * @param immediate If true, request a pid as soon as possible,
 *                  else use the default interval (500ms).
 * @locality any
 * @locks none
 */
static void rd_kafka_idemp_pid_timer_restart(rd_kafka_t *rk,
                                             rd_bool_t immediate,
                                             const char *reason) {
        rd_kafka_dbg(rk, EOS, "TXN", "Starting PID FSM timer%s: %s",
                     immediate ? " (fire immediately)" : "", reason);
        rd_kafka_timer_start_oneshot(&rk->rk_timers, &rk->rk_eos.pid_tmr,
                                     rd_true,
                                     1000 * (immediate ? 1 : 500 /*500ms*/),
                                     rd_kafka_idemp_pid_timer_cb, rk);
}


/**
 * @brief Handle failure to acquire a PID from broker.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_idemp_request_pid_failed(rd_kafka_broker_t *rkb,
                                       rd_kafka_resp_err_t err) {
        rd_kafka_t *rk = rkb->rkb_rk;
        char errstr[512];

        rd_rkb_dbg(rkb, EOS, "GETPID", "Failed to acquire PID: %s",
                   rd_kafka_err2str(err));

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return; /* Ignore */

        rd_assert(thrd_is_current(rk->rk_thread));

        rd_snprintf(errstr, sizeof(errstr),
                    "Failed to acquire %s PID from broker %s: %s",
                    rd_kafka_is_transactional(rk) ? "transactional"
                                                  : "idempotence",
                    rd_kafka_broker_name(rkb), rd_kafka_err2str(err));

        rd_kafka_wrlock(rk);

        if (rd_kafka_idemp_check_error(rk, err, errstr, rd_false)) {
                rd_kafka_wrunlock(rk);
                return; /* Fatal error */
        }

        RD_UT_COVERAGE(0);

        if (rd_kafka_is_transactional(rk) &&
            (err == RD_KAFKA_RESP_ERR_NOT_COORDINATOR ||
             err == RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE))
                rd_kafka_txn_coord_set(rk, NULL, "%s", errstr);

        /* This error code is read by init_transactions() for propagation
         * to the application. */
        rk->rk_eos.txn_init_err = err;

        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_REQ_PID);

        rd_kafka_wrunlock(rk);

        rd_kafka_log(rk, LOG_WARNING, "GETPID", "%s: retrying", errstr);

        /* Restart acquisition after a short wait */
        rd_kafka_idemp_pid_timer_restart(rk, rd_false, errstr);
}


/**
 * @brief Update Producer ID from InitProducerId response.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_idemp_pid_update(rd_kafka_broker_t *rkb,
                               const rd_kafka_pid_t pid) {
        rd_kafka_t *rk = rkb->rkb_rk;

        rd_kafka_wrlock(rk);
        if (rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_WAIT_PID) {
                rd_rkb_dbg(rkb, EOS, "GETPID",
                           "Ignoring InitProduceId response (%s) "
                           "in state %s",
                           rd_kafka_pid2str(pid),
                           rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                rd_kafka_wrunlock(rk);
                return;
        }

        if (!rd_kafka_pid_valid(pid)) {
                rd_kafka_wrunlock(rk);
                rd_rkb_log(rkb, LOG_WARNING, "GETPID",
                           "Acquired invalid PID{%" PRId64 ",%hd}: ignoring",
                           pid.id, pid.epoch);
                rd_kafka_idemp_request_pid_failed(rkb,
                                                  RD_KAFKA_RESP_ERR__BAD_MSG);
                return;
        }

        if (rd_kafka_pid_valid(rk->rk_eos.pid))
                rd_kafka_dbg(rk, EOS, "GETPID", "Acquired %s (previous %s)",
                             rd_kafka_pid2str(pid),
                             rd_kafka_pid2str(rk->rk_eos.pid));
        else
                rd_kafka_dbg(rk, EOS, "GETPID", "Acquired %s",
                             rd_kafka_pid2str(pid));
        rk->rk_eos.pid = pid;
        rk->rk_eos.epoch_cnt++;

        /* The idempotence state change will trigger the transaction manager,
         * see rd_kafka_txn_idemp_state_change(). */
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_ASSIGNED);

        rd_kafka_wrunlock(rk);

        /* Wake up all broker threads (that may have messages to send
         * that were waiting for a Producer ID). */
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                    "PID updated");
}


/**
 * @brief Call when all partition request queues
 *        are drained to reset and re-request a new PID.
 *
 * @locality any
 * @locks none
 */
static void rd_kafka_idemp_drain_done(rd_kafka_t *rk) {
        rd_bool_t restart_tmr    = rd_false;
        rd_bool_t wakeup_brokers = rd_false;

        rd_kafka_wrlock(rk);
        if (rk->rk_eos.idemp_state == RD_KAFKA_IDEMP_STATE_DRAIN_RESET) {
                rd_kafka_dbg(rk, EOS, "DRAIN", "All partitions drained");
                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_REQ_PID);
                restart_tmr = rd_true;

        } else if (rk->rk_eos.idemp_state == RD_KAFKA_IDEMP_STATE_DRAIN_BUMP &&
                   rd_kafka_pid_valid(rk->rk_eos.pid)) {

                if (rd_kafka_is_transactional(rk)) {
                        /* The epoch bump needs to be performed by the
                         * coordinator by sending it an InitPid request. */
                        rd_kafka_dbg(rk, EOS, "DRAIN",
                                     "All partitions drained, asking "
                                     "coordinator to bump epoch (currently %s)",
                                     rd_kafka_pid2str(rk->rk_eos.pid));
                        rd_kafka_idemp_set_state(rk,
                                                 RD_KAFKA_IDEMP_STATE_REQ_PID);
                        restart_tmr = rd_true;

                } else {
                        /* The idempotent producer can bump its own epoch */
                        rk->rk_eos.pid = rd_kafka_pid_bump(rk->rk_eos.pid);
                        rd_kafka_dbg(rk, EOS, "DRAIN",
                                     "All partitions drained, bumped "
                                     "epoch to %s",
                                     rd_kafka_pid2str(rk->rk_eos.pid));
                        rd_kafka_idemp_set_state(rk,
                                                 RD_KAFKA_IDEMP_STATE_ASSIGNED);
                        wakeup_brokers = rd_true;
                }
        }
        rd_kafka_wrunlock(rk);

        /* Restart timer to eventually trigger a re-request */
        if (restart_tmr)
                rd_kafka_idemp_pid_timer_restart(rk, rd_true, "Drain done");

        /* Wake up all broker threads (that may have messages to send
         * that were waiting for a Producer ID). */
        if (wakeup_brokers)
                rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                            "message drain done");
}

/**
 * @brief Check if in-flight toppars drain is done, if so transition to
 *        next state.
 *
 * @locality any
 * @locks none
 */
static RD_INLINE void rd_kafka_idemp_check_drain_done(rd_kafka_t *rk) {
        if (rd_atomic32_get(&rk->rk_eos.inflight_toppar_cnt) == 0)
                rd_kafka_idemp_drain_done(rk);
}


/**
 * @brief Schedule a reset and re-request of PID when the
 *        local ProduceRequest queues have been fully drained.
 *
 * The PID is not reset until the queues are fully drained.
 *
 * @locality any
 * @locks none
 */
void rd_kafka_idemp_drain_reset(rd_kafka_t *rk, const char *reason) {
        rd_kafka_wrlock(rk);
        rd_kafka_dbg(rk, EOS, "DRAIN",
                     "Beginning partition drain for %s reset "
                     "for %d partition(s) with in-flight requests: %s",
                     rd_kafka_pid2str(rk->rk_eos.pid),
                     rd_atomic32_get(&rk->rk_eos.inflight_toppar_cnt), reason);
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_DRAIN_RESET);
        rd_kafka_wrunlock(rk);

        /* Check right away if the drain could be done. */
        rd_kafka_idemp_check_drain_done(rk);
}


/**
 * @brief Schedule an epoch bump when the local ProduceRequest queues
 *        have been fully drained.
 *
 * The PID is not bumped until the queues are fully drained and the current
 * transaction is aborted (if any).
 *
 * @param allow_txn_abort If this is a transactional producer and this flag is
 *                        true then we trigger an abortable txn error to abort
 *                        the current transaction first. The txnmgr will later
 *                        call us back with this flag set to false to go ahead
 *                        with the epoch bump.
 * @param fmt is a human-readable reason for the bump
 *
 *
 * @locality any
 * @locks none
 */
void rd_kafka_idemp_drain_epoch_bump0(rd_kafka_t *rk,
                                      rd_bool_t allow_txn_abort,
                                      rd_kafka_resp_err_t err,
                                      const char *fmt,
                                      ...) {
        va_list ap;
        char buf[256];
        rd_bool_t requires_txn_abort =
            allow_txn_abort && rd_kafka_is_transactional(rk);

        va_start(ap, fmt);
        rd_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        rd_kafka_wrlock(rk);


        if (requires_txn_abort) {
                rd_kafka_dbg(rk, EOS, "DRAIN",
                             "Need transaction abort before beginning "
                             "partition drain in state %s for %s epoch bump "
                             "for %d partition(s) with in-flight requests: %s",
                             rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                             rd_kafka_pid2str(rk->rk_eos.pid),
                             rd_atomic32_get(&rk->rk_eos.inflight_toppar_cnt),
                             buf);
                rd_kafka_idemp_set_state(rk,
                                         RD_KAFKA_IDEMP_STATE_WAIT_TXN_ABORT);

        } else {
                rd_kafka_dbg(rk, EOS, "DRAIN",
                             "Beginning partition drain in state %s "
                             "for %s epoch bump "
                             "for %d partition(s) with in-flight requests: %s",
                             rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                             rd_kafka_pid2str(rk->rk_eos.pid),
                             rd_atomic32_get(&rk->rk_eos.inflight_toppar_cnt),
                             buf);

                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_DRAIN_BUMP);
        }

        rd_kafka_wrunlock(rk);

        if (requires_txn_abort) {
                /* Transactions: bumping the epoch requires the current
                 *               transaction to be aborted first. */
                rd_kafka_txn_set_abortable_error_with_bump(rk, err, "%s", buf);

        } else {
                /* Idempotent producer: check right away if the drain could
                 *                      be done. */
                rd_kafka_idemp_check_drain_done(rk);
        }
}

/**
 * @brief Mark partition as waiting-to-drain.
 *
 * @locks toppar_lock MUST be held
 * @locality broker thread (leader or not)
 */
void rd_kafka_idemp_drain_toppar(rd_kafka_toppar_t *rktp, const char *reason) {
        if (rktp->rktp_eos.wait_drain)
                return;

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, EOS | RD_KAFKA_DBG_TOPIC, "DRAIN",
                     "%.*s [%" PRId32 "] beginning partition drain: %s",
                     RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                     rktp->rktp_partition, reason);
        rktp->rktp_eos.wait_drain = rd_true;
}


/**
 * @brief Mark partition as no longer having a ProduceRequest in-flight.
 *
 * @locality any
 * @locks none
 */
void rd_kafka_idemp_inflight_toppar_sub(rd_kafka_t *rk,
                                        rd_kafka_toppar_t *rktp) {
        int r = rd_atomic32_sub(&rk->rk_eos.inflight_toppar_cnt, 1);

        if (r == 0) {
                /* Check if we're waiting for the partitions to drain
                 * before resetting the PID, and if so trigger a reset
                 * since this was the last drained one. */
                rd_kafka_idemp_drain_done(rk);
        } else {
                rd_assert(r >= 0);
        }
}


/**
 * @brief Mark partition as having a ProduceRequest in-flight.
 *
 * @locality toppar handler thread
 * @locks none
 */
void rd_kafka_idemp_inflight_toppar_add(rd_kafka_t *rk,
                                        rd_kafka_toppar_t *rktp) {
        rd_atomic32_add(&rk->rk_eos.inflight_toppar_cnt, 1);
}



/**
 * @brief Start idempotent producer (asynchronously).
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_idemp_start(rd_kafka_t *rk, rd_bool_t immediate) {

        if (rd_kafka_terminating(rk))
                return;

        rd_kafka_wrlock(rk);
        /* Don't restart PID acquisition if there's already an outstanding
         * request. */
        if (rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_WAIT_PID)
                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_REQ_PID);
        rd_kafka_wrunlock(rk);

        /* Schedule request timer */
        rd_kafka_idemp_pid_timer_restart(rk, immediate,
                                         "Starting idempotent producer");
}


/**
 * @brief Initialize the idempotent producer.
 *
 * @remark Must be called from rd_kafka_new() and only once.
 * @locality rdkafka main thread
 * @locks none / not needed from rd_kafka_new()
 */
void rd_kafka_idemp_init(rd_kafka_t *rk) {
        rd_assert(thrd_is_current(rk->rk_thread));

        rd_atomic32_init(&rk->rk_eos.inflight_toppar_cnt, 0);
        rd_kafka_pid_reset(&rk->rk_eos.pid);

        /* The transactional producer acquires the PID
         * from init_transactions(), for non-transactional producers
         * the PID can be acquired right away. */
        if (rd_kafka_is_transactional(rk))
                rd_kafka_txns_init(rk);
        else
                /* There are no available brokers this early,
                 * so just set the state to indicate that we want to
                 * acquire a PID as soon as possible and start
                 * the timer. */
                rd_kafka_idemp_start(rk, rd_false /*non-immediate*/);
}


/**
 * @brief Terminate and clean up idempotent producer
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock() MUST be held
 */
void rd_kafka_idemp_term(rd_kafka_t *rk) {
        rd_assert(thrd_is_current(rk->rk_thread));

        rd_kafka_wrlock(rk);
        if (rd_kafka_is_transactional(rk))
                rd_kafka_txns_term(rk);
        rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_TERM);
        rd_kafka_wrunlock(rk);
        rd_kafka_timer_stop(&rk->rk_timers, &rk->rk_eos.pid_tmr, 1);
}
