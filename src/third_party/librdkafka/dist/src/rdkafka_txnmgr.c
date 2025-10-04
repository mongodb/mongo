/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2019-2022, Magnus Edenhill
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

/**
 * @name Transaction Manager
 *
 */

#include <stdarg.h>

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_txnmgr.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_request.h"
#include "rdkafka_error.h"
#include "rdunittest.h"
#include "rdrand.h"


static void rd_kafka_txn_coord_timer_start(rd_kafka_t *rk, int timeout_ms);

#define rd_kafka_txn_curr_api_set_result(rk, actions, error)                   \
        rd_kafka_txn_curr_api_set_result0(__FUNCTION__, __LINE__, rk, actions, \
                                          error)
static void rd_kafka_txn_curr_api_set_result0(const char *func,
                                              int line,
                                              rd_kafka_t *rk,
                                              int actions,
                                              rd_kafka_error_t *error);



/**
 * @return a normalized error code, this for instance abstracts different
 *         fencing errors to return one single fencing error to the application.
 */
static rd_kafka_resp_err_t rd_kafka_txn_normalize_err(rd_kafka_resp_err_t err) {

        switch (err) {
        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
        case RD_KAFKA_RESP_ERR_PRODUCER_FENCED:
                return RD_KAFKA_RESP_ERR__FENCED;
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
                return RD_KAFKA_RESP_ERR__TIMED_OUT;
        default:
                return err;
        }
}


/**
 * @brief Ensure client is configured as a transactional producer,
 *        else return error.
 *
 * @locality application thread
 * @locks none
 */
static RD_INLINE rd_kafka_error_t *
rd_kafka_ensure_transactional(const rd_kafka_t *rk) {
        if (unlikely(rk->rk_type != RD_KAFKA_PRODUCER))
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "The Transactional API can only be used "
                    "on producer instances");

        if (unlikely(!rk->rk_conf.eos.transactional_id))
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__NOT_CONFIGURED,
                                          "The Transactional API requires "
                                          "transactional.id to be configured");

        return NULL;
}



/**
 * @brief Ensure transaction state is one of \p states.
 *
 * @param the required states, ended by a -1 sentinel.
 *
 * @locks_required rd_kafka_*lock(rk) MUST be held
 * @locality any
 */
static RD_INLINE rd_kafka_error_t *
rd_kafka_txn_require_states0(rd_kafka_t *rk, rd_kafka_txn_state_t states[]) {
        rd_kafka_error_t *error;
        size_t i;

        if (unlikely((error = rd_kafka_ensure_transactional(rk)) != NULL))
                return error;

        for (i = 0; (int)states[i] != -1; i++)
                if (rk->rk_eos.txn_state == states[i])
                        return NULL;

        /* For fatal and abortable states return the last transactional
         * error, for all other states just return a state error. */
        if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_FATAL_ERROR)
                error = rd_kafka_error_new_fatal(rk->rk_eos.txn_err, "%s",
                                                 rk->rk_eos.txn_errstr);
        else if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_ABORTABLE_ERROR) {
                error = rd_kafka_error_new(rk->rk_eos.txn_err, "%s",
                                           rk->rk_eos.txn_errstr);
                rd_kafka_error_set_txn_requires_abort(error);
        } else
                error = rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__STATE, "Operation not valid in state %s",
                    rd_kafka_txn_state2str(rk->rk_eos.txn_state));


        return error;
}

/** @brief \p ... is a list of states */
#define rd_kafka_txn_require_state(rk, ...)                                    \
        rd_kafka_txn_require_states0(                                          \
            rk, (rd_kafka_txn_state_t[]) {__VA_ARGS__, -1})



/**
 * @param ignore Will be set to true if the state transition should be
 *               completely ignored.
 * @returns true if the state transition is valid, else false.
 */
static rd_bool_t
rd_kafka_txn_state_transition_is_valid(rd_kafka_txn_state_t curr,
                                       rd_kafka_txn_state_t new_state,
                                       rd_bool_t *ignore) {

        *ignore = rd_false;

        switch (new_state) {
        case RD_KAFKA_TXN_STATE_INIT:
                /* This is the initialized value and this transition will
                 * never happen. */
                return rd_false;

        case RD_KAFKA_TXN_STATE_WAIT_PID:
                return curr == RD_KAFKA_TXN_STATE_INIT;

        case RD_KAFKA_TXN_STATE_READY_NOT_ACKED:
                return curr == RD_KAFKA_TXN_STATE_WAIT_PID;

        case RD_KAFKA_TXN_STATE_READY:
                return curr == RD_KAFKA_TXN_STATE_READY_NOT_ACKED ||
                       curr == RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED ||
                       curr == RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED;

        case RD_KAFKA_TXN_STATE_IN_TRANSACTION:
                return curr == RD_KAFKA_TXN_STATE_READY;

        case RD_KAFKA_TXN_STATE_BEGIN_COMMIT:
                return curr == RD_KAFKA_TXN_STATE_IN_TRANSACTION;

        case RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION:
                return curr == RD_KAFKA_TXN_STATE_BEGIN_COMMIT;

        case RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED:
                return curr == RD_KAFKA_TXN_STATE_BEGIN_COMMIT ||
                       curr == RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION;

        case RD_KAFKA_TXN_STATE_BEGIN_ABORT:
                return curr == RD_KAFKA_TXN_STATE_IN_TRANSACTION ||
                       curr == RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION ||
                       curr == RD_KAFKA_TXN_STATE_ABORTABLE_ERROR;

        case RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION:
                return curr == RD_KAFKA_TXN_STATE_BEGIN_ABORT;

        case RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED:
                return curr == RD_KAFKA_TXN_STATE_BEGIN_ABORT ||
                       curr == RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION;

        case RD_KAFKA_TXN_STATE_ABORTABLE_ERROR:
                if (curr == RD_KAFKA_TXN_STATE_BEGIN_ABORT ||
                    curr == RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION ||
                    curr == RD_KAFKA_TXN_STATE_FATAL_ERROR) {
                        /* Ignore sub-sequent abortable errors in
                         * these states. */
                        *ignore = rd_true;
                        return 1;
                }

                return curr == RD_KAFKA_TXN_STATE_IN_TRANSACTION ||
                       curr == RD_KAFKA_TXN_STATE_BEGIN_COMMIT ||
                       curr == RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION;

        case RD_KAFKA_TXN_STATE_FATAL_ERROR:
                /* Any state can transition to a fatal error */
                return rd_true;

        default:
                RD_BUG("Invalid txn state transition: %s -> %s",
                       rd_kafka_txn_state2str(curr),
                       rd_kafka_txn_state2str(new_state));
                return rd_false;
        }
}


/**
 * @brief Transition the transaction state to \p new_state.
 *
 * @returns 0 on success or an error code if the state transition
 *          was invalid.
 *
 * @locality rdkafka main thread
 * @locks_required rd_kafka_wrlock MUST be held
 */
static void rd_kafka_txn_set_state(rd_kafka_t *rk,
                                   rd_kafka_txn_state_t new_state) {
        rd_bool_t ignore;

        if (rk->rk_eos.txn_state == new_state)
                return;

        /* Check if state transition is valid */
        if (!rd_kafka_txn_state_transition_is_valid(rk->rk_eos.txn_state,
                                                    new_state, &ignore)) {
                rd_kafka_log(rk, LOG_CRIT, "TXNSTATE",
                             "BUG: Invalid transaction state transition "
                             "attempted: %s -> %s",
                             rd_kafka_txn_state2str(rk->rk_eos.txn_state),
                             rd_kafka_txn_state2str(new_state));

                rd_assert(!*"BUG: Invalid transaction state transition");
        }

        if (ignore) {
                /* Ignore this state change */
                return;
        }

        rd_kafka_dbg(rk, EOS, "TXNSTATE", "Transaction state change %s -> %s",
                     rd_kafka_txn_state2str(rk->rk_eos.txn_state),
                     rd_kafka_txn_state2str(new_state));

        /* If transitioning from IN_TRANSACTION, the app is no longer
         * allowed to enqueue (produce) messages. */
        if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_IN_TRANSACTION)
                rd_atomic32_set(&rk->rk_eos.txn_may_enq, 0);
        else if (new_state == RD_KAFKA_TXN_STATE_IN_TRANSACTION)
                rd_atomic32_set(&rk->rk_eos.txn_may_enq, 1);

        rk->rk_eos.txn_state = new_state;
}


/**
 * @returns the current transaction timeout, i.e., the time remaining in
 *          the current transaction.
 *
 * @remark The remaining timeout is currently not tracked, so this function
 *         will always return the remaining time based on transaction.timeout.ms
 *         and we rely on the broker to enforce the actual remaining timeout.
 *         This is still better than not having a timeout cap at all, which
 *         used to be the case.
 *         It's also tricky knowing exactly what the controller thinks the
 *         remaining transaction time is.
 *
 * @locks_required rd_kafka_*lock(rk) MUST be held.
 */
static RD_INLINE rd_ts_t rd_kafka_txn_current_timeout(const rd_kafka_t *rk) {
        return rd_timeout_init(rk->rk_conf.eos.transaction_timeout_ms);
}


/**
 * @brief An unrecoverable transactional error has occurred.
 *
 * @param do_lock RD_DO_LOCK: rd_kafka_wrlock(rk) will be acquired and released,
 *                RD_DONT_LOCK: rd_kafka_wrlock(rk) MUST be held by the caller.
 * @locality any
 * @locks rd_kafka_wrlock MUST NOT be held
 */
void rd_kafka_txn_set_fatal_error(rd_kafka_t *rk,
                                  rd_dolock_t do_lock,
                                  rd_kafka_resp_err_t err,
                                  const char *fmt,
                                  ...) {
        char errstr[512];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(errstr, sizeof(errstr), fmt, ap);
        va_end(ap);

        rd_kafka_log(rk, LOG_ALERT, "TXNERR",
                     "Fatal transaction error: %s (%s)", errstr,
                     rd_kafka_err2name(err));

        if (do_lock)
                rd_kafka_wrlock(rk);
        rd_kafka_set_fatal_error0(rk, RD_DONT_LOCK, err, "%s", errstr);

        rk->rk_eos.txn_err = err;
        if (rk->rk_eos.txn_errstr)
                rd_free(rk->rk_eos.txn_errstr);
        rk->rk_eos.txn_errstr = rd_strdup(errstr);

        rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_FATAL_ERROR);

        if (do_lock)
                rd_kafka_wrunlock(rk);

        /* If application has called a transactional API and
         * it has now failed, reply to the app.
         * If there is no currently called API then this is a no-op. */
        rd_kafka_txn_curr_api_set_result(
            rk, 0, rd_kafka_error_new_fatal(err, "%s", errstr));
}


/**
 * @brief An abortable/recoverable transactional error has occured.
 *
 * @param requires_epoch_bump If true; abort_transaction() will bump the epoch
 *                            on the coordinator (KIP-360).

 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock MUST NOT be held
 */
void rd_kafka_txn_set_abortable_error0(rd_kafka_t *rk,
                                       rd_kafka_resp_err_t err,
                                       rd_bool_t requires_epoch_bump,
                                       const char *fmt,
                                       ...) {
        char errstr[512];
        va_list ap;

        if (rd_kafka_fatal_error(rk, NULL, 0)) {
                rd_kafka_dbg(rk, EOS, "FATAL",
                             "Not propagating abortable transactional "
                             "error (%s) "
                             "since previous fatal error already raised",
                             rd_kafka_err2name(err));
                return;
        }

        va_start(ap, fmt);
        vsnprintf(errstr, sizeof(errstr), fmt, ap);
        va_end(ap);

        rd_kafka_wrlock(rk);

        if (requires_epoch_bump)
                rk->rk_eos.txn_requires_epoch_bump = requires_epoch_bump;

        if (rk->rk_eos.txn_err) {
                rd_kafka_dbg(rk, EOS, "TXNERR",
                             "Ignoring sub-sequent abortable transaction "
                             "error: %s (%s): "
                             "previous error (%s) already raised",
                             errstr, rd_kafka_err2name(err),
                             rd_kafka_err2name(rk->rk_eos.txn_err));
                rd_kafka_wrunlock(rk);
                return;
        }

        rk->rk_eos.txn_err = err;
        if (rk->rk_eos.txn_errstr)
                rd_free(rk->rk_eos.txn_errstr);
        rk->rk_eos.txn_errstr = rd_strdup(errstr);

        rd_kafka_log(rk, LOG_ERR, "TXNERR",
                     "Current transaction failed in state %s: %s (%s%s)",
                     rd_kafka_txn_state2str(rk->rk_eos.txn_state), errstr,
                     rd_kafka_err2name(err),
                     requires_epoch_bump ? ", requires epoch bump" : "");

        rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_ABORTABLE_ERROR);
        rd_kafka_wrunlock(rk);

        /* Purge all messages in queue/flight */
        rd_kafka_purge(rk, RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_ABORT_TXN |
                               RD_KAFKA_PURGE_F_NON_BLOCKING);
}



/**
 * @brief Send request-reply op to txnmgr callback, waits for a reply
 *        or timeout, and returns an error object or NULL on success.
 *
 * @remark Does not alter the current API state.
 *
 * @returns an error object on failure, else NULL.
 *
 * @locality application thread
 *
 * @locks_acquired rk->rk_eos.txn_curr_api.lock
 */
#define rd_kafka_txn_op_req(rk, op_cb, abs_timeout)                            \
        rd_kafka_txn_op_req0(__FUNCTION__, __LINE__, rk,                       \
                             rd_kafka_op_new_cb(rk, RD_KAFKA_OP_TXN, op_cb),   \
                             abs_timeout)
#define rd_kafka_txn_op_req1(rk, rko, abs_timeout)                             \
        rd_kafka_txn_op_req0(__FUNCTION__, __LINE__, rk, rko, abs_timeout)
static rd_kafka_error_t *rd_kafka_txn_op_req0(const char *func,
                                              int line,
                                              rd_kafka_t *rk,
                                              rd_kafka_op_t *rko,
                                              rd_ts_t abs_timeout) {
        rd_kafka_error_t *error = NULL;
        rd_bool_t has_result    = rd_false;

        mtx_lock(&rk->rk_eos.txn_curr_api.lock);

        /* See if there's already a result, if so return that immediately. */
        if (rk->rk_eos.txn_curr_api.has_result) {
                error                         = rk->rk_eos.txn_curr_api.error;
                rk->rk_eos.txn_curr_api.error = NULL;
                rk->rk_eos.txn_curr_api.has_result = rd_false;
                mtx_unlock(&rk->rk_eos.txn_curr_api.lock);
                rd_kafka_op_destroy(rko);
                rd_kafka_dbg(rk, EOS, "OPREQ",
                             "%s:%d: %s: returning already set result: %s",
                             func, line, rk->rk_eos.txn_curr_api.name,
                             error ? rd_kafka_error_string(error) : "Success");
                return error;
        }

        /* Send one-way op to txnmgr */
        if (!rd_kafka_q_enq(rk->rk_ops, rko))
                RD_BUG("rk_ops queue disabled");

        /* Wait for result to be set, or timeout */
        do {
                if (cnd_timedwait_ms(&rk->rk_eos.txn_curr_api.cnd,
                                     &rk->rk_eos.txn_curr_api.lock,
                                     rd_timeout_remains(abs_timeout)) ==
                    thrd_timedout)
                        break;
        } while (!rk->rk_eos.txn_curr_api.has_result);



        if ((has_result = rk->rk_eos.txn_curr_api.has_result)) {
                rk->rk_eos.txn_curr_api.has_result = rd_false;
                error                         = rk->rk_eos.txn_curr_api.error;
                rk->rk_eos.txn_curr_api.error = NULL;
        }

        mtx_unlock(&rk->rk_eos.txn_curr_api.lock);

        /* If there was no reply it means the background operation is still
         * in progress and its result will be set later, so the application
         * should call this API again to resume. */
        if (!has_result) {
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__TIMED_OUT,
                    "Timed out waiting for operation to finish, "
                    "retry call to resume");
        }

        return error;
}


/**
 * @brief Begin (or resume) a public API call.
 *
 * This function will prevent conflicting calls.
 *
 * @returns an error on failure, or NULL on success.
 *
 * @locality application thread
 *
 * @locks_acquired rk->rk_eos.txn_curr_api.lock
 */
static rd_kafka_error_t *rd_kafka_txn_curr_api_begin(rd_kafka_t *rk,
                                                     const char *api_name,
                                                     rd_bool_t cap_timeout,
                                                     int timeout_ms,
                                                     rd_ts_t *abs_timeoutp) {
        rd_kafka_error_t *error = NULL;

        if ((error = rd_kafka_ensure_transactional(rk)))
                return error;

        rd_kafka_rdlock(rk); /* Need lock for retrieving the states */
        rd_kafka_dbg(rk, EOS, "TXNAPI",
                     "Transactional API called: %s "
                     "(in txn state %s, idemp state %s, API timeout %d)",
                     api_name, rd_kafka_txn_state2str(rk->rk_eos.txn_state),
                     rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                     timeout_ms);
        rd_kafka_rdunlock(rk);

        mtx_lock(&rk->rk_eos.txn_curr_api.lock);


        /* Make sure there is no other conflicting in-progress API call,
         * and that this same call is not currently under way in another thread.
         */
        if (unlikely(*rk->rk_eos.txn_curr_api.name &&
                     strcmp(rk->rk_eos.txn_curr_api.name, api_name))) {
                /* Another API is being called. */
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__CONFLICT,
                    "Conflicting %s API call is already in progress",
                    rk->rk_eos.txn_curr_api.name);

        } else if (unlikely(rk->rk_eos.txn_curr_api.calling)) {
                /* There is an active call to this same API
                 * from another thread. */
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS,
                    "Simultaneous %s API calls not allowed",
                    rk->rk_eos.txn_curr_api.name);

        } else if (*rk->rk_eos.txn_curr_api.name) {
                /* Resumed call */
                rk->rk_eos.txn_curr_api.calling = rd_true;

        } else {
                /* New call */
                rd_snprintf(rk->rk_eos.txn_curr_api.name,
                            sizeof(rk->rk_eos.txn_curr_api.name), "%s",
                            api_name);
                rk->rk_eos.txn_curr_api.calling = rd_true;
                rd_assert(!rk->rk_eos.txn_curr_api.error);
        }

        if (!error && abs_timeoutp) {
                rd_ts_t abs_timeout = rd_timeout_init(timeout_ms);

                if (cap_timeout) {
                        /* Cap API timeout to remaining transaction timeout */
                        rd_ts_t abs_txn_timeout =
                            rd_kafka_txn_current_timeout(rk);
                        if (abs_timeout > abs_txn_timeout ||
                            abs_timeout == RD_POLL_INFINITE)
                                abs_timeout = abs_txn_timeout;
                }

                *abs_timeoutp = abs_timeout;
        }

        mtx_unlock(&rk->rk_eos.txn_curr_api.lock);

        return error;
}



/**
 * @brief Return from public API.
 *
 * This function updates the current API state and must be used in
 * all return statements from the public txn API.
 *
 * @param resumable If true and the error is retriable, the current API state
 *                  will be maintained to allow a future call to the same API
 *                  to resume the background operation that is in progress.
 * @param error The error object, if not NULL, is simply inspected and returned.
 *
 * @returns the \p error object as-is.
 *
 * @locality application thread
 * @locks_acquired rk->rk_eos.txn_curr_api.lock
 */
#define rd_kafka_txn_curr_api_return(rk, resumable, error)                     \
        rd_kafka_txn_curr_api_return0(__FUNCTION__, __LINE__, rk, resumable,   \
                                      error)
static rd_kafka_error_t *
rd_kafka_txn_curr_api_return0(const char *func,
                              int line,
                              rd_kafka_t *rk,
                              rd_bool_t resumable,
                              rd_kafka_error_t *error) {

        mtx_lock(&rk->rk_eos.txn_curr_api.lock);

        rd_kafka_dbg(
            rk, EOS, "TXNAPI", "Transactional API %s return%s at %s:%d: %s",
            rk->rk_eos.txn_curr_api.name,
            resumable && rd_kafka_error_is_retriable(error) ? " resumable" : "",
            func, line, error ? rd_kafka_error_string(error) : "Success");

        rd_assert(*rk->rk_eos.txn_curr_api.name);
        rd_assert(rk->rk_eos.txn_curr_api.calling);

        rk->rk_eos.txn_curr_api.calling = rd_false;

        /* Reset the current API call so that other APIs may be called,
         * unless this is a resumable API and the error is retriable. */
        if (!resumable || (error && !rd_kafka_error_is_retriable(error))) {
                *rk->rk_eos.txn_curr_api.name = '\0';
                /* It is possible for another error to have been set,
                 * typically when a fatal error is raised, so make sure
                 * we're not destroying the error we're supposed to return. */
                if (rk->rk_eos.txn_curr_api.error != error)
                        rd_kafka_error_destroy(rk->rk_eos.txn_curr_api.error);
                rk->rk_eos.txn_curr_api.error = NULL;
        }

        mtx_unlock(&rk->rk_eos.txn_curr_api.lock);

        return error;
}



/**
 * @brief Set the (possibly intermediary) result for the current API call.
 *
 * The result is \p error NULL for success or \p error object on failure.
 * If the application is actively blocked on the call the result will be
 * sent on its replyq, otherwise the result will be stored for future retrieval
 * the next time the application calls the API again.
 *
 * @locality rdkafka main thread
 * @locks_acquired rk->rk_eos.txn_curr_api.lock
 */
static void rd_kafka_txn_curr_api_set_result0(const char *func,
                                              int line,
                                              rd_kafka_t *rk,
                                              int actions,
                                              rd_kafka_error_t *error) {

        mtx_lock(&rk->rk_eos.txn_curr_api.lock);

        if (!*rk->rk_eos.txn_curr_api.name) {
                /* No current API being called, this could happen
                 * if the application thread API deemed the API was done,
                 * or for fatal errors that attempt to set the result
                 * regardless of current API state.
                 * In this case we simply throw away this result. */
                if (error)
                        rd_kafka_error_destroy(error);
                mtx_unlock(&rk->rk_eos.txn_curr_api.lock);
                return;
        }

        rd_kafka_dbg(rk, EOS, "APIRESULT",
                     "Transactional API %s (intermediary%s) result set "
                     "at %s:%d: %s (%sprevious result%s%s)",
                     rk->rk_eos.txn_curr_api.name,
                     rk->rk_eos.txn_curr_api.calling ? ", calling" : "", func,
                     line, error ? rd_kafka_error_string(error) : "Success",
                     rk->rk_eos.txn_curr_api.has_result ? "" : "no ",
                     rk->rk_eos.txn_curr_api.error ? ": " : "",
                     rd_kafka_error_string(rk->rk_eos.txn_curr_api.error));

        rk->rk_eos.txn_curr_api.has_result = rd_true;


        if (rk->rk_eos.txn_curr_api.error) {
                /* If there's already an error it typically means
                 * a fatal error has been raised, so nothing more to do here. */
                rd_kafka_dbg(
                    rk, EOS, "APIRESULT",
                    "Transactional API %s error "
                    "already set: %s",
                    rk->rk_eos.txn_curr_api.name,
                    rd_kafka_error_string(rk->rk_eos.txn_curr_api.error));

                mtx_unlock(&rk->rk_eos.txn_curr_api.lock);

                if (error)
                        rd_kafka_error_destroy(error);

                return;
        }

        if (error) {
                if (actions & RD_KAFKA_ERR_ACTION_FATAL)
                        rd_kafka_error_set_fatal(error);
                else if (actions & RD_KAFKA_ERR_ACTION_PERMANENT)
                        rd_kafka_error_set_txn_requires_abort(error);
                else if (actions & RD_KAFKA_ERR_ACTION_RETRY)
                        rd_kafka_error_set_retriable(error);
        }

        rk->rk_eos.txn_curr_api.error = error;
        error                         = NULL;
        cnd_broadcast(&rk->rk_eos.txn_curr_api.cnd);


        mtx_unlock(&rk->rk_eos.txn_curr_api.lock);
}



/**
 * @brief The underlying idempotent producer state changed,
 *        see if this affects the transactional operations.
 *
 * @locality any thread
 * @locks rd_kafka_wrlock(rk) MUST be held
 */
void rd_kafka_txn_idemp_state_change(rd_kafka_t *rk,
                                     rd_kafka_idemp_state_t idemp_state) {
        rd_bool_t set_result = rd_false;

        if (idemp_state == RD_KAFKA_IDEMP_STATE_ASSIGNED &&
            rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_WAIT_PID) {
                /* Application is calling (or has called) init_transactions() */
                RD_UT_COVERAGE(1);
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_READY_NOT_ACKED);
                set_result = rd_true;

        } else if (idemp_state == RD_KAFKA_IDEMP_STATE_ASSIGNED &&
                   (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_BEGIN_ABORT ||
                    rk->rk_eos.txn_state ==
                        RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION)) {
                /* Application is calling abort_transaction() as we're
                 * recovering from a fatal idempotence error. */
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED);
                set_result = rd_true;

        } else if (idemp_state == RD_KAFKA_IDEMP_STATE_FATAL_ERROR &&
                   rk->rk_eos.txn_state != RD_KAFKA_TXN_STATE_FATAL_ERROR) {
                /* A fatal error has been raised. */

                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_FATAL_ERROR);
        }

        if (set_result) {
                /* Application has called init_transactions() or
                 * abort_transaction() and it is now complete,
                 * reply to the app. */
                rd_kafka_txn_curr_api_set_result(rk, 0, NULL);
        }
}


/**
 * @brief Moves a partition from the pending list to the proper list.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_partition_registered(rd_kafka_toppar_t *rktp) {
        rd_kafka_t *rk = rktp->rktp_rkt->rkt_rk;

        rd_kafka_toppar_lock(rktp);

        if (unlikely(!(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_PEND_TXN))) {
                rd_kafka_dbg(rk, EOS | RD_KAFKA_DBG_PROTOCOL, "ADDPARTS",
                             "\"%.*s\" [%" PRId32
                             "] is not in pending "
                             "list but returned in AddPartitionsToTxn "
                             "response: ignoring",
                             RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                             rktp->rktp_partition);
                rd_kafka_toppar_unlock(rktp);
                return;
        }

        rd_kafka_dbg(rk, EOS | RD_KAFKA_DBG_TOPIC, "ADDPARTS",
                     "%.*s [%" PRId32 "] registered with transaction",
                     RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                     rktp->rktp_partition);

        rd_assert((rktp->rktp_flags &
                   (RD_KAFKA_TOPPAR_F_PEND_TXN | RD_KAFKA_TOPPAR_F_IN_TXN)) ==
                  RD_KAFKA_TOPPAR_F_PEND_TXN);

        rktp->rktp_flags = (rktp->rktp_flags & ~RD_KAFKA_TOPPAR_F_PEND_TXN) |
                           RD_KAFKA_TOPPAR_F_IN_TXN;

        rd_kafka_toppar_unlock(rktp);

        mtx_lock(&rk->rk_eos.txn_pending_lock);
        TAILQ_REMOVE(&rk->rk_eos.txn_waitresp_rktps, rktp, rktp_txnlink);
        mtx_unlock(&rk->rk_eos.txn_pending_lock);

        /* Not destroy()/keep():ing rktp since it just changes tailq. */

        TAILQ_INSERT_TAIL(&rk->rk_eos.txn_rktps, rktp, rktp_txnlink);
}



/**
 * @brief Handle AddPartitionsToTxnResponse
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_handle_AddPartitionsToTxn(rd_kafka_t *rk,
                                                   rd_kafka_broker_t *rkb,
                                                   rd_kafka_resp_err_t err,
                                                   rd_kafka_buf_t *rkbuf,
                                                   rd_kafka_buf_t *request,
                                                   void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int32_t TopicCnt;
        int actions                         = 0;
        int retry_backoff_ms                = 500; /* retry backoff */
        rd_kafka_resp_err_t reset_coord_err = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_bool_t require_bump              = rd_false;

        if (err)
                goto done;

        rd_kafka_rdlock(rk);
        rd_assert(rk->rk_eos.txn_state !=
                  RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION);

        if (rk->rk_eos.txn_state != RD_KAFKA_TXN_STATE_IN_TRANSACTION &&
            rk->rk_eos.txn_state != RD_KAFKA_TXN_STATE_BEGIN_COMMIT) {
                /* Response received after aborting transaction */
                rd_rkb_dbg(rkb, EOS, "ADDPARTS",
                           "Ignoring outdated AddPartitionsToTxn response in "
                           "state %s",
                           rd_kafka_txn_state2str(rk->rk_eos.txn_state));
                rd_kafka_rdunlock(rk);
                err = RD_KAFKA_RESP_ERR__OUTDATED;
                goto done;
        }
        rd_kafka_rdunlock(rk);

        rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i32(rkbuf, &TopicCnt);

        while (TopicCnt-- > 0) {
                rd_kafkap_str_t Topic;
                rd_kafka_topic_t *rkt;
                int32_t PartCnt;
                rd_bool_t request_error = rd_false;

                rd_kafka_buf_read_str(rkbuf, &Topic);
                rd_kafka_buf_read_i32(rkbuf, &PartCnt);

                rkt = rd_kafka_topic_find0(rk, &Topic);
                if (rkt)
                        rd_kafka_topic_rdlock(rkt); /* for toppar_get() */

                while (PartCnt-- > 0) {
                        rd_kafka_toppar_t *rktp = NULL;
                        int32_t Partition;
                        int16_t ErrorCode;
                        int p_actions = 0;

                        rd_kafka_buf_read_i32(rkbuf, &Partition);
                        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

                        if (rkt)
                                rktp = rd_kafka_toppar_get(rkt, Partition,
                                                           rd_false);

                        if (!rktp) {
                                rd_rkb_dbg(rkb, EOS | RD_KAFKA_DBG_PROTOCOL,
                                           "ADDPARTS",
                                           "Unknown partition \"%.*s\" "
                                           "[%" PRId32
                                           "] in AddPartitionsToTxn "
                                           "response: ignoring",
                                           RD_KAFKAP_STR_PR(&Topic), Partition);
                                continue;
                        }

                        switch (ErrorCode) {
                        case RD_KAFKA_RESP_ERR_NO_ERROR:
                                /* Move rktp from pending to proper list */
                                rd_kafka_txn_partition_registered(rktp);
                                break;

                                /* Request-level errors.
                                 * As soon as any of these errors are seen
                                 * the rest of the partitions are ignored
                                 * since they will have the same error. */
                        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
                        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
                                reset_coord_err = ErrorCode;
                                p_actions |= RD_KAFKA_ERR_ACTION_RETRY;
                                err           = ErrorCode;
                                request_error = rd_true;
                                break;

                        case RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS:
                                retry_backoff_ms = 20;
                                /* FALLTHRU */
                        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
                        case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
                                p_actions |= RD_KAFKA_ERR_ACTION_RETRY;
                                err           = ErrorCode;
                                request_error = rd_true;
                                break;

                        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
                        case RD_KAFKA_RESP_ERR_PRODUCER_FENCED:
                        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
                        case RD_KAFKA_RESP_ERR_INVALID_TXN_STATE:
                        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
                                p_actions |= RD_KAFKA_ERR_ACTION_FATAL;
                                err           = ErrorCode;
                                request_error = rd_true;
                                break;

                        case RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID:
                        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_ID_MAPPING:
                                require_bump = rd_true;
                                p_actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                                err           = ErrorCode;
                                request_error = rd_true;
                                break;

                                /* Partition-level errors.
                                 * Continue with rest of partitions. */
                        case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
                                p_actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                                err = ErrorCode;
                                break;

                        case RD_KAFKA_RESP_ERR_OPERATION_NOT_ATTEMPTED:
                                /* Partition skipped due to other partition's
                                 * error. */
                                p_actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                                if (!err)
                                        err = ErrorCode;
                                break;

                        default:
                                /* Other partition error */
                                p_actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                                err = ErrorCode;
                                break;
                        }

                        if (ErrorCode) {
                                actions |= p_actions;

                                if (!(p_actions &
                                      (RD_KAFKA_ERR_ACTION_FATAL |
                                       RD_KAFKA_ERR_ACTION_PERMANENT)))
                                        rd_rkb_dbg(
                                            rkb, EOS, "ADDPARTS",
                                            "AddPartitionsToTxn response: "
                                            "partition \"%.*s\": "
                                            "[%" PRId32 "]: %s",
                                            RD_KAFKAP_STR_PR(&Topic), Partition,
                                            rd_kafka_err2str(ErrorCode));
                                else
                                        rd_rkb_log(rkb, LOG_ERR, "ADDPARTS",
                                                   "Failed to add partition "
                                                   "\"%.*s\" [%" PRId32
                                                   "] to "
                                                   "transaction: %s",
                                                   RD_KAFKAP_STR_PR(&Topic),
                                                   Partition,
                                                   rd_kafka_err2str(ErrorCode));
                        }

                        rd_kafka_toppar_destroy(rktp);

                        if (request_error)
                                break; /* Request-level error seen, bail out */
                }

                if (rkt) {
                        rd_kafka_topic_rdunlock(rkt);
                        rd_kafka_topic_destroy0(rkt);
                }

                if (request_error)
                        break; /* Request-level error seen, bail out */
        }

        if (actions) /* Actions set from encountered errors */
                goto done;

        /* Since these partitions are now allowed to produce
         * we wake up all broker threads. */
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                    "partitions added to transaction");

        goto done;

err_parse:
        err = rkbuf->rkbuf_err;
        actions |= RD_KAFKA_ERR_ACTION_PERMANENT;

done:
        if (err) {
                rd_assert(rk->rk_eos.txn_req_cnt > 0);
                rk->rk_eos.txn_req_cnt--;
        }

        /* Handle local request-level errors */
        switch (err) {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;

        case RD_KAFKA_RESP_ERR__DESTROY:
        case RD_KAFKA_RESP_ERR__OUTDATED:
                /* Terminating or outdated, ignore response */
                return;

        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        default:
                /* For these errors we can't be sure if the
                 * request was received by the broker or not,
                 * so increase the txn_req_cnt back up as if
                 * they were received so that and EndTxnRequest
                 * is sent on abort_transaction(). */
                rk->rk_eos.txn_req_cnt++;
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;
        }

        if (reset_coord_err) {
                rd_kafka_wrlock(rk);
                rd_kafka_txn_coord_set(rk, NULL,
                                       "AddPartitionsToTxn failed: %s",
                                       rd_kafka_err2str(reset_coord_err));
                rd_kafka_wrunlock(rk);
        }

        /* Partitions that failed will still be on the waitresp list
         * and are moved back to the pending list for the next scheduled
         * AddPartitionsToTxn request.
         * If this request was successful there will be no remaining partitions
         * on the waitresp list.
         */
        mtx_lock(&rk->rk_eos.txn_pending_lock);
        TAILQ_CONCAT_SORTED(&rk->rk_eos.txn_pending_rktps,
                            &rk->rk_eos.txn_waitresp_rktps, rd_kafka_toppar_t *,
                            rktp_txnlink, rd_kafka_toppar_topic_cmp);
        mtx_unlock(&rk->rk_eos.txn_pending_lock);

        err = rd_kafka_txn_normalize_err(err);

        if (actions & RD_KAFKA_ERR_ACTION_FATAL) {
                rd_kafka_txn_set_fatal_error(rk, RD_DO_LOCK, err,
                                             "Failed to add partitions to "
                                             "transaction: %s",
                                             rd_kafka_err2str(err));

        } else if (actions & RD_KAFKA_ERR_ACTION_PERMANENT) {
                /* Treat all other permanent errors as abortable errors.
                 * If an epoch bump is required let idempo sort it out. */
                if (require_bump)
                        rd_kafka_idemp_drain_epoch_bump(
                            rk, err,
                            "Failed to add partition(s) to transaction "
                            "on broker %s: %s (after %d ms)",
                            rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                            (int)(request->rkbuf_ts_sent / 1000));
                else
                        rd_kafka_txn_set_abortable_error(
                            rk, err,
                            "Failed to add partition(s) to transaction "
                            "on broker %s: %s (after %d ms)",
                            rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                            (int)(request->rkbuf_ts_sent / 1000));

        } else {
                /* Schedule registration of any new or remaining partitions */
                rd_kafka_txn_schedule_register_partitions(
                    rk, (actions & RD_KAFKA_ERR_ACTION_RETRY)
                            ? retry_backoff_ms
                            : 1 /*immediate*/);
        }
}


/**
 * @brief Send AddPartitionsToTxnRequest to the transaction coordinator.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_register_partitions(rd_kafka_t *rk) {
        char errstr[512];
        rd_kafka_resp_err_t err;
        rd_kafka_error_t *error;
        rd_kafka_pid_t pid;

        /* Require operational state */
        rd_kafka_rdlock(rk);
        error =
            rd_kafka_txn_require_state(rk, RD_KAFKA_TXN_STATE_IN_TRANSACTION,
                                       RD_KAFKA_TXN_STATE_BEGIN_COMMIT);

        if (unlikely(error != NULL)) {
                rd_kafka_rdunlock(rk);
                rd_kafka_dbg(rk, EOS, "ADDPARTS",
                             "Not registering partitions: %s",
                             rd_kafka_error_string(error));
                rd_kafka_error_destroy(error);
                return;
        }

        /* Get pid, checked later */
        pid = rd_kafka_idemp_get_pid0(rk, RD_DONT_LOCK, rd_false);

        rd_kafka_rdunlock(rk);

        /* Transaction coordinator needs to be up */
        if (!rd_kafka_broker_is_up(rk->rk_eos.txn_coord)) {
                rd_kafka_dbg(rk, EOS, "ADDPARTS",
                             "Not registering partitions: "
                             "coordinator is not available");
                return;
        }

        mtx_lock(&rk->rk_eos.txn_pending_lock);
        if (TAILQ_EMPTY(&rk->rk_eos.txn_pending_rktps)) {
                /* No pending partitions to register */
                mtx_unlock(&rk->rk_eos.txn_pending_lock);
                return;
        }

        if (!TAILQ_EMPTY(&rk->rk_eos.txn_waitresp_rktps)) {
                /* Only allow one outstanding AddPartitionsToTxnRequest */
                mtx_unlock(&rk->rk_eos.txn_pending_lock);
                rd_kafka_dbg(rk, EOS, "ADDPARTS",
                             "Not registering partitions: waiting for "
                             "previous AddPartitionsToTxn request to complete");
                return;
        }

        /* Require valid pid */
        if (unlikely(!rd_kafka_pid_valid(pid))) {
                mtx_unlock(&rk->rk_eos.txn_pending_lock);
                rd_kafka_dbg(rk, EOS, "ADDPARTS",
                             "Not registering partitions: "
                             "No PID available (idempotence state %s)",
                             rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                rd_dassert(!*"BUG: No PID despite proper transaction state");
                return;
        }


        /* Send request to coordinator */
        err = rd_kafka_AddPartitionsToTxnRequest(
            rk->rk_eos.txn_coord, rk->rk_conf.eos.transactional_id, pid,
            &rk->rk_eos.txn_pending_rktps, errstr, sizeof(errstr),
            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
            rd_kafka_txn_handle_AddPartitionsToTxn, NULL);
        if (err) {
                mtx_unlock(&rk->rk_eos.txn_pending_lock);
                rd_kafka_dbg(rk, EOS, "ADDPARTS",
                             "Not registering partitions: %s", errstr);
                return;
        }

        /* Move all pending partitions to wait-response list.
         * No need to keep waitresp sorted. */
        TAILQ_CONCAT(&rk->rk_eos.txn_waitresp_rktps,
                     &rk->rk_eos.txn_pending_rktps, rktp_txnlink);

        mtx_unlock(&rk->rk_eos.txn_pending_lock);

        rk->rk_eos.txn_req_cnt++;

        rd_rkb_dbg(rk->rk_eos.txn_coord, EOS, "ADDPARTS",
                   "Registering partitions with transaction");
}


static void rd_kafka_txn_register_partitions_tmr_cb(rd_kafka_timers_t *rkts,
                                                    void *arg) {
        rd_kafka_t *rk = arg;
        rd_kafka_txn_register_partitions(rk);
}


/**
 * @brief Schedule register_partitions() as soon as possible.
 *
 * @locality any
 * @locks any
 */
void rd_kafka_txn_schedule_register_partitions(rd_kafka_t *rk, int backoff_ms) {
        rd_kafka_timer_start_oneshot(
            &rk->rk_timers, &rk->rk_eos.txn_register_parts_tmr,
            rd_false /*dont-restart*/,
            backoff_ms ? backoff_ms * 1000 : 1 /* immediate */,
            rd_kafka_txn_register_partitions_tmr_cb, rk);
}



/**
 * @brief Clears \p flag from all rktps and destroys them, emptying
 *        and reinitializing the \p tqh.
 */
static void rd_kafka_txn_clear_partitions_flag(rd_kafka_toppar_tqhead_t *tqh,
                                               int flag) {
        rd_kafka_toppar_t *rktp, *tmp;

        TAILQ_FOREACH_SAFE(rktp, tqh, rktp_txnlink, tmp) {
                rd_kafka_toppar_lock(rktp);
                rd_dassert(rktp->rktp_flags & flag);
                rktp->rktp_flags &= ~flag;
                rd_kafka_toppar_unlock(rktp);
                rd_kafka_toppar_destroy(rktp);
        }

        TAILQ_INIT(tqh);
}


/**
 * @brief Clear all pending partitions.
 *
 * @locks txn_pending_lock MUST be held
 */
static void rd_kafka_txn_clear_pending_partitions(rd_kafka_t *rk) {
        rd_kafka_txn_clear_partitions_flag(&rk->rk_eos.txn_pending_rktps,
                                           RD_KAFKA_TOPPAR_F_PEND_TXN);
        rd_kafka_txn_clear_partitions_flag(&rk->rk_eos.txn_waitresp_rktps,
                                           RD_KAFKA_TOPPAR_F_PEND_TXN);
}

/**
 * @brief Clear all added partitions.
 *
 * @locks rd_kafka_wrlock(rk) MUST be held
 */
static void rd_kafka_txn_clear_partitions(rd_kafka_t *rk) {
        rd_kafka_txn_clear_partitions_flag(&rk->rk_eos.txn_rktps,
                                           RD_KAFKA_TOPPAR_F_IN_TXN);
}



/**
 * @brief Async handler for init_transactions()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t rd_kafka_txn_op_init_transactions(rd_kafka_t *rk,
                                                           rd_kafka_q_t *rkq,
                                                           rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if ((error = rd_kafka_txn_require_state(
                 rk, RD_KAFKA_TXN_STATE_INIT, RD_KAFKA_TXN_STATE_WAIT_PID,
                 RD_KAFKA_TXN_STATE_READY_NOT_ACKED))) {
                rd_kafka_wrunlock(rk);
                rd_kafka_txn_curr_api_set_result(rk, 0, error);

        } else if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_READY_NOT_ACKED) {
                /* A previous init_transactions() called finished successfully
                 * after timeout, the application has called init_transactions()
                 * again, we do nothin here, ack_init_transactions() will
                 * transition the state from READY_NOT_ACKED to READY. */
                rd_kafka_wrunlock(rk);

        } else {

                /* Possibly a no-op if already in WAIT_PID state */
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_WAIT_PID);

                rk->rk_eos.txn_init_err = RD_KAFKA_RESP_ERR_NO_ERROR;

                rd_kafka_wrunlock(rk);

                /* Start idempotent producer to acquire PID */
                rd_kafka_idemp_start(rk, rd_true /*immediately*/);

                /* Do not call curr_api_set_result, it will be triggered from
                 * idemp_state_change() when the PID has been retrieved. */
        }

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Async handler for the application to acknowledge
 *        successful background completion of init_transactions().
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t
rd_kafka_txn_op_ack_init_transactions(rd_kafka_t *rk,
                                      rd_kafka_q_t *rkq,
                                      rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if (!(error = rd_kafka_txn_require_state(
                  rk, RD_KAFKA_TXN_STATE_READY_NOT_ACKED)))
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_READY);

        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}



rd_kafka_error_t *rd_kafka_init_transactions(rd_kafka_t *rk, int timeout_ms) {
        rd_kafka_error_t *error;
        rd_ts_t abs_timeout;

        /* Cap actual timeout to transaction.timeout.ms * 2 when an infinite
         * timeout is provided, this is to make sure the call doesn't block
         * indefinitely in case a coordinator is not available.
         * This is only needed for init_transactions() since there is no
         * coordinator to time us out yet. */
        if (timeout_ms == RD_POLL_INFINITE &&
            /* Avoid overflow */
            rk->rk_conf.eos.transaction_timeout_ms < INT_MAX / 2)
                timeout_ms = rk->rk_conf.eos.transaction_timeout_ms * 2;

        if ((error = rd_kafka_txn_curr_api_begin(rk, "init_transactions",
                                                 rd_false /* no cap */,
                                                 timeout_ms, &abs_timeout)))
                return error;

        /* init_transactions() will continue to operate in the background
         * if the timeout expires, and the application may call
         * init_transactions() again to resume the initialization
         * process.
         * For this reason we need two states:
         *  - TXN_STATE_READY_NOT_ACKED for when initialization is done
         *    but the API call timed out prior to success, meaning the
         *    application does not know initialization finished and
         *    is thus not allowed to call sub-sequent txn APIs, e.g. begin..()
         *  - TXN_STATE_READY for when initialization is done and this
         *    function has returned successfully to the application.
         *
         * And due to the two states we need two calls to the rdkafka main
         * thread (to keep txn_state synchronization in one place). */

        /* First call is to trigger initialization */
        if ((error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_init_transactions,
                                         abs_timeout))) {
                if (rd_kafka_error_code(error) ==
                    RD_KAFKA_RESP_ERR__TIMED_OUT) {
                        /* See if there's a more meaningful txn_init_err set
                         * by idempo that we can return. */
                        rd_kafka_resp_err_t err;
                        rd_kafka_rdlock(rk);
                        err =
                            rd_kafka_txn_normalize_err(rk->rk_eos.txn_init_err);
                        rd_kafka_rdunlock(rk);

                        if (err && err != RD_KAFKA_RESP_ERR__TIMED_OUT) {
                                rd_kafka_error_destroy(error);
                                error = rd_kafka_error_new_retriable(
                                    err, "Failed to initialize Producer ID: %s",
                                    rd_kafka_err2str(err));
                        }
                }

                return rd_kafka_txn_curr_api_return(rk, rd_true, error);
        }


        /* Second call is to transition from READY_NOT_ACKED -> READY,
         * if necessary. */
        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_ack_init_transactions,
                                    /* Timeout must be infinite since this is
                                     * a synchronization point.
                                     * The call is immediate though, so this
                                     * will not block. */
                                    RD_POLL_INFINITE);

        return rd_kafka_txn_curr_api_return(rk,
                                            /* not resumable at this point */
                                            rd_false, error);
}



/**
 * @brief Handler for begin_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t rd_kafka_txn_op_begin_transaction(rd_kafka_t *rk,
                                                           rd_kafka_q_t *rkq,
                                                           rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;
        rd_bool_t wakeup_brokers = rd_false;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);
        if (!(error =
                  rd_kafka_txn_require_state(rk, RD_KAFKA_TXN_STATE_READY))) {
                rd_assert(TAILQ_EMPTY(&rk->rk_eos.txn_rktps));

                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_IN_TRANSACTION);

                rd_assert(rk->rk_eos.txn_req_cnt == 0);
                rd_atomic64_set(&rk->rk_eos.txn_dr_fails, 0);
                rk->rk_eos.txn_err = RD_KAFKA_RESP_ERR_NO_ERROR;
                RD_IF_FREE(rk->rk_eos.txn_errstr, rd_free);
                rk->rk_eos.txn_errstr = NULL;

                /* Wake up all broker threads (that may have messages to send
                 * that were waiting for this transaction state.
                 * But needs to be done below with no lock held. */
                wakeup_brokers = rd_true;
        }
        rd_kafka_wrunlock(rk);

        if (wakeup_brokers)
                rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_INIT,
                                            "begin transaction");

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}


rd_kafka_error_t *rd_kafka_begin_transaction(rd_kafka_t *rk) {
        rd_kafka_error_t *error;

        if ((error = rd_kafka_txn_curr_api_begin(rk, "begin_transaction",
                                                 rd_false, 0, NULL)))
                return error;

        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_begin_transaction,
                                    RD_POLL_INFINITE);

        return rd_kafka_txn_curr_api_return(rk, rd_false /*not resumable*/,
                                            error);
}


static rd_kafka_resp_err_t
rd_kafka_txn_send_TxnOffsetCommitRequest(rd_kafka_broker_t *rkb,
                                         rd_kafka_op_t *rko,
                                         rd_kafka_replyq_t replyq,
                                         rd_kafka_resp_cb_t *resp_cb,
                                         void *reply_opaque);

/**
 * @brief Handle TxnOffsetCommitResponse
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_handle_TxnOffsetCommit(rd_kafka_t *rk,
                                                rd_kafka_broker_t *rkb,
                                                rd_kafka_resp_err_t err,
                                                rd_kafka_buf_t *rkbuf,
                                                rd_kafka_buf_t *request,
                                                void *opaque) {
        const int log_decode_errors                 = LOG_ERR;
        rd_kafka_op_t *rko                          = opaque;
        int actions                                 = 0;
        rd_kafka_topic_partition_list_t *partitions = NULL;
        char errstr[512];

        *errstr = '\0';

        if (err)
                goto done;

        rd_kafka_buf_read_throttle_time(rkbuf);

        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        partitions = rd_kafka_buf_read_topic_partitions(
            rkbuf, rd_false /*don't use topic_id*/, rd_true, 0, fields);
        if (!partitions)
                goto err_parse;

        err = rd_kafka_topic_partition_list_get_err(partitions);
        if (err) {
                char errparts[256];
                rd_kafka_topic_partition_list_str(partitions, errparts,
                                                  sizeof(errparts),
                                                  RD_KAFKA_FMT_F_ONLY_ERR);
                rd_snprintf(errstr, sizeof(errstr),
                            "Failed to commit offsets to transaction on "
                            "broker %s: %s "
                            "(after %dms)",
                            rd_kafka_broker_name(rkb), errparts,
                            (int)(request->rkbuf_ts_sent / 1000));
        }

        goto done;

err_parse:
        err = rkbuf->rkbuf_err;

done:
        if (err) {
                if (!*errstr) {
                        rd_snprintf(errstr, sizeof(errstr),
                                    "Failed to commit offsets to "
                                    "transaction on broker %s: %s "
                                    "(after %d ms)",
                                    rkb ? rd_kafka_broker_name(rkb) : "(none)",
                                    rd_kafka_err2str(err),
                                    (int)(request->rkbuf_ts_sent / 1000));
                }
        }


        if (partitions)
                rd_kafka_topic_partition_list_destroy(partitions);

        switch (err) {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;

        case RD_KAFKA_RESP_ERR__DESTROY:
                /* Producer is being terminated, ignore the response. */
        case RD_KAFKA_RESP_ERR__OUTDATED:
                /* Set a non-actionable actions flag so that
                 * curr_api_set_result() is called below, without
                 * other side-effects. */
                actions = RD_KAFKA_ERR_ACTION_SPECIAL;
                return;

        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
                /* Note: this is the group coordinator, not the
                 *       transaction coordinator. */
                rd_kafka_coord_cache_evict(&rk->rk_coord_cache, rkb);
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS:
        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
        case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_ID_MAPPING:
        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
        case RD_KAFKA_RESP_ERR_INVALID_TXN_STATE:
        case RD_KAFKA_RESP_ERR_UNSUPPORTED_FOR_MESSAGE_FORMAT:
                actions |= RD_KAFKA_ERR_ACTION_FATAL;
                break;

        case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;

        case RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION:
        case RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID:
        case RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;

        default:
                /* Unhandled error, fail transaction */
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;
        }

        err = rd_kafka_txn_normalize_err(err);

        if (actions & RD_KAFKA_ERR_ACTION_FATAL) {
                rd_kafka_txn_set_fatal_error(rk, RD_DO_LOCK, err, "%s", errstr);

        } else if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                int remains_ms = rd_timeout_remains(rko->rko_u.txn.abs_timeout);

                if (!rd_timeout_expired(remains_ms)) {
                        rd_kafka_coord_req(
                            rk, RD_KAFKA_COORD_GROUP,
                            rko->rko_u.txn.cgmetadata->group_id,
                            rd_kafka_txn_send_TxnOffsetCommitRequest, rko,
                            500 /* 500ms delay before retrying */,
                            rd_timeout_remains_limit0(
                                remains_ms, rk->rk_conf.socket_timeout_ms),
                            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                            rd_kafka_txn_handle_TxnOffsetCommit, rko);
                        return;
                } else if (!err)
                        err = RD_KAFKA_RESP_ERR__TIMED_OUT;
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
        }

        if (actions & RD_KAFKA_ERR_ACTION_PERMANENT)
                rd_kafka_txn_set_abortable_error(rk, err, "%s", errstr);

        if (err)
                rd_kafka_txn_curr_api_set_result(
                    rk, actions, rd_kafka_error_new(err, "%s", errstr));
        else
                rd_kafka_txn_curr_api_set_result(rk, 0, NULL);

        rd_kafka_op_destroy(rko);
}



/**
 * @brief Construct and send TxnOffsetCommitRequest.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static rd_kafka_resp_err_t
rd_kafka_txn_send_TxnOffsetCommitRequest(rd_kafka_broker_t *rkb,
                                         rd_kafka_op_t *rko,
                                         rd_kafka_replyq_t replyq,
                                         rd_kafka_resp_cb_t *resp_cb,
                                         void *reply_opaque) {
        rd_kafka_t *rk = rkb->rkb_rk;
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion;
        rd_kafka_pid_t pid;
        const rd_kafka_consumer_group_metadata_t *cgmetadata =
            rko->rko_u.txn.cgmetadata;
        int cnt;

        rd_kafka_rdlock(rk);
        if (rk->rk_eos.txn_state != RD_KAFKA_TXN_STATE_IN_TRANSACTION) {
                rd_kafka_rdunlock(rk);
                /* Do not free the rko, it is passed as the reply_opaque
                 * on the reply queue by coord_req_fsm() when we return
                 * an error here. */
                return RD_KAFKA_RESP_ERR__STATE;
        }

        pid = rd_kafka_idemp_get_pid0(rk, RD_DONT_LOCK, rd_false);
        rd_kafka_rdunlock(rk);
        if (!rd_kafka_pid_valid(pid)) {
                /* Do not free the rko, it is passed as the reply_opaque
                 * on the reply queue by coord_req_fsm() when we return
                 * an error here. */
                return RD_KAFKA_RESP_ERR__STATE;
        }

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_TxnOffsetCommit, 0, 3, NULL);
        if (ApiVersion == -1) {
                /* Do not free the rko, it is passed as the reply_opaque
                 * on the reply queue by coord_req_fsm() when we return
                 * an error here. */
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_TxnOffsetCommit, 1, rko->rko_u.txn.offsets->cnt * 50,
            ApiVersion >= 3);

        /* transactional_id */
        rd_kafka_buf_write_str(rkbuf, rk->rk_conf.eos.transactional_id, -1);

        /* group_id */
        rd_kafka_buf_write_str(rkbuf, rko->rko_u.txn.cgmetadata->group_id, -1);

        /* PID */
        rd_kafka_buf_write_i64(rkbuf, pid.id);
        rd_kafka_buf_write_i16(rkbuf, pid.epoch);

        if (ApiVersion >= 3) {
                /* GenerationId */
                rd_kafka_buf_write_i32(rkbuf, cgmetadata->generation_id);
                /* MemberId */
                rd_kafka_buf_write_str(rkbuf, cgmetadata->member_id, -1);
                /* GroupInstanceId */
                rd_kafka_buf_write_str(rkbuf, cgmetadata->group_instance_id,
                                       -1);
        }

        /* Write per-partition offsets list */
        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
            ApiVersion >= 2 ? RD_KAFKA_TOPIC_PARTITION_FIELD_EPOCH
                            : RD_KAFKA_TOPIC_PARTITION_FIELD_NOOP,
            RD_KAFKA_TOPIC_PARTITION_FIELD_METADATA,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        cnt = rd_kafka_buf_write_topic_partitions(
            rkbuf, rko->rko_u.txn.offsets, rd_true /*skip invalid offsets*/,
            rd_false /*any offset*/, rd_false /*don't use topic id*/,
            rd_true /*use topic name*/, fields);
        if (!cnt) {
                /* No valid partition offsets, don't commit. */
                rd_kafka_buf_destroy(rkbuf);
                /* Do not free the rko, it is passed as the reply_opaque
                 * on the reply queue by coord_req_fsm() when we return
                 * an error here. */
                return RD_KAFKA_RESP_ERR__NO_OFFSET;
        }

        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);

        rkbuf->rkbuf_max_retries = RD_KAFKA_REQUEST_MAX_RETRIES;

        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb,
                                       reply_opaque);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Handle AddOffsetsToTxnResponse
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_handle_AddOffsetsToTxn(rd_kafka_t *rk,
                                                rd_kafka_broker_t *rkb,
                                                rd_kafka_resp_err_t err,
                                                rd_kafka_buf_t *rkbuf,
                                                rd_kafka_buf_t *request,
                                                void *opaque) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko          = opaque;
        int16_t ErrorCode;
        int actions = 0;
        int remains_ms;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                rd_kafka_op_destroy(rko);
                return;
        }

        if (err)
                goto done;

        rd_kafka_buf_read_throttle_time(rkbuf);
        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

        err = ErrorCode;
        goto done;

err_parse:
        err = rkbuf->rkbuf_err;

done:
        if (err) {
                rd_assert(rk->rk_eos.txn_req_cnt > 0);
                rk->rk_eos.txn_req_cnt--;
        }

        remains_ms = rd_timeout_remains(rko->rko_u.txn.abs_timeout);
        if (rd_timeout_expired(remains_ms) && !err)
                err = RD_KAFKA_RESP_ERR__TIMED_OUT;

        switch (err) {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;

        case RD_KAFKA_RESP_ERR__DESTROY:
                /* Producer is being terminated, ignore the response. */
        case RD_KAFKA_RESP_ERR__OUTDATED:
                /* Set a non-actionable actions flag so that
                 * curr_api_set_result() is called below, without
                 * other side-effects. */
                actions = RD_KAFKA_ERR_ACTION_SPECIAL;
                break;

        case RD_KAFKA_RESP_ERR__TRANSPORT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
                /* For these errors we can't be sure if the
                 * request was received by the broker or not,
                 * so increase the txn_req_cnt back up as if
                 * they were received so that and EndTxnRequest
                 * is sent on abort_transaction(). */
                rk->rk_eos.txn_req_cnt++;
                /* FALLTHRU */
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
        case RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT:
                actions |=
                    RD_KAFKA_ERR_ACTION_RETRY | RD_KAFKA_ERR_ACTION_REFRESH;
                break;

        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
        case RD_KAFKA_RESP_ERR_INVALID_TXN_STATE:
        case RD_KAFKA_RESP_ERR_UNSUPPORTED_FOR_MESSAGE_FORMAT:
                actions |= RD_KAFKA_ERR_ACTION_FATAL;
                break;

        case RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;

        case RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART:
        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
        case RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS:
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        default:
                /* All unhandled errors are permanent */
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                break;
        }

        err = rd_kafka_txn_normalize_err(err);

        rd_kafka_dbg(rk, EOS, "ADDOFFSETS",
                     "AddOffsetsToTxn response from %s: %s (%s)",
                     rkb ? rd_kafka_broker_name(rkb) : "(none)",
                     rd_kafka_err2name(err), rd_kafka_actions2str(actions));

        /* All unhandled errors are considered permanent */
        if (err && !actions)
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;

        if (actions & RD_KAFKA_ERR_ACTION_FATAL) {
                rd_kafka_txn_set_fatal_error(rk, RD_DO_LOCK, err,
                                             "Failed to add offsets to "
                                             "transaction: %s",
                                             rd_kafka_err2str(err));
        } else {
                if (actions & RD_KAFKA_ERR_ACTION_REFRESH)
                        rd_kafka_txn_coord_timer_start(rk, 50);

                if (actions & RD_KAFKA_ERR_ACTION_RETRY) {
                        rd_rkb_dbg(
                            rkb, EOS, "ADDOFFSETS",
                            "Failed to add offsets to transaction on "
                            "broker %s: %s (after %dms, %dms remains): "
                            "error is retriable",
                            rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                            (int)(request->rkbuf_ts_sent / 1000), remains_ms);

                        if (!rd_timeout_expired(remains_ms) &&
                            rd_kafka_buf_retry(rk->rk_eos.txn_coord, request)) {
                                rk->rk_eos.txn_req_cnt++;
                                return;
                        }

                        /* Propagate as retriable error through
                         * api_reply() below */
                }
        }

        if (err)
                rd_rkb_log(rkb, LOG_ERR, "ADDOFFSETS",
                           "Failed to add offsets to transaction on broker %s: "
                           "%s",
                           rkb ? rd_kafka_broker_name(rkb) : "(none)",
                           rd_kafka_err2str(err));

        if (actions & RD_KAFKA_ERR_ACTION_PERMANENT)
                rd_kafka_txn_set_abortable_error(
                    rk, err,
                    "Failed to add offsets to "
                    "transaction on broker %s: "
                    "%s (after %dms)",
                    rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                    (int)(request->rkbuf_ts_sent / 1000));

        if (!err) {
                /* Step 2: Commit offsets to transaction on the
                 * group coordinator. */

                rd_kafka_coord_req(
                    rk, RD_KAFKA_COORD_GROUP,
                    rko->rko_u.txn.cgmetadata->group_id,
                    rd_kafka_txn_send_TxnOffsetCommitRequest, rko,
                    0 /* no delay */,
                    rd_timeout_remains_limit0(remains_ms,
                                              rk->rk_conf.socket_timeout_ms),
                    RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                    rd_kafka_txn_handle_TxnOffsetCommit, rko);

        } else {

                rd_kafka_txn_curr_api_set_result(
                    rk, actions,
                    rd_kafka_error_new(
                        err,
                        "Failed to add offsets to transaction on "
                        "broker %s: %s (after %dms)",
                        rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                        (int)(request->rkbuf_ts_sent / 1000)));

                rd_kafka_op_destroy(rko);
        }
}


/**
 * @brief Async handler for send_offsets_to_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t
rd_kafka_txn_op_send_offsets_to_transaction(rd_kafka_t *rk,
                                            rd_kafka_q_t *rkq,
                                            rd_kafka_op_t *rko) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        char errstr[512];
        rd_kafka_error_t *error;
        rd_kafka_pid_t pid;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        *errstr = '\0';

        rd_kafka_wrlock(rk);

        if ((error = rd_kafka_txn_require_state(
                 rk, RD_KAFKA_TXN_STATE_IN_TRANSACTION))) {
                rd_kafka_wrunlock(rk);
                goto err;
        }

        rd_kafka_wrunlock(rk);

        pid = rd_kafka_idemp_get_pid0(rk, RD_DONT_LOCK, rd_false);
        if (!rd_kafka_pid_valid(pid)) {
                rd_dassert(!*"BUG: No PID despite proper transaction state");
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__STATE,
                    "No PID available (idempotence state %s)",
                    rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                goto err;
        }

        /* This is a multi-stage operation, consisting of:
         *  1) send AddOffsetsToTxnRequest to transaction coordinator.
         *  2) send TxnOffsetCommitRequest to group coordinator. */

        err = rd_kafka_AddOffsetsToTxnRequest(
            rk->rk_eos.txn_coord, rk->rk_conf.eos.transactional_id, pid,
            rko->rko_u.txn.cgmetadata->group_id, errstr, sizeof(errstr),
            RD_KAFKA_REPLYQ(rk->rk_ops, 0), rd_kafka_txn_handle_AddOffsetsToTxn,
            rko);

        if (err) {
                error = rd_kafka_error_new_retriable(err, "%s", errstr);
                goto err;
        }

        rk->rk_eos.txn_req_cnt++;

        return RD_KAFKA_OP_RES_KEEP; /* the rko is passed to AddOffsetsToTxn */

err:
        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}

/**
 * error returns:
 *   ERR__TRANSPORT - retryable
 */
rd_kafka_error_t *rd_kafka_send_offsets_to_transaction(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *offsets,
    const rd_kafka_consumer_group_metadata_t *cgmetadata,
    int timeout_ms) {
        rd_kafka_error_t *error;
        rd_kafka_op_t *rko;
        rd_kafka_topic_partition_list_t *valid_offsets;
        rd_ts_t abs_timeout;

        if (!cgmetadata || !offsets)
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "cgmetadata and offsets are required parameters");

        if ((error = rd_kafka_txn_curr_api_begin(
                 rk, "send_offsets_to_transaction",
                 /* Cap timeout to txn timeout */
                 rd_true, timeout_ms, &abs_timeout)))
                return error;


        valid_offsets = rd_kafka_topic_partition_list_match(
            offsets, rd_kafka_topic_partition_match_valid_offset, NULL);

        if (valid_offsets->cnt == 0) {
                /* No valid offsets, e.g., nothing was consumed,
                 * this is not an error, do nothing. */
                rd_kafka_topic_partition_list_destroy(valid_offsets);
                return rd_kafka_txn_curr_api_return(rk, rd_false, NULL);
        }

        rd_kafka_topic_partition_list_sort_by_topic(valid_offsets);

        rko                    = rd_kafka_op_new_cb(rk, RD_KAFKA_OP_TXN,
                                 rd_kafka_txn_op_send_offsets_to_transaction);
        rko->rko_u.txn.offsets = valid_offsets;
        rko->rko_u.txn.cgmetadata =
            rd_kafka_consumer_group_metadata_dup(cgmetadata);
        rko->rko_u.txn.abs_timeout = abs_timeout;

        /* Timeout is enforced by op_send_offsets_to_transaction() */
        error = rd_kafka_txn_op_req1(rk, rko, RD_POLL_INFINITE);

        return rd_kafka_txn_curr_api_return(rk, rd_false, error);
}



/**
 * @brief Successfully complete the transaction.
 *
 * Current state must be either COMMIT_NOT_ACKED or ABORT_NOT_ACKED.
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock(rk) MUST be held
 */
static void rd_kafka_txn_complete(rd_kafka_t *rk, rd_bool_t is_commit) {
        rd_kafka_dbg(rk, EOS, "TXNCOMPLETE", "Transaction successfully %s",
                     is_commit ? "committed" : "aborted");

        /* Clear all transaction partition state */
        rd_kafka_txn_clear_pending_partitions(rk);
        rd_kafka_txn_clear_partitions(rk);

        rk->rk_eos.txn_requires_epoch_bump = rd_false;
        rk->rk_eos.txn_req_cnt             = 0;

        rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_READY);
}


/**
 * @brief EndTxn (commit or abort of transaction on the coordinator) is done,
 *        or was skipped.
 *        Continue with next steps (if any) before completing the local
 *        transaction state.
 *
 * @locality rdkafka main thread
 * @locks_acquired rd_kafka_wrlock(rk), rk->rk_eos.txn_curr_api.lock
 */
static void rd_kafka_txn_endtxn_complete(rd_kafka_t *rk) {
        rd_bool_t is_commit;

        mtx_lock(&rk->rk_eos.txn_curr_api.lock);
        is_commit = !strcmp(rk->rk_eos.txn_curr_api.name, "commit_transaction");
        mtx_unlock(&rk->rk_eos.txn_curr_api.lock);

        rd_kafka_wrlock(rk);

        /* If an epoch bump is required, let idempo handle it.
         * When the bump is finished we'll be notified through
         * idemp_state_change() and we can complete the local transaction state
         * and set the final API call result.
         * If the bumping fails a fatal error will be raised. */
        if (rk->rk_eos.txn_requires_epoch_bump) {
                rd_kafka_resp_err_t bump_err = rk->rk_eos.txn_err;
                rd_dassert(!is_commit);

                rd_kafka_wrunlock(rk);

                /* After the epoch bump is done we'll be transitioned
                 * to the next state. */
                rd_kafka_idemp_drain_epoch_bump0(
                    rk, rd_false /* don't allow txn abort */, bump_err,
                    "Transaction aborted: %s", rd_kafka_err2str(bump_err));
                return;
        }

        if (is_commit)
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED);
        else
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED);

        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, NULL);
}


/**
 * @brief Handle EndTxnResponse (commit or abort)
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_handle_EndTxn(rd_kafka_t *rk,
                                       rd_kafka_broker_t *rkb,
                                       rd_kafka_resp_err_t err,
                                       rd_kafka_buf_t *rkbuf,
                                       rd_kafka_buf_t *request,
                                       void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode;
        int actions = 0;
        rd_bool_t is_commit, may_retry = rd_false, require_bump = rd_false;

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                return;

        is_commit = request->rkbuf_u.EndTxn.commit;

        if (err)
                goto err;

        rd_kafka_buf_read_throttle_time(rkbuf);
        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);
        err = ErrorCode;
        goto err;

err_parse:
        err = rkbuf->rkbuf_err;
        /* FALLTHRU */

err:
        rd_kafka_wrlock(rk);

        if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION) {
                may_retry = rd_true;

        } else if (rk->rk_eos.txn_state ==
                   RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION) {
                may_retry = rd_true;

        } else if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_ABORTABLE_ERROR) {
                /* Transaction has failed locally, typically due to timeout.
                 * Get the transaction error and return that instead of
                 * this error.
                 * This is a tricky state since the transaction will have
                 * failed locally but the EndTxn(commit) may have succeeded. */


                if (err) {
                        rd_kafka_txn_curr_api_set_result(
                            rk, RD_KAFKA_ERR_ACTION_PERMANENT,
                            rd_kafka_error_new(
                                rk->rk_eos.txn_err,
                                "EndTxn failed with %s but transaction "
                                "had already failed due to: %s",
                                rd_kafka_err2name(err), rk->rk_eos.txn_errstr));
                } else {
                        /* If the transaction has failed locally but
                         * this EndTxn commit succeeded we'll raise
                         * a fatal error. */
                        if (is_commit)
                                rd_kafka_txn_curr_api_set_result(
                                    rk, RD_KAFKA_ERR_ACTION_FATAL,
                                    rd_kafka_error_new(
                                        rk->rk_eos.txn_err,
                                        "Transaction commit succeeded on the "
                                        "broker but the transaction "
                                        "had already failed locally due to: %s",
                                        rk->rk_eos.txn_errstr));

                        else
                                rd_kafka_txn_curr_api_set_result(
                                    rk, RD_KAFKA_ERR_ACTION_PERMANENT,
                                    rd_kafka_error_new(
                                        rk->rk_eos.txn_err,
                                        "Transaction abort succeeded on the "
                                        "broker but the transaction"
                                        "had already failed locally due to: %s",
                                        rk->rk_eos.txn_errstr));
                }

                rd_kafka_wrunlock(rk);


                return;

        } else if (!err) {
                /* Request is outdated */
                err = RD_KAFKA_RESP_ERR__OUTDATED;
        }


        rd_kafka_dbg(rk, EOS, "ENDTXN",
                     "EndTxn returned %s in state %s (may_retry=%s)",
                     rd_kafka_err2name(err),
                     rd_kafka_txn_state2str(rk->rk_eos.txn_state),
                     RD_STR_ToF(may_retry));

        rd_kafka_wrunlock(rk);

        switch (err) {
        case RD_KAFKA_RESP_ERR_NO_ERROR:
                break;

        case RD_KAFKA_RESP_ERR__DESTROY:
                /* Producer is being terminated, ignore the response. */
        case RD_KAFKA_RESP_ERR__OUTDATED:
                /* Transactional state no longer relevant for this
                 * outdated response. */
                break;
        case RD_KAFKA_RESP_ERR__TIMED_OUT:
        case RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE:
                /* Request timeout */
                /* FALLTHRU */
        case RD_KAFKA_RESP_ERR__TRANSPORT:
                actions |=
                    RD_KAFKA_ERR_ACTION_RETRY | RD_KAFKA_ERR_ACTION_REFRESH;
                break;

        case RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE:
        case RD_KAFKA_RESP_ERR_NOT_COORDINATOR:
                rd_kafka_wrlock(rk);
                rd_kafka_txn_coord_set(rk, NULL, "EndTxn failed: %s",
                                       rd_kafka_err2str(err));
                rd_kafka_wrunlock(rk);
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS:
        case RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS:
                actions |= RD_KAFKA_ERR_ACTION_RETRY;
                break;

        case RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID:
        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_ID_MAPPING:
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
                require_bump = rd_true;
                break;

        case RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH:
        case RD_KAFKA_RESP_ERR_PRODUCER_FENCED:
        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_INVALID_TXN_STATE:
                actions |= RD_KAFKA_ERR_ACTION_FATAL;
                break;

        default:
                /* All unhandled errors are permanent */
                actions |= RD_KAFKA_ERR_ACTION_PERMANENT;
        }

        err = rd_kafka_txn_normalize_err(err);

        if (actions & RD_KAFKA_ERR_ACTION_FATAL) {
                rd_kafka_txn_set_fatal_error(rk, RD_DO_LOCK, err,
                                             "Failed to end transaction: %s",
                                             rd_kafka_err2str(err));
        } else {
                if (actions & RD_KAFKA_ERR_ACTION_REFRESH)
                        rd_kafka_txn_coord_timer_start(rk, 50);

                if (actions & RD_KAFKA_ERR_ACTION_PERMANENT) {
                        if (require_bump && !is_commit) {
                                /* Abort failed to due invalid PID, starting
                                 * with KIP-360 we can have idempo sort out
                                 * epoch bumping.
                                 * When the epoch has been bumped we'll detect
                                 * the idemp_state_change and complete the
                                 * current API call. */
                                rd_kafka_idemp_drain_epoch_bump0(
                                    rk,
                                    /* don't allow txn abort */
                                    rd_false, err, "EndTxn %s failed: %s",
                                    is_commit ? "commit" : "abort",
                                    rd_kafka_err2str(err));
                                return;
                        }

                        /* For aborts we need to revert the state back to
                         * BEGIN_ABORT so that the abort can be retried from
                         * the beginning in op_abort_transaction(). */
                        rd_kafka_wrlock(rk);
                        if (rk->rk_eos.txn_state ==
                            RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION)
                                rd_kafka_txn_set_state(
                                    rk, RD_KAFKA_TXN_STATE_BEGIN_ABORT);
                        rd_kafka_wrunlock(rk);

                        rd_kafka_txn_set_abortable_error0(
                            rk, err, require_bump,
                            "Failed to end transaction: "
                            "%s",
                            rd_kafka_err2str(err));

                } else if (may_retry && actions & RD_KAFKA_ERR_ACTION_RETRY &&
                           rd_kafka_buf_retry(rkb, request))
                        return;
        }

        if (err)
                rd_kafka_txn_curr_api_set_result(
                    rk, actions,
                    rd_kafka_error_new(err, "EndTxn %s failed: %s",
                                       is_commit ? "commit" : "abort",
                                       rd_kafka_err2str(err)));
        else
                rd_kafka_txn_endtxn_complete(rk);
}



/**
 * @brief Handler for commit_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t
rd_kafka_txn_op_commit_transaction(rd_kafka_t *rk,
                                   rd_kafka_q_t *rkq,
                                   rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_pid_t pid;
        int64_t dr_fails;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if ((error = rd_kafka_txn_require_state(
                 rk, RD_KAFKA_TXN_STATE_BEGIN_COMMIT,
                 RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION,
                 RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED)))
                goto done;

        if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED) {
                /* A previous call to commit_transaction() timed out but the
                 * commit completed since then, we still
                 * need to wait for the application to call commit_transaction()
                 * again to resume the call, and it just did. */
                goto done;
        } else if (rk->rk_eos.txn_state ==
                   RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION) {
                /* A previous call to commit_transaction() timed out but the
                 * commit is still in progress, we still
                 * need to wait for the application to call commit_transaction()
                 * again to resume the call, and it just did. */
                rd_kafka_wrunlock(rk);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        /* If any messages failed delivery the transaction must be aborted. */
        dr_fails = rd_atomic64_get(&rk->rk_eos.txn_dr_fails);
        if (unlikely(dr_fails > 0)) {
                error = rd_kafka_error_new_txn_requires_abort(
                    RD_KAFKA_RESP_ERR__INCONSISTENT,
                    "%" PRId64
                    " message(s) failed delivery "
                    "(see individual delivery reports)",
                    dr_fails);
                goto done;
        }

        if (!rk->rk_eos.txn_req_cnt) {
                /* If there were no messages produced, or no send_offsets,
                 * in this transaction, simply complete the transaction
                 * without sending anything to the transaction coordinator
                 * (since it will not have any txn state). */
                rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                             "No partitions registered: not sending EndTxn");
                rd_kafka_wrunlock(rk);
                rd_kafka_txn_endtxn_complete(rk);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        pid = rd_kafka_idemp_get_pid0(rk, RD_DONT_LOCK, rd_false);
        if (!rd_kafka_pid_valid(pid)) {
                rd_dassert(!*"BUG: No PID despite proper transaction state");
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__STATE,
                    "No PID available (idempotence state %s)",
                    rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                goto done;
        }

        err = rd_kafka_EndTxnRequest(
            rk->rk_eos.txn_coord, rk->rk_conf.eos.transactional_id, pid,
            rd_true /* commit */, errstr, sizeof(errstr),
            RD_KAFKA_REPLYQ(rk->rk_ops, 0), rd_kafka_txn_handle_EndTxn, NULL);
        if (err) {
                error = rd_kafka_error_new_retriable(err, "%s", errstr);
                goto done;
        }

        rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION);

        rd_kafka_wrunlock(rk);

        return RD_KAFKA_OP_RES_HANDLED;

done:
        rd_kafka_wrunlock(rk);

        /* If the returned error is an abortable error
         * also set the current transaction state accordingly. */
        if (rd_kafka_error_txn_requires_abort(error))
                rd_kafka_txn_set_abortable_error(rk, rd_kafka_error_code(error),
                                                 "%s",
                                                 rd_kafka_error_string(error));

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Handler for commit_transaction()'s first phase: begin commit
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t rd_kafka_txn_op_begin_commit(rd_kafka_t *rk,
                                                      rd_kafka_q_t *rkq,
                                                      rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;


        rd_kafka_wrlock(rk);

        error = rd_kafka_txn_require_state(
            rk, RD_KAFKA_TXN_STATE_IN_TRANSACTION,
            RD_KAFKA_TXN_STATE_BEGIN_COMMIT,
            RD_KAFKA_TXN_STATE_COMMITTING_TRANSACTION,
            RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED);

        if (!error &&
            rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_IN_TRANSACTION) {
                /* Transition to BEGIN_COMMIT state if no error and commit not
                 * already started. */
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_BEGIN_COMMIT);
        }

        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Handler for last ack of commit_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t
rd_kafka_txn_op_commit_transaction_ack(rd_kafka_t *rk,
                                       rd_kafka_q_t *rkq,
                                       rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if (!(error = rd_kafka_txn_require_state(
                  rk, RD_KAFKA_TXN_STATE_COMMIT_NOT_ACKED))) {
                rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                             "Committed transaction now acked by application");
                rd_kafka_txn_complete(rk, rd_true /*is commit*/);
        }

        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}



rd_kafka_error_t *rd_kafka_commit_transaction(rd_kafka_t *rk, int timeout_ms) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;
        rd_ts_t abs_timeout;

        /* The commit is in three phases:
         *   - begin commit: wait for outstanding messages to be produced,
         *                   disallow new messages from being produced
         *                   by application.
         *   - commit: commit transaction.
         *   - commit not acked: commit done, but waiting for application
         *                       to acknowledge by completing this API call.
         */

        if ((error = rd_kafka_txn_curr_api_begin(rk, "commit_transaction",
                                                 rd_false /* no cap */,
                                                 timeout_ms, &abs_timeout)))
                return error;

        /* Begin commit */
        if ((error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_begin_commit,
                                         abs_timeout)))
                return rd_kafka_txn_curr_api_return(rk,
                                                    /* not resumable yet */
                                                    rd_false, error);

        rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                     "Flushing %d outstanding message(s) prior to commit",
                     rd_kafka_outq_len(rk));

        /* Wait for queued messages to be delivered, limited by
         * the remaining transaction lifetime. */
        if ((err = rd_kafka_flush(rk, rd_timeout_remains(abs_timeout)))) {
                rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                             "Flush failed (with %d messages remaining): %s",
                             rd_kafka_outq_len(rk), rd_kafka_err2str(err));

                if (err == RD_KAFKA_RESP_ERR__TIMED_OUT)
                        error = rd_kafka_error_new_retriable(
                            err,
                            "Failed to flush all outstanding messages "
                            "within the API timeout: "
                            "%d message(s) remaining%s",
                            rd_kafka_outq_len(rk),
                            /* In case event queue delivery reports
                             * are enabled and there is no dr callback
                             * we instruct the developer to poll
                             * the event queue separately, since we
                             * can't do it for them. */
                            ((rk->rk_conf.enabled_events & RD_KAFKA_EVENT_DR) &&
                             !rk->rk_conf.dr_msg_cb && !rk->rk_conf.dr_cb)
                                ? ": the event queue must be polled "
                                  "for delivery report events in a separate "
                                  "thread or prior to calling commit"
                                : "");
                else
                        error = rd_kafka_error_new_retriable(
                            err, "Failed to flush outstanding messages: %s",
                            rd_kafka_err2str(err));

                /* The commit operation is in progress in the background
                 * and the application will need to call this API again
                 * to resume. */
                return rd_kafka_txn_curr_api_return(rk, rd_true, error);
        }

        rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                     "Transaction commit message flush complete");

        /* Commit transaction */
        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_commit_transaction,
                                    abs_timeout);
        if (error)
                return rd_kafka_txn_curr_api_return(rk, rd_true, error);

        /* Last call is to transition from COMMIT_NOT_ACKED to READY */
        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_commit_transaction_ack,
                                    /* Timeout must be infinite since this is
                                     * a synchronization point.
                                     * The call is immediate though, so this
                                     * will not block. */
                                    RD_POLL_INFINITE);

        return rd_kafka_txn_curr_api_return(rk,
                                            /* not resumable at this point */
                                            rd_false, error);
}



/**
 * @brief Handler for abort_transaction()'s first phase: begin abort
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t rd_kafka_txn_op_begin_abort(rd_kafka_t *rk,
                                                     rd_kafka_q_t *rkq,
                                                     rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;
        rd_bool_t clear_pending = rd_false;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        error =
            rd_kafka_txn_require_state(rk, RD_KAFKA_TXN_STATE_IN_TRANSACTION,
                                       RD_KAFKA_TXN_STATE_BEGIN_ABORT,
                                       RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION,
                                       RD_KAFKA_TXN_STATE_ABORTABLE_ERROR,
                                       RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED);

        if (!error &&
            (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_IN_TRANSACTION ||
             rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_ABORTABLE_ERROR)) {
                /* Transition to ABORTING_TRANSACTION state if no error and
                 * abort not already started. */
                rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_BEGIN_ABORT);
                clear_pending = rd_true;
        }

        rd_kafka_wrunlock(rk);

        if (clear_pending) {
                mtx_lock(&rk->rk_eos.txn_pending_lock);
                rd_kafka_txn_clear_pending_partitions(rk);
                mtx_unlock(&rk->rk_eos.txn_pending_lock);
        }

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Handler for abort_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t rd_kafka_txn_op_abort_transaction(rd_kafka_t *rk,
                                                           rd_kafka_q_t *rkq,
                                                           rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_pid_t pid;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if ((error = rd_kafka_txn_require_state(
                 rk, RD_KAFKA_TXN_STATE_BEGIN_ABORT,
                 RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION,
                 RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED)))
                goto done;

        if (rk->rk_eos.txn_state == RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED) {
                /* A previous call to abort_transaction() timed out but
                 * the aborting completed since then, we still need to wait
                 * for the application to call abort_transaction() again
                 * to synchronize state, and it just did. */
                goto done;
        } else if (rk->rk_eos.txn_state ==
                   RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION) {
                /* A previous call to abort_transaction() timed out but
                 * the abort is still in progress, we still need to wait
                 * for the application to call abort_transaction() again
                 * to synchronize state, and it just did. */
                rd_kafka_wrunlock(rk);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        if (!rk->rk_eos.txn_req_cnt) {
                rd_kafka_dbg(rk, EOS, "TXNABORT",
                             "No partitions registered: not sending EndTxn");
                rd_kafka_wrunlock(rk);
                rd_kafka_txn_endtxn_complete(rk);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        /* If the underlying idempotent producer's state indicates it
         * is re-acquiring its PID we need to wait for that to finish
         * before allowing a new begin_transaction(), and since that is
         * not a blocking call we need to perform that wait in this
         * state instead.
         * To recover we need to request an epoch bump from the
         * transaction coordinator. This is handled automatically
         * by the idempotent producer, so we just need to wait for
         * the new pid to be assigned.
         */
        if (rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_ASSIGNED &&
            rk->rk_eos.idemp_state != RD_KAFKA_IDEMP_STATE_WAIT_TXN_ABORT) {
                rd_kafka_dbg(rk, EOS, "TXNABORT",
                             "Waiting for transaction coordinator "
                             "PID bump to complete before aborting "
                             "transaction (idempotent producer state %s)",
                             rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));

                rd_kafka_wrunlock(rk);

                return RD_KAFKA_OP_RES_HANDLED;
        }

        pid = rd_kafka_idemp_get_pid0(rk, RD_DONT_LOCK, rd_true);
        if (!rd_kafka_pid_valid(pid)) {
                rd_dassert(!*"BUG: No PID despite proper transaction state");
                error = rd_kafka_error_new_retriable(
                    RD_KAFKA_RESP_ERR__STATE,
                    "No PID available (idempotence state %s)",
                    rd_kafka_idemp_state2str(rk->rk_eos.idemp_state));
                goto done;
        }

        err = rd_kafka_EndTxnRequest(
            rk->rk_eos.txn_coord, rk->rk_conf.eos.transactional_id, pid,
            rd_false /* abort */, errstr, sizeof(errstr),
            RD_KAFKA_REPLYQ(rk->rk_ops, 0), rd_kafka_txn_handle_EndTxn, NULL);
        if (err) {
                error = rd_kafka_error_new_retriable(err, "%s", errstr);
                goto done;
        }

        rd_kafka_txn_set_state(rk, RD_KAFKA_TXN_STATE_ABORTING_TRANSACTION);

        rd_kafka_wrunlock(rk);

        return RD_KAFKA_OP_RES_HANDLED;

done:
        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Handler for last ack of abort_transaction()
 *
 * @locks none
 * @locality rdkafka main thread
 */
static rd_kafka_op_res_t
rd_kafka_txn_op_abort_transaction_ack(rd_kafka_t *rk,
                                      rd_kafka_q_t *rkq,
                                      rd_kafka_op_t *rko) {
        rd_kafka_error_t *error;

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED;

        rd_kafka_wrlock(rk);

        if (!(error = rd_kafka_txn_require_state(
                  rk, RD_KAFKA_TXN_STATE_ABORT_NOT_ACKED))) {
                rd_kafka_dbg(rk, EOS, "TXNABORT",
                             "Aborted transaction now acked by application");
                rd_kafka_txn_complete(rk, rd_false /*is abort*/);
        }

        rd_kafka_wrunlock(rk);

        rd_kafka_txn_curr_api_set_result(rk, 0, error);

        return RD_KAFKA_OP_RES_HANDLED;
}



rd_kafka_error_t *rd_kafka_abort_transaction(rd_kafka_t *rk, int timeout_ms) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;
        rd_ts_t abs_timeout;

        if ((error = rd_kafka_txn_curr_api_begin(rk, "abort_transaction",
                                                 rd_false /* no cap */,
                                                 timeout_ms, &abs_timeout)))
                return error;

        /* The abort is multi-phase:
         * - set state to BEGIN_ABORT
         * - flush() outstanding messages
         * - send EndTxn
         */

        /* Begin abort */
        if ((error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_begin_abort,
                                         abs_timeout)))
                return rd_kafka_txn_curr_api_return(rk,
                                                    /* not resumable yet */
                                                    rd_false, error);

        rd_kafka_dbg(rk, EOS, "TXNABORT",
                     "Purging and flushing %d outstanding message(s) prior "
                     "to abort",
                     rd_kafka_outq_len(rk));

        /* Purge all queued messages.
         * Will need to wait for messages in-flight since purging these
         * messages may lead to gaps in the idempotent producer sequences. */
        err = rd_kafka_purge(rk, RD_KAFKA_PURGE_F_QUEUE |
                                     RD_KAFKA_PURGE_F_ABORT_TXN);

        /* Serve delivery reports for the purged messages. */
        if ((err = rd_kafka_flush(rk, rd_timeout_remains(abs_timeout)))) {
                /* FIXME: Not sure these errors matter that much */
                if (err == RD_KAFKA_RESP_ERR__TIMED_OUT)
                        error = rd_kafka_error_new_retriable(
                            err,
                            "Failed to flush all outstanding messages "
                            "within the API timeout: "
                            "%d message(s) remaining%s",
                            rd_kafka_outq_len(rk),
                            (rk->rk_conf.enabled_events & RD_KAFKA_EVENT_DR)
                                ? ": the event queue must be polled "
                                  "for delivery report events in a separate "
                                  "thread or prior to calling abort"
                                : "");

                else
                        error = rd_kafka_error_new_retriable(
                            err, "Failed to flush outstanding messages: %s",
                            rd_kafka_err2str(err));

                /* The abort operation is in progress in the background
                 * and the application will need to call this API again
                 * to resume. */
                return rd_kafka_txn_curr_api_return(rk, rd_true, error);
        }

        rd_kafka_dbg(rk, EOS, "TXNCOMMIT",
                     "Transaction abort message purge and flush complete");

        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_abort_transaction,
                                    abs_timeout);
        if (error)
                return rd_kafka_txn_curr_api_return(rk, rd_true, error);

        /* Last call is to transition from ABORT_NOT_ACKED to READY. */
        error = rd_kafka_txn_op_req(rk, rd_kafka_txn_op_abort_transaction_ack,
                                    /* Timeout must be infinite since this is
                                     * a synchronization point.
                                     * The call is immediate though, so this
                                     * will not block. */
                                    RD_POLL_INFINITE);

        return rd_kafka_txn_curr_api_return(rk,
                                            /* not resumable at this point */
                                            rd_false, error);
}



/**
 * @brief Coordinator query timer
 *
 * @locality rdkafka main thread
 * @locks none
 */

static void rd_kafka_txn_coord_timer_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_wrlock(rk);
        rd_kafka_txn_coord_query(rk, "Coordinator query timer");
        rd_kafka_wrunlock(rk);
}

/**
 * @brief Start coord query timer if not already started.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_coord_timer_start(rd_kafka_t *rk, int timeout_ms) {
        rd_assert(rd_kafka_is_transactional(rk));
        rd_kafka_timer_start_oneshot(&rk->rk_timers, &rk->rk_eos.txn_coord_tmr,
                                     /* don't restart if already started */
                                     rd_false, 1000 * timeout_ms,
                                     rd_kafka_txn_coord_timer_cb, rk);
}


/**
 * @brief Parses and handles a FindCoordinator response.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_txn_handle_FindCoordinator(rd_kafka_t *rk,
                                                rd_kafka_broker_t *rkb,
                                                rd_kafka_resp_err_t err,
                                                rd_kafka_buf_t *rkbuf,
                                                rd_kafka_buf_t *request,
                                                void *opaque) {
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode;
        rd_kafkap_str_t Host;
        int32_t NodeId, Port;
        char errstr[512];

        *errstr = '\0';

        rk->rk_eos.txn_wait_coord = rd_false;

        if (err)
                goto err;

        if (request->rkbuf_reqhdr.ApiVersion >= 1)
                rd_kafka_buf_read_throttle_time(rkbuf);

        rd_kafka_buf_read_i16(rkbuf, &ErrorCode);

        if (request->rkbuf_reqhdr.ApiVersion >= 1) {
                rd_kafkap_str_t ErrorMsg;
                rd_kafka_buf_read_str(rkbuf, &ErrorMsg);
                if (ErrorCode)
                        rd_snprintf(errstr, sizeof(errstr), "%.*s",
                                    RD_KAFKAP_STR_PR(&ErrorMsg));
        }

        if ((err = ErrorCode))
                goto err;

        rd_kafka_buf_read_i32(rkbuf, &NodeId);
        rd_kafka_buf_read_str(rkbuf, &Host);
        rd_kafka_buf_read_i32(rkbuf, &Port);

        rd_rkb_dbg(rkb, EOS, "TXNCOORD",
                   "FindCoordinator response: "
                   "Transaction coordinator is broker %" PRId32 " (%.*s:%d)",
                   NodeId, RD_KAFKAP_STR_PR(&Host), (int)Port);

        rd_kafka_rdlock(rk);
        if (NodeId == -1)
                err = RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE;
        else if (!(rkb = rd_kafka_broker_find_by_nodeid(rk, NodeId))) {
                rd_snprintf(errstr, sizeof(errstr),
                            "Transaction coordinator %" PRId32 " is unknown",
                            NodeId);
                err = RD_KAFKA_RESP_ERR__UNKNOWN_BROKER;
        }
        rd_kafka_rdunlock(rk);

        if (err)
                goto err;

        rd_kafka_wrlock(rk);
        rd_kafka_txn_coord_set(rk, rkb, "FindCoordinator response");
        rd_kafka_wrunlock(rk);

        rd_kafka_broker_destroy(rkb);

        return;

err_parse:
        err = rkbuf->rkbuf_err;
err:

        switch (err) {
        case RD_KAFKA_RESP_ERR__DESTROY:
                return;

        case RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED:
        case RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED:
                rd_kafka_wrlock(rk);
                rd_kafka_txn_set_fatal_error(
                    rkb->rkb_rk, RD_DONT_LOCK, err,
                    "Failed to find transaction coordinator: %s: %s%s%s",
                    rd_kafka_broker_name(rkb), rd_kafka_err2str(err),
                    *errstr ? ": " : "", errstr);

                rd_kafka_idemp_set_state(rk, RD_KAFKA_IDEMP_STATE_FATAL_ERROR);
                rd_kafka_wrunlock(rk);
                return;

        case RD_KAFKA_RESP_ERR__UNKNOWN_BROKER:
                rd_kafka_metadata_refresh_brokers(rk, NULL, errstr);
                break;

        default:
                break;
        }

        rd_kafka_wrlock(rk);
        rd_kafka_txn_coord_set(
            rk, NULL, "Failed to find transaction coordinator: %s: %s",
            rd_kafka_err2name(err), *errstr ? errstr : rd_kafka_err2str(err));
        rd_kafka_wrunlock(rk);
}



/**
 * @brief Query for the transaction coordinator.
 *
 * @returns true if a fatal error was raised, else false.
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock(rk) MUST be held.
 */
rd_bool_t rd_kafka_txn_coord_query(rd_kafka_t *rk, const char *reason) {
        rd_kafka_resp_err_t err;
        char errstr[512];
        rd_kafka_broker_t *rkb;

        rd_assert(rd_kafka_is_transactional(rk));

        if (rk->rk_eos.txn_wait_coord) {
                rd_kafka_dbg(rk, EOS, "TXNCOORD",
                             "Not sending coordinator query (%s): "
                             "waiting for previous query to finish",
                             reason);
                return rd_false;
        }

        /* Find usable broker to query for the txn coordinator */
        rkb = rd_kafka_idemp_broker_any(rk, &err, errstr, sizeof(errstr));
        if (!rkb) {
                rd_kafka_dbg(rk, EOS, "TXNCOORD",
                             "Unable to query for transaction coordinator: "
                             "%s: %s",
                             reason, errstr);

                if (rd_kafka_idemp_check_error(rk, err, errstr, rd_false))
                        return rd_true;

                rd_kafka_txn_coord_timer_start(rk, 500);

                return rd_false;
        }

        rd_kafka_dbg(rk, EOS, "TXNCOORD",
                     "Querying for transaction coordinator: %s", reason);

        /* Send FindCoordinator request */
        err = rd_kafka_FindCoordinatorRequest(
            rkb, RD_KAFKA_COORD_TXN, rk->rk_conf.eos.transactional_id,
            RD_KAFKA_REPLYQ(rk->rk_ops, 0), rd_kafka_txn_handle_FindCoordinator,
            NULL);

        if (err) {
                rd_snprintf(errstr, sizeof(errstr),
                            "Failed to send coordinator query to %s: "
                            "%s",
                            rd_kafka_broker_name(rkb), rd_kafka_err2str(err));

                rd_kafka_broker_destroy(rkb);

                if (rd_kafka_idemp_check_error(rk, err, errstr, rd_false))
                        return rd_true; /* Fatal error */

                rd_kafka_txn_coord_timer_start(rk, 500);

                return rd_false;
        }

        rd_kafka_broker_destroy(rkb);

        rk->rk_eos.txn_wait_coord = rd_true;

        return rd_false;
}

/**
 * @brief Sets or clears the current coordinator address.
 *
 * @returns true if the coordinator was changed, else false.
 *
 * @locality rdkafka main thread
 * @locks rd_kafka_wrlock(rk) MUST be held
 */
rd_bool_t rd_kafka_txn_coord_set(rd_kafka_t *rk,
                                 rd_kafka_broker_t *rkb,
                                 const char *fmt,
                                 ...) {
        char buf[256];
        va_list ap;

        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);


        if (rk->rk_eos.txn_curr_coord == rkb) {
                if (!rkb) {
                        rd_kafka_dbg(rk, EOS, "TXNCOORD", "%s", buf);
                        /* Keep querying for the coordinator */
                        rd_kafka_txn_coord_timer_start(rk, 500);
                }
                return rd_false;
        }

        rd_kafka_dbg(rk, EOS, "TXNCOORD",
                     "Transaction coordinator changed from %s -> %s: %s",
                     rk->rk_eos.txn_curr_coord
                         ? rd_kafka_broker_name(rk->rk_eos.txn_curr_coord)
                         : "(none)",
                     rkb ? rd_kafka_broker_name(rkb) : "(none)", buf);

        if (rk->rk_eos.txn_curr_coord)
                rd_kafka_broker_destroy(rk->rk_eos.txn_curr_coord);

        rk->rk_eos.txn_curr_coord = rkb;
        if (rkb)
                rd_kafka_broker_keep(rkb);

        rd_kafka_broker_set_nodename(rk->rk_eos.txn_coord,
                                     rk->rk_eos.txn_curr_coord);

        if (!rkb) {
                /* Lost the current coordinator, query for new coordinator */
                rd_kafka_txn_coord_timer_start(rk, 500);
        } else {
                /* Trigger PID state machine */
                rd_kafka_idemp_pid_fsm(rk);
        }

        return rd_true;
}


/**
 * @brief Coordinator state monitor callback.
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_txn_coord_monitor_cb(rd_kafka_broker_t *rkb) {
        rd_kafka_t *rk                = rkb->rkb_rk;
        rd_kafka_broker_state_t state = rd_kafka_broker_get_state(rkb);
        rd_bool_t is_up;

        rd_assert(rk->rk_eos.txn_coord == rkb);

        is_up = rd_kafka_broker_state_is_up(state);
        rd_rkb_dbg(rkb, EOS, "COORD", "Transaction coordinator is now %s",
                   is_up ? "up" : "down");

        if (!is_up) {
                /* Coordinator is down, the connection will be re-established
                 * automatically, but we also trigger a coordinator query
                 * to pick up on coordinator change. */
                rd_kafka_txn_coord_timer_start(rk, 500);

        } else {
                /* Coordinator is up. */

                rd_kafka_wrlock(rk);
                if (rk->rk_eos.idemp_state < RD_KAFKA_IDEMP_STATE_ASSIGNED) {
                        /* See if a idempotence state change is warranted. */
                        rd_kafka_idemp_pid_fsm(rk);

                } else if (rk->rk_eos.idemp_state ==
                           RD_KAFKA_IDEMP_STATE_ASSIGNED) {
                        /* PID is already valid, continue transactional
                         * operations by checking for partitions to register */
                        rd_kafka_txn_schedule_register_partitions(rk,
                                                                  1 /*ASAP*/);
                }

                rd_kafka_wrunlock(rk);
        }
}



/**
 * @brief Transactions manager destructor
 *
 * @locality rdkafka main thread
 * @locks none
 */
void rd_kafka_txns_term(rd_kafka_t *rk) {

        RD_IF_FREE(rk->rk_eos.txn_errstr, rd_free);
        RD_IF_FREE(rk->rk_eos.txn_curr_api.error, rd_kafka_error_destroy);

        mtx_destroy(&rk->rk_eos.txn_curr_api.lock);
        cnd_destroy(&rk->rk_eos.txn_curr_api.cnd);

        rd_kafka_timer_stop(&rk->rk_timers, &rk->rk_eos.txn_coord_tmr, 1);
        rd_kafka_timer_stop(&rk->rk_timers, &rk->rk_eos.txn_register_parts_tmr,
                            1);

        if (rk->rk_eos.txn_curr_coord)
                rd_kafka_broker_destroy(rk->rk_eos.txn_curr_coord);

        /* Logical coordinator */
        rd_kafka_broker_persistent_connection_del(
            rk->rk_eos.txn_coord, &rk->rk_eos.txn_coord->rkb_persistconn.coord);
        rd_kafka_broker_monitor_del(&rk->rk_eos.txn_coord_mon);
        rd_kafka_broker_destroy(rk->rk_eos.txn_coord);
        rk->rk_eos.txn_coord = NULL;

        mtx_lock(&rk->rk_eos.txn_pending_lock);
        rd_kafka_txn_clear_pending_partitions(rk);
        mtx_unlock(&rk->rk_eos.txn_pending_lock);
        mtx_destroy(&rk->rk_eos.txn_pending_lock);

        rd_kafka_txn_clear_partitions(rk);
}


/**
 * @brief Initialize transactions manager.
 *
 * @locality application thread
 * @locks none
 */
void rd_kafka_txns_init(rd_kafka_t *rk) {
        rd_atomic32_init(&rk->rk_eos.txn_may_enq, 0);
        mtx_init(&rk->rk_eos.txn_pending_lock, mtx_plain);
        TAILQ_INIT(&rk->rk_eos.txn_pending_rktps);
        TAILQ_INIT(&rk->rk_eos.txn_waitresp_rktps);
        TAILQ_INIT(&rk->rk_eos.txn_rktps);

        mtx_init(&rk->rk_eos.txn_curr_api.lock, mtx_plain);
        cnd_init(&rk->rk_eos.txn_curr_api.cnd);

        /* Logical coordinator */
        rk->rk_eos.txn_coord =
            rd_kafka_broker_add_logical(rk, "TxnCoordinator");

        rd_kafka_broker_monitor_add(&rk->rk_eos.txn_coord_mon,
                                    rk->rk_eos.txn_coord, rk->rk_ops,
                                    rd_kafka_txn_coord_monitor_cb);

        rd_kafka_broker_persistent_connection_add(
            rk->rk_eos.txn_coord, &rk->rk_eos.txn_coord->rkb_persistconn.coord);

        rd_atomic64_init(&rk->rk_eos.txn_dr_fails, 0);
}
