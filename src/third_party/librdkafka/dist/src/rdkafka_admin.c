/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018-2022, Magnus Edenhill
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
#include "rdkafka_admin.h"
#include "rdkafka_request.h"
#include "rdkafka_aux.h"

#include <stdarg.h>



/** @brief Descriptive strings for rko_u.admin_request.state */
static const char *rd_kafka_admin_state_desc[] = {
    "initializing",
    "waiting for broker",
    "waiting for controller",
    "waiting for fanouts",
    "constructing request",
    "waiting for response from broker",
    "waiting for a valid list of brokers to be available"};



/**
 * @brief Admin API implementation.
 *
 * The public Admin API in librdkafka exposes a completely asynchronous
 * interface where the initial request API (e.g., ..CreateTopics())
 * is non-blocking and returns immediately, and the application polls
 * a ..queue_t for the result.
 *
 * The underlying handling of the request is also completely asynchronous
 * inside librdkafka, for two reasons:
 *  - everything is async in librdkafka so adding something new that isn't
 *    would mean that existing functionality will need to be changed if
 *    it should be able to work simultaneously (such as statistics, timers,
 *    etc). There is no functional value to making the admin API
 *    synchronous internally, even if it would simplify its implementation.
 *    So making it async allows the Admin API to be used with existing
 *    client types in existing applications without breakage.
 *  - the async approach allows multiple outstanding Admin API requests
 *    simultaneously.
 *
 * The internal async implementation relies on the following concepts:
 *  - it uses a single rko (rd_kafka_op_t) to maintain state.
 *  - the rko has a callback attached - called the worker callback.
 *  - the worker callback is a small state machine that triggers
 *    async operations (be it controller lookups, timeout timers,
 *    protocol transmits, etc).
 *  - the worker callback is only called on the rdkafka main thread.
 *  - the callback is triggered by different events and sources by enqueuing
 *    the rko on the rdkafka main ops queue.
 *
 *
 * Let's illustrate this with a DeleteTopics example. This might look
 * daunting, but it boils down to an asynchronous state machine being
 * triggered by enqueuing the rko op.
 *
 *  1. [app thread] The user constructs the input arguments,
 *     including a response rkqu queue and then calls DeleteTopics().
 *
 *  2. [app thread] DeleteTopics() creates a new internal op (rko) of type
 *     RD_KAFKA_OP_DELETETOPICS, makes a **copy** on the rko of all the
 *     input arguments (which allows the caller to free the originals
 *     whenever she likes). The rko op worker callback is set to the
 *     generic admin worker callback rd_kafka_admin_worker()
 *
 *  3. [app thread] DeleteTopics() enqueues the rko on librdkafka's main ops
 *     queue that is served by the rdkafka main thread in rd_kafka_thread_main()
 *
 *  4. [rdkafka main thread] The rko is dequeued by rd_kafka_q_serve and
 *     the rd_kafka_poll_cb() is called.
 *
 *  5. [rdkafka main thread] The rko_type switch case identifies the rko
 *     as an RD_KAFKA_OP_DELETETOPICS which is served by the op callback
 *     set in step 2.
 *
 *  6. [rdkafka main thread] The worker callback is called.
 *     After some initial checking of err==ERR__DESTROY events
 *     (which is used to clean up outstanding ops (etc) on termination),
 *     the code hits a state machine using rko_u.admin_request.state.
 *
 *  7. [rdkafka main thread] The initial state is RD_KAFKA_ADMIN_STATE_INIT
 *     where the worker validates the user input.
 *     An enqueue once (eonce) object is created - the use of this object
 *     allows having multiple outstanding async functions referencing the
 *     same underlying rko object, but only allowing the first one
 *     to trigger an event.
 *     A timeout timer is set up to trigger the eonce object when the
 *     full options.request_timeout has elapsed.
 *
 *  8. [rdkafka main thread] After initialization the state is updated
 *     to WAIT_BROKER or WAIT_CONTROLLER and the code falls through to
 *     looking up a specific broker or the controller broker and waiting for
 *     an active connection.
 *     Both the lookup and the waiting for an active connection are
 *     fully asynchronous, and the same eonce used for the timer is passed
 *     to the rd_kafka_broker_controller_async() or broker_async() functions
 *     which will trigger the eonce when a broker state change occurs.
 *     If the controller is already known (from metadata) and the connection
 *     is up a rkb broker object is returned and the eonce is not used,
 *     skip to step 11.
 *
 *  9. [rdkafka main thread] Upon metadata retrieval (which is triggered
 *     automatically by other parts of the code) the controller_id may be
 *     updated in which case the eonce is triggered.
 *     The eonce triggering enqueues the original rko on the rdkafka main
 *     ops queue again and we go to step 8 which will check if the controller
 *     connection is up.
 *
 * 10. [broker thread] If the controller_id is now known we wait for
 *     the corresponding broker's connection to come up. This signaling
 *     is performed from the broker thread upon broker state changes
 *     and uses the same eonce. The eonce triggering enqueues the original
 *     rko on the rdkafka main ops queue again we go to back to step 8
 *     to check if broker is now available.
 *
 * 11. [rdkafka main thread] Back in the worker callback we now have an
 *     rkb broker pointer (with reference count increased) for the controller
 *     with the connection up (it might go down while we're referencing it,
 *     but that does not stop us from enqueuing a protocol request).
 *
 * 12. [rdkafka main thread] A DeleteTopics protocol request buffer is
 *     constructed using the input parameters saved on the rko and the
 *     buffer is enqueued on the broker's transmit queue.
 *     The buffer is set up to provide the reply buffer on the rdkafka main
 *     ops queue (the same queue we are operating from) with a handler
 *     callback of rd_kafka_admin_handle_response().
 *     The state is updated to the RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE.
 *
 * 13. [broker thread] If the request times out, a response with error code
 *     (ERR__TIMED_OUT) is enqueued. Go to 16.
 *
 * 14. [broker thread] If a response is received, the response buffer
 *     is enqueued. Go to 16.
 *
 * 15. [rdkafka main thread] The buffer callback (..handle_response())
 *     is called, which attempts to extract the original rko from the eonce,
 *     but if the eonce has already been triggered by some other source
 *     (the timeout timer) the buffer callback simply returns and does nothing
 *     since the admin request is over and a result (probably a timeout)
 *     has been enqueued for the application.
 *     If the rko was still intact we temporarily set the reply buffer
 *     in the rko struct and call the worker callback. Go to 17.
 *
 * 16. [rdkafka main thread] The worker callback is called in state
 *     RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE without a response but with an error.
 *     An error result op is created and enqueued on the application's
 *     provided response rkqu queue.
 *
 * 17. [rdkafka main thread] The worker callback is called in state
 *     RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE with a response buffer with no
 *     error set.
 *     The worker calls the response `parse()` callback to parse the response
 *     buffer and populates a result op (rko_result) with the response
 *     information (such as per-topic error codes, etc).
 *     The result op is returned to the worker.
 *
 * 18. [rdkafka main thread] The worker enqueues the result op (rko_result)
 *     on the application's provided response rkqu queue.
 *
 * 19. [app thread] The application calls rd_kafka_queue_poll() to
 *     receive the result of the operation. The result may have been
 *     enqueued in step 18 thanks to succesful completion, or in any
 *     of the earlier stages when an error was encountered.
 *
 * 20. [app thread] The application uses rd_kafka_event_DeleteTopics_result()
 *     to retrieve the request-specific result type.
 *
 * 21. Done.
 *
 *
 *
 *
 * Fanout (RD_KAFKA_OP_ADMIN_FANOUT) requests
 * ------------------------------------------
 *
 * Certain Admin APIs may have requests that need to be sent to different
 * brokers, for instance DeleteRecords which needs to be sent to the leader
 * for each given partition.
 *
 * To achieve this we create a Fanout (RD_KAFKA_OP_ADMIN_FANOUT) op for the
 * overall Admin API call (e.g., DeleteRecords), and then sub-ops for each
 * of the per-broker requests. These sub-ops have the proper op type for
 * the operation they are performing (e.g., RD_KAFKA_OP_DELETERECORDS)
 * but their replyq does not point back to the application replyq but
 * rk_ops which is handled by the librdkafka main thread and with the op
 * callback set to rd_kafka_admin_fanout_worker(). This worker aggregates
 * the results of each fanned out sub-op and merges the result into a
 * single result op (RD_KAFKA_OP_ADMIN_RESULT) that is enqueued on the
 * application's replyq.
 *
 * We rely on the timeouts on the fanned out sub-ops rather than the parent
 * fanout op.
 *
 * The parent fanout op must not be destroyed until all fanned out sub-ops
 * are done (either by success, failure or timeout) and destroyed, and this
 * is tracked by the rko_u.admin_request.fanout.outstanding counter.
 *
 */


/**
 * @enum Admin request target broker. Must be negative values since the field
 *       used is broker_id.
 */
enum {
        RD_KAFKA_ADMIN_TARGET_CONTROLLER  = -1, /**< Cluster controller */
        RD_KAFKA_ADMIN_TARGET_COORDINATOR = -2, /**< (Group) Coordinator */
        RD_KAFKA_ADMIN_TARGET_FANOUT      = -3, /**< This rko is a fanout and
                                                 *   and has no target broker */
        RD_KAFKA_ADMIN_TARGET_ALL = -4,         /**< All available brokers */
};

/**
 * @brief Admin op callback types
 */
typedef rd_kafka_resp_err_t(rd_kafka_admin_Request_cb_t)(
    rd_kafka_broker_t *rkb,
    const rd_list_t *configs /*(ConfigResource_t*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) RD_WARN_UNUSED_RESULT;

typedef rd_kafka_resp_err_t(rd_kafka_admin_Response_parse_cb_t)(
    rd_kafka_op_t *rko_req,
    rd_kafka_op_t **rko_resultp,
    rd_kafka_buf_t *reply,
    char *errstr,
    size_t errstr_size) RD_WARN_UNUSED_RESULT;

typedef void(rd_kafka_admin_fanout_PartialResponse_cb_t)(
    rd_kafka_op_t *rko_req,
    const rd_kafka_op_t *rko_partial);

typedef rd_list_copy_cb_t rd_kafka_admin_fanout_CopyResult_cb_t;

typedef rd_list_copy_cb_t rd_kafka_admin_fanout_CopyArg_cb_t;

/**
 * @struct Request-specific worker callbacks.
 */
struct rd_kafka_admin_worker_cbs {
        /**< Protocol request callback which is called
         *   to construct and send the request. */
        rd_kafka_admin_Request_cb_t *request;

        /**< Protocol response parser callback which is called
         *   to translate the response to a rko_result op. */
        rd_kafka_admin_Response_parse_cb_t *parse;
};

/**
 * @struct Fanout request callbacks.
 */
struct rd_kafka_admin_fanout_worker_cbs {
        /** Merge results from a fanned out request into the user response. */
        rd_kafka_admin_fanout_PartialResponse_cb_t *partial_response;

        /** Copy an accumulated result for storing into the rko_result. */
        rd_kafka_admin_fanout_CopyResult_cb_t *copy_result;

        /** Copy the original arguments, used by target ALL. */
        rd_kafka_admin_fanout_CopyArg_cb_t *copy_arg;
};

/* Forward declarations */
static void rd_kafka_admin_common_worker_destroy(rd_kafka_t *rk,
                                                 rd_kafka_op_t *rko,
                                                 rd_bool_t do_destroy);
static void rd_kafka_AdminOptions_init(rd_kafka_t *rk,
                                       rd_kafka_AdminOptions_t *options);

static void rd_kafka_AdminOptions_copy_to(rd_kafka_AdminOptions_t *dst,
                                          const rd_kafka_AdminOptions_t *src);

static rd_kafka_op_res_t
rd_kafka_admin_worker(rd_kafka_t *rk, rd_kafka_q_t *rkq, rd_kafka_op_t *rko);
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_copy(const rd_kafka_ConfigEntry_t *src);
static void rd_kafka_ConfigEntry_free(void *ptr);
static void *rd_kafka_ConfigEntry_list_copy(const void *src, void *opaque);

static void rd_kafka_admin_handle_response(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb,
                                           rd_kafka_resp_err_t err,
                                           rd_kafka_buf_t *reply,
                                           rd_kafka_buf_t *request,
                                           void *opaque);

static rd_kafka_op_res_t
rd_kafka_admin_fanout_worker(rd_kafka_t *rk,
                             rd_kafka_q_t *rkq,
                             rd_kafka_op_t *rko_fanout);


/**
 * @name Common admin request code
 * @{
 *
 *
 */

/**
 * @brief Create a new admin_result op based on the request op \p rko_req.
 *
 * @remark This moves the rko_req's admin_request.args list from \p rko_req
 *         to the returned rko. The \p rko_req args will be emptied.
 */
static rd_kafka_op_t *rd_kafka_admin_result_new(rd_kafka_op_t *rko_req) {
        rd_kafka_op_t *rko_result;
        rd_kafka_op_t *rko_fanout;

        if ((rko_fanout = rko_req->rko_u.admin_request.fanout_parent)) {
                /* If this is a fanned out request the rko_result needs to be
                 * handled by the fanout worker rather than the application. */
                rko_result = rd_kafka_op_new_cb(rko_req->rko_rk,
                                                RD_KAFKA_OP_ADMIN_RESULT,
                                                rd_kafka_admin_fanout_worker);
                /* Transfer fanout pointer to result */
                rko_result->rko_u.admin_result.fanout_parent = rko_fanout;
                rko_req->rko_u.admin_request.fanout_parent   = NULL;
                /* Set event type based on original fanout ops reqtype,
                 * e.g., ..OP_DELETERECORDS */
                rko_result->rko_u.admin_result.reqtype =
                    rko_fanout->rko_u.admin_request.fanout.reqtype;

        } else {
                rko_result = rd_kafka_op_new(RD_KAFKA_OP_ADMIN_RESULT);

                /* If this is fanout request (i.e., the parent OP_ADMIN_FANOUT
                 * to fanned out requests) we need to use the original
                 * application request type. */
                if (rko_req->rko_type == RD_KAFKA_OP_ADMIN_FANOUT)
                        rko_result->rko_u.admin_result.reqtype =
                            rko_req->rko_u.admin_request.fanout.reqtype;
                else
                        rko_result->rko_u.admin_result.reqtype =
                            rko_req->rko_type;
        }

        rko_result->rko_rk = rko_req->rko_rk;

        rko_result->rko_u.admin_result.opaque = rd_kafka_confval_get_ptr(
            &rko_req->rko_u.admin_request.options.opaque);

        /* Move request arguments (list) from request to result.
         * This is mainly so that partial_response() knows what arguments
         * were provided to the response's request it is merging. */
        rd_list_move(&rko_result->rko_u.admin_result.args,
                     &rko_req->rko_u.admin_request.args);

        rko_result->rko_evtype = rko_req->rko_u.admin_request.reply_event_type;

        rko_result->rko_u.admin_result.cbs = rko_req->rko_u.admin_request.cbs;

        return rko_result;
}


/**
 * @brief Set error code and error string on admin_result op \p rko.
 */
static void rd_kafka_admin_result_set_err0(rd_kafka_op_t *rko,
                                           rd_kafka_resp_err_t err,
                                           const char *fmt,
                                           va_list ap) {
        char buf[512];

        rd_vsnprintf(buf, sizeof(buf), fmt, ap);

        rko->rko_err = err;

        if (rko->rko_u.admin_result.errstr)
                rd_free(rko->rko_u.admin_result.errstr);
        rko->rko_u.admin_result.errstr = rd_strdup(buf);

        rd_kafka_dbg(rko->rko_rk, ADMIN, "ADMINFAIL",
                     "Admin %s result error: %s",
                     rd_kafka_op2str(rko->rko_u.admin_result.reqtype),
                     rko->rko_u.admin_result.errstr);
}

/**
 * @sa rd_kafka_admin_result_set_err0
 */
static RD_UNUSED RD_FORMAT(printf, 3, 4) void rd_kafka_admin_result_set_err(
    rd_kafka_op_t *rko,
    rd_kafka_resp_err_t err,
    const char *fmt,
    ...) {
        va_list ap;

        va_start(ap, fmt);
        rd_kafka_admin_result_set_err0(rko, err, fmt, ap);
        va_end(ap);
}

/**
 * @brief Enqueue admin_result on application's queue.
 */
static RD_INLINE void rd_kafka_admin_result_enq(rd_kafka_op_t *rko_req,
                                                rd_kafka_op_t *rko_result) {
        if (rko_req->rko_u.admin_result.result_cb)
                rko_req->rko_u.admin_result.result_cb(rko_result);
        rd_kafka_replyq_enq(&rko_req->rko_u.admin_request.replyq, rko_result,
                            rko_req->rko_u.admin_request.replyq.version);
}

/**
 * @brief Set request-level error code and string in reply op.
 *
 * @remark This function will NOT destroy the \p rko_req, so don't forget to
 *         call rd_kafka_admin_common_worker_destroy() when done with the rko.
 */
static RD_FORMAT(printf,
                 3,
                 4) void rd_kafka_admin_result_fail(rd_kafka_op_t *rko_req,
                                                    rd_kafka_resp_err_t err,
                                                    const char *fmt,
                                                    ...) {
        va_list ap;
        rd_kafka_op_t *rko_result;

        if (!rko_req->rko_u.admin_request.replyq.q)
                return;

        rko_result = rd_kafka_admin_result_new(rko_req);

        va_start(ap, fmt);
        rd_kafka_admin_result_set_err0(rko_result, err, fmt, ap);
        va_end(ap);

        rd_kafka_admin_result_enq(rko_req, rko_result);
}


/**
 * @brief Send the admin request contained in \p rko upon receiving
 *        a FindCoordinator response.
 *
 * @param opaque Must be an admin request op's eonce (rko_u.admin_request.eonce)
 *               (i.e. created by \c rd_kafka_admin_request_op_new )
 *
 * @remark To be used as a callback for \c rd_kafka_coord_req
 */
static rd_kafka_resp_err_t
rd_kafka_admin_coord_request(rd_kafka_broker_t *rkb,
                             rd_kafka_op_t *rko_ignore,
                             rd_kafka_replyq_t replyq,
                             rd_kafka_resp_cb_t *resp_cb,
                             void *opaque) {
        rd_kafka_t *rk             = rkb->rkb_rk;
        rd_kafka_enq_once_t *eonce = opaque;
        rd_kafka_op_t *rko;
        char errstr[512];
        rd_kafka_resp_err_t err;


        rko = rd_kafka_enq_once_del_source_return(eonce, "coordinator request");
        if (!rko)
                /* Admin request has timed out and been destroyed */
                return RD_KAFKA_RESP_ERR__DESTROY;

        rd_kafka_enq_once_add_source(eonce, "coordinator response");

        err = rko->rko_u.admin_request.cbs->request(
            rkb, &rko->rko_u.admin_request.args,
            &rko->rko_u.admin_request.options, errstr, sizeof(errstr), replyq,
            rd_kafka_admin_handle_response, eonce);

        if (err) {
                rd_kafka_admin_result_fail(
                    rko, err, "%s worker failed to send request: %s",
                    rd_kafka_op2str(rko->rko_type), errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
        }
        return err;
}


/**
 * @brief Return the topics list from a topic-related result object.
 */
static const rd_kafka_topic_result_t **
rd_kafka_admin_result_ret_topics(const rd_kafka_op_t *rko, size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_CREATETOPICS ||
                  reqtype == RD_KAFKA_OP_DELETETOPICS ||
                  reqtype == RD_KAFKA_OP_CREATEPARTITIONS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_topic_result_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Return the ConfigResource list from a config-related result object.
 */
static const rd_kafka_ConfigResource_t **
rd_kafka_admin_result_ret_resources(const rd_kafka_op_t *rko, size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_ALTERCONFIGS ||
                  reqtype == RD_KAFKA_OP_DESCRIBECONFIGS ||
                  reqtype == RD_KAFKA_OP_INCREMENTALALTERCONFIGS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_ConfigResource_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Return the acl result list from a acl-related result object.
 */
static const rd_kafka_acl_result_t **
rd_kafka_admin_result_ret_acl_results(const rd_kafka_op_t *rko, size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_CREATEACLS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_acl_result_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Return the acl binding list from a acl-related result object.
 */
static const rd_kafka_AclBinding_t **
rd_kafka_admin_result_ret_acl_bindings(const rd_kafka_op_t *rko, size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DESCRIBEACLS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_AclBinding_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Return the groups list from a group-related result object.
 */
static const rd_kafka_group_result_t **
rd_kafka_admin_result_ret_groups(const rd_kafka_op_t *rko, size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DELETEGROUPS ||
                  reqtype == RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS ||
                  reqtype == RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS ||
                  reqtype == RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_group_result_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Return the DeleteAcls response list from a acl-related result object.
 */
static const rd_kafka_DeleteAcls_result_response_t **
rd_kafka_admin_result_ret_delete_acl_result_responses(const rd_kafka_op_t *rko,
                                                      size_t *cntp) {
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DELETEACLS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_DeleteAcls_result_response_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Create a new admin_request op of type \p optype and sets up the
 *        generic (type independent files).
 *
 *        The caller shall then populate the admin_request.args list
 *        and enqueue the op on rk_ops for further processing work.
 *
 * @param cbs Callbacks, must reside in .data segment.
 * @param options Optional options, may be NULL to use defaults.
 *
 * @locks none
 * @locality application thread
 */
static rd_kafka_op_t *
rd_kafka_admin_request_op_new(rd_kafka_t *rk,
                              rd_kafka_op_type_t optype,
                              rd_kafka_event_type_t reply_event_type,
                              const struct rd_kafka_admin_worker_cbs *cbs,
                              const rd_kafka_AdminOptions_t *options,
                              rd_kafka_q_t *rkq) {
        rd_kafka_op_t *rko;

        rd_assert(rk);
        rd_assert(rkq);
        rd_assert(cbs);

        rko = rd_kafka_op_new_cb(rk, optype, rd_kafka_admin_worker);

        rko->rko_u.admin_request.reply_event_type = reply_event_type;

        rko->rko_u.admin_request.cbs = (struct rd_kafka_admin_worker_cbs *)cbs;

        /* Make a copy of the options */
        if (options)
                rd_kafka_AdminOptions_copy_to(&rko->rko_u.admin_request.options,
                                              options);
        else
                rd_kafka_AdminOptions_init(rk,
                                           &rko->rko_u.admin_request.options);

        /* Default to controller */
        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_CONTROLLER;

        /* Calculate absolute timeout */
        rko->rko_u.admin_request.abs_timeout =
            rd_timeout_init(rd_kafka_confval_get_int(
                &rko->rko_u.admin_request.options.request_timeout));

        /* Setup enq-op-once, which is triggered by either timer code
         * or future wait-controller code. */
        rko->rko_u.admin_request.eonce =
            rd_kafka_enq_once_new(rko, RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* The timer itself must be started from the rdkafka main thread,
         * not here. */

        /* Set up replyq */
        rd_kafka_set_replyq(&rko->rko_u.admin_request.replyq, rkq, 0);

        rko->rko_u.admin_request.state = RD_KAFKA_ADMIN_STATE_INIT;
        return rko;
}

static void
rd_kafka_admin_request_op_result_cb_set(rd_kafka_op_t *op,
                                        void (*result_cb)(rd_kafka_op_t *)) {
        op->rko_u.admin_result.result_cb = result_cb;
}


/**
 * @returns the remaining request timeout in milliseconds.
 */
static RD_INLINE int rd_kafka_admin_timeout_remains(rd_kafka_op_t *rko) {
        return rd_timeout_remains(rko->rko_u.admin_request.abs_timeout);
}

/**
 * @returns the remaining request timeout in microseconds.
 */
static RD_INLINE rd_ts_t rd_kafka_admin_timeout_remains_us(rd_kafka_op_t *rko) {
        return rd_timeout_remains_us(rko->rko_u.admin_request.abs_timeout);
}


/**
 * @brief Timer timeout callback for the admin rko's eonce object.
 */
static void rd_kafka_admin_eonce_timeout_cb(rd_kafka_timers_t *rkts,
                                            void *arg) {
        rd_kafka_enq_once_t *eonce = arg;

        rd_kafka_enq_once_trigger(eonce, RD_KAFKA_RESP_ERR__TIMED_OUT,
                                  "timeout timer");
}



/**
 * @brief Common worker destroy to be called in destroy: label
 *        in worker.
 */
static void rd_kafka_admin_common_worker_destroy(rd_kafka_t *rk,
                                                 rd_kafka_op_t *rko,
                                                 rd_bool_t do_destroy) {
        int timer_was_stopped;

        /* Free resources for this op. */
        timer_was_stopped = rd_kafka_timer_stop(
            &rk->rk_timers, &rko->rko_u.admin_request.tmr, rd_true);


        if (rko->rko_u.admin_request.eonce) {
                /* Remove the stopped timer's eonce reference since its
                 * callback will not have fired if we stopped the timer. */
                if (timer_was_stopped)
                        rd_kafka_enq_once_del_source(
                            rko->rko_u.admin_request.eonce, "timeout timer");

                /* This is thread-safe to do even if there are outstanding
                 * timers or wait-controller references to the eonce
                 * since they only hold direct reference to the eonce,
                 * not the rko (the eonce holds a reference to the rko but
                 * it is cleared here). */
                rd_kafka_enq_once_destroy(rko->rko_u.admin_request.eonce);
                rko->rko_u.admin_request.eonce = NULL;
        }

        if (do_destroy)
                rd_kafka_op_destroy(rko);
}



/**
 * @brief Asynchronously look up a broker.
 *        To be called repeatedly from each invocation of the worker
 *        when in state RD_KAFKA_ADMIN_STATE_WAIT_BROKER until
 *        a valid rkb is returned.
 *
 * @returns the broker rkb with refcount increased, or NULL if not yet
 *          available.
 */
static rd_kafka_broker_t *rd_kafka_admin_common_get_broker(rd_kafka_t *rk,
                                                           rd_kafka_op_t *rko,
                                                           int32_t broker_id) {
        rd_kafka_broker_t *rkb;

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: looking up broker %" PRId32,
                     rd_kafka_op2str(rko->rko_type), broker_id);

        /* Since we're iterating over this broker_async() call
         * (asynchronously) until a broker is availabe (or timeout)
         * we need to re-enable the eonce to be triggered again (which
         * is not necessary the first time we get here, but there
         * is no harm doing it then either). */
        rd_kafka_enq_once_reenable(rko->rko_u.admin_request.eonce, rko,
                                   RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* Look up the broker asynchronously, if the broker
         * is not available the eonce is registered for broker
         * state changes which will cause our function to be called
         * again as soon as (any) broker state changes.
         * When we are called again we perform the broker lookup
         * again and hopefully get an rkb back, otherwise defer a new
         * async wait. Repeat until success or timeout. */
        if (!(rkb = rd_kafka_broker_get_async(
                  rk, broker_id, RD_KAFKA_BROKER_STATE_UP,
                  rko->rko_u.admin_request.eonce))) {
                /* Broker not available, wait asynchronously
                 * for broker metadata code to trigger eonce. */
                return NULL;
        }

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: broker %" PRId32 " is %s",
                     rd_kafka_op2str(rko->rko_type), broker_id, rkb->rkb_name);

        return rkb;
}


/**
 * @brief Asynchronously look up the controller.
 *        To be called repeatedly from each invocation of the worker
 *        when in state RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER until
 *        a valid rkb is returned.
 *
 * @returns the controller rkb with refcount increased, or NULL if not yet
 *          available.
 */
static rd_kafka_broker_t *
rd_kafka_admin_common_get_controller(rd_kafka_t *rk, rd_kafka_op_t *rko) {
        rd_kafka_broker_t *rkb;

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: looking up controller",
                     rd_kafka_op2str(rko->rko_type));

        /* Since we're iterating over this controller_async() call
         * (asynchronously) until a controller is availabe (or timeout)
         * we need to re-enable the eonce to be triggered again (which
         * is not necessary the first time we get here, but there
         * is no harm doing it then either). */
        rd_kafka_enq_once_reenable(rko->rko_u.admin_request.eonce, rko,
                                   RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* Look up the controller asynchronously, if the controller
         * is not available the eonce is registered for broker
         * state changes which will cause our function to be called
         * again as soon as (any) broker state changes.
         * When we are called again we perform the controller lookup
         * again and hopefully get an rkb back, otherwise defer a new
         * async wait. Repeat until success or timeout. */
        if (!(rkb = rd_kafka_broker_controller_async(
                  rk, RD_KAFKA_BROKER_STATE_UP,
                  rko->rko_u.admin_request.eonce))) {
                /* Controller not available, wait asynchronously
                 * for controller code to trigger eonce. */
                return NULL;
        }

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: controller %s",
                     rd_kafka_op2str(rko->rko_type), rkb->rkb_name);

        return rkb;
}


/**
 * @brief Asynchronously look up current list of broker ids until available.
 *        Bootstrap and logical brokers are excluded from the list.
 *
 *        To be called repeatedly from each invocation of the worker
 *        when in state RD_KAFKA_ADMIN_STATE_WAIT_BROKER_LIST until
 *        a not-NULL rd_list_t * is returned.
 *
 * @param rk Client instance.
 * @param rko Op containing the admin request eonce to use for the
 *            async callback.
 * @return List of int32_t with broker nodeids when ready, NULL when
 *         the eonce callback will be called.
 */
static rd_list_t *
rd_kafka_admin_common_brokers_get_nodeids(rd_kafka_t *rk, rd_kafka_op_t *rko) {
        rd_list_t *broker_ids;

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: looking up brokers",
                     rd_kafka_op2str(rko->rko_type));

        /* Since we're iterating over this rd_kafka_brokers_get_nodeids_async()
         * call (asynchronously) until a nodeids list is available (or timeout),
         * we need to re-enable the eonce to be triggered again (which
         * is not necessary the first time we get here, but there
         * is no harm doing it then either). */
        rd_kafka_enq_once_reenable(rko->rko_u.admin_request.eonce, rko,
                                   RD_KAFKA_REPLYQ(rk->rk_ops, 0));

        /* Look up the nodeids list asynchronously, if it's
         * not available the eonce is registered for broker
         * state changes which will cause our function to be called
         * again as soon as (any) broker state changes.
         * When we are called again we perform the same lookup
         * again and hopefully get a list of nodeids again,
         * otherwise defer a new async wait.
         * Repeat until success or timeout. */
        if (!(broker_ids = rd_kafka_brokers_get_nodeids_async(
                  rk, rko->rko_u.admin_request.eonce))) {
                /* nodeids list not available, wait asynchronously
                 * for the eonce to be triggered. */
                return NULL;
        }

        rd_kafka_dbg(rk, ADMIN, "ADMIN", "%s: %d broker(s)",
                     rd_kafka_op2str(rko->rko_type), rd_list_cnt(broker_ids));

        return broker_ids;
}



/**
 * @brief Handle response from broker by triggering worker callback.
 *
 * @param opaque is the eonce from the worker protocol request call.
 */
static void rd_kafka_admin_handle_response(rd_kafka_t *rk,
                                           rd_kafka_broker_t *rkb,
                                           rd_kafka_resp_err_t err,
                                           rd_kafka_buf_t *reply,
                                           rd_kafka_buf_t *request,
                                           void *opaque) {
        rd_kafka_enq_once_t *eonce = opaque;
        rd_kafka_op_t *rko;

        /* From ...add_source("send") */
        rko = rd_kafka_enq_once_disable(eonce);

        if (!rko) {
                /* The operation timed out and the worker was
                 * dismantled while we were waiting for broker response,
                 * do nothing - everything has been cleaned up. */
                rd_kafka_dbg(
                    rk, ADMIN, "ADMIN",
                    "Dropping outdated %sResponse with return code %s",
                    request ? rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey)
                            : "???",
                    rd_kafka_err2str(err));
                return;
        }

        /* Attach reply buffer to rko for parsing in the worker. */
        rd_assert(!rko->rko_u.admin_request.reply_buf);
        rko->rko_u.admin_request.reply_buf = reply;
        rko->rko_err                       = err;

        if (rko->rko_op_cb(rk, NULL, rko) == RD_KAFKA_OP_RES_HANDLED)
                rd_kafka_op_destroy(rko);
}

/**
 * @brief Generic handler for protocol responses, calls the admin ops'
 *        Response_parse_cb and enqueues the result to the caller's queue.
 */
static void rd_kafka_admin_response_parse(rd_kafka_op_t *rko) {
        rd_kafka_resp_err_t err;
        rd_kafka_op_t *rko_result = NULL;
        char errstr[512];

        if (rko->rko_err) {
                rd_kafka_admin_result_fail(rko, rko->rko_err,
                                           "%s worker request failed: %s",
                                           rd_kafka_op2str(rko->rko_type),
                                           rd_kafka_err2str(rko->rko_err));
                return;
        }

        /* Response received.
         * Let callback parse response and provide result in rko_result
         * which is then enqueued on the reply queue. */
        err = rko->rko_u.admin_request.cbs->parse(
            rko, &rko_result, rko->rko_u.admin_request.reply_buf, errstr,
            sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(
                    rko, err, "%s worker failed to parse response: %s",
                    rd_kafka_op2str(rko->rko_type), errstr);
                return;
        }

        rd_assert(rko_result);

        /* Enqueue result on application queue, we're done. */
        rd_kafka_admin_result_enq(rko, rko_result);
}

/**
 * @brief Generic handler for coord_req() responses.
 */
static void rd_kafka_admin_coord_response_parse(rd_kafka_t *rk,
                                                rd_kafka_broker_t *rkb,
                                                rd_kafka_resp_err_t err,
                                                rd_kafka_buf_t *rkbuf,
                                                rd_kafka_buf_t *request,
                                                void *opaque) {
        rd_kafka_op_t *rko_result;
        rd_kafka_enq_once_t *eonce = opaque;
        rd_kafka_op_t *rko;
        char errstr[512];

        rko =
            rd_kafka_enq_once_del_source_return(eonce, "coordinator response");
        if (!rko)
                /* Admin request has timed out and been destroyed */
                return;

        if (err) {
                rd_kafka_admin_result_fail(
                    rko, err, "%s worker coordinator request failed: %s",
                    rd_kafka_op2str(rko->rko_type), rd_kafka_err2str(err));
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        err = rko->rko_u.admin_request.cbs->parse(rko, &rko_result, rkbuf,
                                                  errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(
                    rko, err,
                    "%s worker failed to parse coordinator %sResponse: %s",
                    rd_kafka_op2str(rko->rko_type),
                    rd_kafka_ApiKey2str(request->rkbuf_reqhdr.ApiKey), errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_assert(rko_result);

        /* Enqueue result on application queue, we're done. */
        rd_kafka_admin_result_enq(rko, rko_result);
}

static void rd_kafka_admin_fanout_op_distribute(rd_kafka_t *rk,
                                                rd_kafka_op_t *rko,
                                                rd_list_t *nodeids);


/**
 * @brief Common worker state machine handling regardless of request type.
 *
 * Tasks:
 *  - Sets up timeout on first call.
 *  - Checks for timeout.
 *  - Checks for and fails on errors.
 *  - Async Controller and broker lookups
 *  - Calls the Request callback
 *  - Calls the parse callback
 *  - Result reply
 *  - Destruction of rko
 *
 * rko->rko_err may be one of:
 * RD_KAFKA_RESP_ERR_NO_ERROR, or
 * RD_KAFKA_RESP_ERR__DESTROY for queue destruction cleanup, or
 * RD_KAFKA_RESP_ERR__TIMED_OUT if request has timed out,
 * or any other error code triggered by other parts of the code.
 *
 * @returns a hint to the op code whether the rko should be destroyed or not.
 */
static rd_kafka_op_res_t
rd_kafka_admin_worker(rd_kafka_t *rk, rd_kafka_q_t *rkq, rd_kafka_op_t *rko) {
        const char *name = rd_kafka_op2str(rko->rko_type);
        rd_ts_t timeout_in;
        rd_kafka_broker_t *rkb = NULL;
        rd_kafka_resp_err_t err;
        rd_list_t *nodeids = NULL;
        char errstr[512];

        /* ADMIN_FANOUT handled by fanout_worker() */
        rd_assert((rko->rko_type & ~RD_KAFKA_OP_FLAGMASK) !=
                  RD_KAFKA_OP_ADMIN_FANOUT);

        if (rd_kafka_terminating(rk)) {
                rd_kafka_dbg(
                    rk, ADMIN, name,
                    "%s worker called in state %s: "
                    "handle is terminating: %s",
                    name,
                    rd_kafka_admin_state_desc[rko->rko_u.admin_request.state],
                    rd_kafka_err2str(rko->rko_err));
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__DESTROY,
                                           "Handle is terminating: %s",
                                           rd_kafka_err2str(rko->rko_err));
                goto destroy;
        }

        if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY) {
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__DESTROY,
                                           "Destroyed");
                goto destroy; /* rko being destroyed (silent) */
        }

        rd_kafka_dbg(rk, ADMIN, name, "%s worker called in state %s: %s", name,
                     rd_kafka_admin_state_desc[rko->rko_u.admin_request.state],
                     rd_kafka_err2str(rko->rko_err));

        rd_assert(thrd_is_current(rko->rko_rk->rk_thread));

        /* Check for errors raised asynchronously (e.g., by timer) */
        if (rko->rko_err) {
                rd_kafka_admin_result_fail(
                    rko, rko->rko_err, "Failed while %s: %s",
                    rd_kafka_admin_state_desc[rko->rko_u.admin_request.state],
                    rd_kafka_err2str(rko->rko_err));
                goto destroy;
        }

        /* Check for timeout */
        timeout_in = rd_kafka_admin_timeout_remains_us(rko);
        if (timeout_in <= 0) {
                rd_kafka_admin_result_fail(
                    rko, RD_KAFKA_RESP_ERR__TIMED_OUT, "Timed out %s",
                    rd_kafka_admin_state_desc[rko->rko_u.admin_request.state]);
                goto destroy;
        }

redo:
        switch (rko->rko_u.admin_request.state) {
        case RD_KAFKA_ADMIN_STATE_INIT: {
                int32_t broker_id;

                /* First call. */

                /* Set up timeout timer. */
                rd_kafka_enq_once_add_source(rko->rko_u.admin_request.eonce,
                                             "timeout timer");
                rd_kafka_timer_start_oneshot(
                    &rk->rk_timers, &rko->rko_u.admin_request.tmr, rd_true,
                    timeout_in, rd_kafka_admin_eonce_timeout_cb,
                    rko->rko_u.admin_request.eonce);

                /* Use explicitly specified broker_id, if available. */
                broker_id = (int32_t)rd_kafka_confval_get_int(
                    &rko->rko_u.admin_request.options.broker);

                if (broker_id != -1) {
                        rd_kafka_dbg(rk, ADMIN, name,
                                     "%s using explicitly "
                                     "set broker id %" PRId32
                                     " rather than %" PRId32,
                                     name, broker_id,
                                     rko->rko_u.admin_request.broker_id);
                        rko->rko_u.admin_request.broker_id = broker_id;
                } else {
                        /* Default to controller */
                        broker_id = RD_KAFKA_ADMIN_TARGET_CONTROLLER;
                }

                /* Resolve target broker(s) */
                switch (rko->rko_u.admin_request.broker_id) {
                case RD_KAFKA_ADMIN_TARGET_CONTROLLER:
                        /* Controller */
                        rko->rko_u.admin_request.state =
                            RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER;
                        goto redo; /* Trigger next state immediately */

                case RD_KAFKA_ADMIN_TARGET_COORDINATOR:
                        /* Group (or other) coordinator */
                        rko->rko_u.admin_request.state =
                            RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE;
                        rd_kafka_enq_once_add_source(
                            rko->rko_u.admin_request.eonce,
                            "coordinator request");
                        rd_kafka_coord_req(
                            rk, rko->rko_u.admin_request.coordtype,
                            rko->rko_u.admin_request.coordkey,
                            rd_kafka_admin_coord_request, NULL, 0 /* no delay*/,
                            rd_kafka_admin_timeout_remains(rko),
                            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                            rd_kafka_admin_coord_response_parse,
                            rko->rko_u.admin_request.eonce);
                        /* Wait asynchronously for broker response, which will
                         * trigger the eonce and worker to be called again. */
                        return RD_KAFKA_OP_RES_KEEP;
                case RD_KAFKA_ADMIN_TARGET_ALL:
                        /* All brokers */
                        rko->rko_u.admin_request.state =
                            RD_KAFKA_ADMIN_STATE_WAIT_BROKER_LIST;
                        goto redo; /* Trigger next state immediately */

                case RD_KAFKA_ADMIN_TARGET_FANOUT:
                        /* Shouldn't come here, fanouts are handled by
                         * fanout_worker() */
                        RD_NOTREACHED();
                        return RD_KAFKA_OP_RES_KEEP;

                default:
                        /* Specific broker */
                        rd_assert(rko->rko_u.admin_request.broker_id >= 0);
                        rko->rko_u.admin_request.state =
                            RD_KAFKA_ADMIN_STATE_WAIT_BROKER;
                        goto redo; /* Trigger next state immediately */
                }
        }


        case RD_KAFKA_ADMIN_STATE_WAIT_BROKER:
                /* Broker lookup */
                if (!(rkb = rd_kafka_admin_common_get_broker(
                          rk, rko, rko->rko_u.admin_request.broker_id))) {
                        /* Still waiting for broker to become available */
                        return RD_KAFKA_OP_RES_KEEP;
                }

                rko->rko_u.admin_request.state =
                    RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST;
                goto redo;

        case RD_KAFKA_ADMIN_STATE_WAIT_CONTROLLER:
                if (!(rkb = rd_kafka_admin_common_get_controller(rk, rko))) {
                        /* Still waiting for controller to become available. */
                        return RD_KAFKA_OP_RES_KEEP;
                }

                rko->rko_u.admin_request.state =
                    RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST;
                goto redo;

        case RD_KAFKA_ADMIN_STATE_WAIT_BROKER_LIST:
                /* Wait for a valid list of brokers to be available. */
                if (!(nodeids =
                          rd_kafka_admin_common_brokers_get_nodeids(rk, rko))) {
                        /* Still waiting for brokers to become available. */
                        return RD_KAFKA_OP_RES_KEEP;
                }

                rd_kafka_admin_fanout_op_distribute(rk, rko, nodeids);
                rd_list_destroy(nodeids);
                rko->rko_u.admin_request.state =
                    RD_KAFKA_ADMIN_STATE_WAIT_FANOUTS;
                goto redo;

        case RD_KAFKA_ADMIN_STATE_WAIT_FANOUTS:
                /* This op can be destroyed, as a new fanout op has been
                 * sent, and the response will be enqueued there. */
                goto destroy;

        case RD_KAFKA_ADMIN_STATE_CONSTRUCT_REQUEST:
                /* Got broker, send protocol request. */

                /* Make sure we're called from a 'goto redo' where
                 * the rkb was set. */
                rd_assert(rkb);

                /* Still need to use the eonce since this worker may
                 * time out while waiting for response from broker, in which
                 * case the broker response will hit an empty eonce (ok). */
                rd_kafka_enq_once_add_source(rko->rko_u.admin_request.eonce,
                                             "send");

                /* Send request (async) */
                err = rko->rko_u.admin_request.cbs->request(
                    rkb, &rko->rko_u.admin_request.args,
                    &rko->rko_u.admin_request.options, errstr, sizeof(errstr),
                    RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                    rd_kafka_admin_handle_response,
                    rko->rko_u.admin_request.eonce);

                /* Loose broker refcount from get_broker(), get_controller() */
                rd_kafka_broker_destroy(rkb);

                if (err) {
                        rd_kafka_enq_once_del_source(
                            rko->rko_u.admin_request.eonce, "send");
                        rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                        goto destroy;
                }

                rko->rko_u.admin_request.state =
                    RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE;

                /* Wait asynchronously for broker response, which will
                 * trigger the eonce and worker to be called again. */
                return RD_KAFKA_OP_RES_KEEP;


        case RD_KAFKA_ADMIN_STATE_WAIT_RESPONSE:
                rd_kafka_admin_response_parse(rko);
                goto destroy;
        }

        return RD_KAFKA_OP_RES_KEEP;

destroy:
        rd_kafka_admin_common_worker_destroy(rk, rko,
                                             rd_false /*don't destroy*/);
        return RD_KAFKA_OP_RES_HANDLED; /* trigger's op_destroy() */
}

/**
 * @brief Create a new admin_fanout op of type \p req_type and sets up the
 *        generic (type independent files).
 *
 *        The caller shall then populate the \c admin_fanout.requests list,
 *        initialize the \c admin_fanout.responses list,
 *        set the initial \c admin_fanout.outstanding value,
 *        and enqueue the op on rk_ops for further processing work.
 *
 * @param cbs Callbacks, must reside in .data segment.
 * @param options Optional options, may be NULL to use defaults.
 * @param rkq is the application reply queue.
 *
 * @locks none
 * @locality application thread
 */
static rd_kafka_op_t *
rd_kafka_admin_fanout_op_new(rd_kafka_t *rk,
                             rd_kafka_op_type_t req_type,
                             rd_kafka_event_type_t reply_event_type,
                             const struct rd_kafka_admin_fanout_worker_cbs *cbs,
                             const rd_kafka_AdminOptions_t *options,
                             rd_kafka_q_t *rkq) {
        rd_kafka_op_t *rko;

        rd_assert(rk);
        rd_assert(rkq);
        rd_assert(cbs);

        rko         = rd_kafka_op_new(RD_KAFKA_OP_ADMIN_FANOUT);
        rko->rko_rk = rk;

        rko->rko_u.admin_request.reply_event_type = reply_event_type;

        rko->rko_u.admin_request.fanout.cbs =
            (struct rd_kafka_admin_fanout_worker_cbs *)cbs;

        /* Make a copy of the options */
        if (options)
                rd_kafka_AdminOptions_copy_to(&rko->rko_u.admin_request.options,
                                              options);
        else
                rd_kafka_AdminOptions_init(rk,
                                           &rko->rko_u.admin_request.options);

        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_FANOUT;

        /* Calculate absolute timeout */
        rko->rko_u.admin_request.abs_timeout =
            rd_timeout_init(rd_kafka_confval_get_int(
                &rko->rko_u.admin_request.options.request_timeout));

        /* Set up replyq */
        rd_kafka_set_replyq(&rko->rko_u.admin_request.replyq, rkq, 0);

        rko->rko_u.admin_request.state = RD_KAFKA_ADMIN_STATE_WAIT_FANOUTS;

        rko->rko_u.admin_request.fanout.reqtype = req_type;

        return rko;
}

/**
 * @brief Duplicate the fanout operation for each nodeid passed and
 *        enqueue each new operation. Use the same fanout_parent as
 *        the passed \p rko.
 *
 * @param rk Client instance.
 * @param rko Operation to distribute to each broker.
 * @param nodeids List of int32_t with the broker nodeids.
 * @param rkq
 * @return rd_kafka_op_t*
 */
static void rd_kafka_admin_fanout_op_distribute(rd_kafka_t *rk,
                                                rd_kafka_op_t *rko,
                                                rd_list_t *nodeids) {
        int i, nodeids_cnt, timeout_remains;
        rd_kafka_op_t *rko_fanout;
        rd_kafka_AdminOptions_t *options = &rko->rko_u.admin_request.options;
        timeout_remains                  = rd_kafka_admin_timeout_remains(rko);
        rd_kafka_AdminOptions_set_request_timeout(options, timeout_remains,
                                                  NULL, 0);

        nodeids_cnt = rd_list_cnt(nodeids);
        rko_fanout  = rko->rko_u.admin_request.fanout_parent;
        rko_fanout->rko_u.admin_request.fanout.outstanding = (int)nodeids_cnt;
        rko->rko_u.admin_request.fanout_parent             = NULL;

        /* Create individual request ops for each node */
        for (i = 0; i < nodeids_cnt; i++) {
                rd_kafka_op_t *rko_dup = rd_kafka_admin_request_op_new(
                    rk, rko->rko_type,
                    rko->rko_u.admin_request.reply_event_type,
                    rko->rko_u.admin_request.cbs, options, rk->rk_ops);

                rko_dup->rko_u.admin_request.fanout_parent = rko_fanout;
                rko_dup->rko_u.admin_request.broker_id =
                    rd_list_get_int32(nodeids, i);

                rd_list_init_copy(&rko_dup->rko_u.admin_request.args,
                                  &rko->rko_u.admin_request.args);
                rd_list_copy_to(
                    &rko_dup->rko_u.admin_request.args,
                    &rko->rko_u.admin_request.args,
                    rko_fanout->rko_u.admin_request.fanout.cbs->copy_arg, NULL);

                rd_kafka_q_enq(rk->rk_ops, rko_dup);
        }
}


/**
 * @brief Common fanout worker state machine handling regardless of request type
 *
 * @param rko Result of a fanned out operation, e.g., DELETERECORDS result.
 *
 * Tasks:
 *  - Checks for and responds to client termination
 *  - Polls for fanned out responses
 *  - Calls the partial response callback
 *  - Calls the merge responses callback upon receipt of all partial responses
 *  - Destruction of rko
 *
 * rko->rko_err may be one of:
 * RD_KAFKA_RESP_ERR_NO_ERROR, or
 * RD_KAFKA_RESP_ERR__DESTROY for queue destruction cleanup.
 *
 * @returns a hint to the op code whether the rko should be destroyed or not.
 */
static rd_kafka_op_res_t rd_kafka_admin_fanout_worker(rd_kafka_t *rk,
                                                      rd_kafka_q_t *rkq,
                                                      rd_kafka_op_t *rko) {
        rd_kafka_op_t *rko_fanout = rko->rko_u.admin_result.fanout_parent;
        const char *name =
            rd_kafka_op2str(rko_fanout->rko_u.admin_request.fanout.reqtype);
        rd_kafka_op_t *rko_result;

        RD_KAFKA_OP_TYPE_ASSERT(rko, RD_KAFKA_OP_ADMIN_RESULT);
        RD_KAFKA_OP_TYPE_ASSERT(rko_fanout, RD_KAFKA_OP_ADMIN_FANOUT);

        rd_assert(rko_fanout->rko_u.admin_request.fanout.outstanding > 0);
        rko_fanout->rko_u.admin_request.fanout.outstanding--;

        rko->rko_u.admin_result.fanout_parent = NULL;

        if (rd_kafka_terminating(rk)) {
                rd_kafka_dbg(rk, ADMIN, name,
                             "%s fanout worker called for fanned out op %s: "
                             "handle is terminating: %s",
                             name, rd_kafka_op2str(rko->rko_type),
                             rd_kafka_err2str(rko_fanout->rko_err));
                if (!rko->rko_err)
                        rko->rko_err = RD_KAFKA_RESP_ERR__DESTROY;
        }

        rd_kafka_dbg(rk, ADMIN, name,
                     "%s fanout worker called for %s with %d request(s) "
                     "outstanding: %s",
                     name, rd_kafka_op2str(rko->rko_type),
                     rko_fanout->rko_u.admin_request.fanout.outstanding,
                     rd_kafka_err2str(rko_fanout->rko_err));

        /* Add partial response to rko_fanout's result list. */
        rko_fanout->rko_u.admin_request.fanout.cbs->partial_response(rko_fanout,
                                                                     rko);

        if (rko_fanout->rko_u.admin_request.fanout.outstanding > 0)
                /* Wait for outstanding requests to finish */
                return RD_KAFKA_OP_RES_HANDLED;

        rko_result = rd_kafka_admin_result_new(rko_fanout);
        rd_list_init_copy(&rko_result->rko_u.admin_result.results,
                          &rko_fanout->rko_u.admin_request.fanout.results);
        rd_list_copy_to(&rko_result->rko_u.admin_result.results,
                        &rko_fanout->rko_u.admin_request.fanout.results,
                        rko_fanout->rko_u.admin_request.fanout.cbs->copy_result,
                        NULL);

        /* Enqueue result on application queue, we're done. */
        rd_kafka_admin_result_enq(rko_fanout, rko_result);

        /* FALLTHRU */
        if (rko_fanout->rko_u.admin_request.fanout.outstanding == 0)
                rd_kafka_op_destroy(rko_fanout);

        return RD_KAFKA_OP_RES_HANDLED; /* trigger's op_destroy(rko) */
}

/**
 * @brief Create a new operation that targets all the brokers.
 *        The operation consists of a fanout parent that is reused and
 *        fanout operation that is duplicated for each broker found.
 *
 * @param rk Client instance-
 * @param optype Operation type.
 * @param reply_event_type Reply event type.
 * @param cbs Fanned out op callbacks.
 * @param fanout_cbs Fanout parent out op callbacks.
 * @param result_free Callback for freeing the result list.
 * @param options Operation options.
 * @param rkq Result queue.
 * @return The newly created op targeting all the brokers.
 *
 * @sa Use rd_kafka_op_destroy() to release it.
 */
static rd_kafka_op_t *rd_kafka_admin_request_op_target_all_new(
    rd_kafka_t *rk,
    rd_kafka_op_type_t optype,
    rd_kafka_event_type_t reply_event_type,
    const struct rd_kafka_admin_worker_cbs *cbs,
    const struct rd_kafka_admin_fanout_worker_cbs *fanout_cbs,
    void (*result_free)(void *),
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_q_t *rkq) {
        rd_kafka_op_t *rko, *rko_fanout;

        rko_fanout = rd_kafka_admin_fanout_op_new(rk, optype, reply_event_type,
                                                  fanout_cbs, options, rkq);

        rko = rd_kafka_admin_request_op_new(rk, optype, reply_event_type, cbs,
                                            options, rk->rk_ops);

        rko_fanout->rko_u.admin_request.fanout.outstanding = 1;
        rko->rko_u.admin_request.fanout_parent             = rko_fanout;
        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_ALL;

        rd_list_init(&rko_fanout->rko_u.admin_request.fanout.results, (int)1,
                     result_free);

        return rko;
}


/**
 * @brief Construct MetadataRequest for use with AdminAPI (does not send).
 *        Common for DescribeTopics and DescribeCluster.
 *
 * @sa rd_kafka_MetadataRequest_resp_cb.
 */
static rd_kafka_resp_err_t
rd_kafka_admin_MetadataRequest(rd_kafka_broker_t *rkb,
                               const rd_list_t *topics,
                               const char *reason,
                               rd_bool_t include_cluster_authorized_operations,
                               rd_bool_t include_topic_authorized_operations,
                               rd_bool_t force_racks,
                               rd_kafka_resp_cb_t *resp_cb,
                               rd_kafka_replyq_t replyq,
                               void *opaque) {
        return rd_kafka_MetadataRequest_resp_cb(
            rkb, topics, NULL, reason,
            rd_false /* No admin operation requires topic creation. */,
            include_cluster_authorized_operations,
            include_topic_authorized_operations,
            rd_false /* No admin operation should update cgrp. */,
            -1 /* No subscription version is used */, force_racks, resp_cb,
            replyq,
            rd_true /* Admin operation metadata requests are always forced. */,
            opaque);
}

/**@}*/


/**
 * @name Generic AdminOptions
 * @{
 *
 *
 */

rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_request_timeout(rd_kafka_AdminOptions_t *options,
                                          int timeout_ms,
                                          char *errstr,
                                          size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->request_timeout,
                                         RD_KAFKA_CONFVAL_INT, &timeout_ms,
                                         errstr, errstr_size);
}


rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_operation_timeout(rd_kafka_AdminOptions_t *options,
                                            int timeout_ms,
                                            char *errstr,
                                            size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->operation_timeout,
                                         RD_KAFKA_CONFVAL_INT, &timeout_ms,
                                         errstr, errstr_size);
}


rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_validate_only(rd_kafka_AdminOptions_t *options,
                                        int true_or_false,
                                        char *errstr,
                                        size_t errstr_size) {
        return rd_kafka_confval_set_type(&options->validate_only,
                                         RD_KAFKA_CONFVAL_INT, &true_or_false,
                                         errstr, errstr_size);
}

rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_broker(rd_kafka_AdminOptions_t *options,
                                 int32_t broker_id,
                                 char *errstr,
                                 size_t errstr_size) {
        int ibroker_id = (int)broker_id;

        return rd_kafka_confval_set_type(&options->broker, RD_KAFKA_CONFVAL_INT,
                                         &ibroker_id, errstr, errstr_size);
}

rd_kafka_error_t *
rd_kafka_AdminOptions_set_isolation_level(rd_kafka_AdminOptions_t *options,
                                          rd_kafka_IsolationLevel_t value) {
        char errstr[512];
        rd_kafka_resp_err_t err = rd_kafka_confval_set_type(
            &options->isolation_level, RD_KAFKA_CONFVAL_INT, &value, errstr,
            sizeof(errstr));
        return !err ? NULL : rd_kafka_error_new(err, "%s", errstr);
}

rd_kafka_error_t *rd_kafka_AdminOptions_set_require_stable_offsets(
    rd_kafka_AdminOptions_t *options,
    int true_or_false) {
        char errstr[512];
        rd_kafka_resp_err_t err = rd_kafka_confval_set_type(
            &options->require_stable_offsets, RD_KAFKA_CONFVAL_INT,
            &true_or_false, errstr, sizeof(errstr));
        return !err ? NULL : rd_kafka_error_new(err, "%s", errstr);
}

rd_kafka_error_t *rd_kafka_AdminOptions_set_include_authorized_operations(
    rd_kafka_AdminOptions_t *options,
    int true_or_false) {
        char errstr[512];
        rd_kafka_resp_err_t err = rd_kafka_confval_set_type(
            &options->include_authorized_operations, RD_KAFKA_CONFVAL_INT,
            &true_or_false, errstr, sizeof(errstr));
        return !err ? NULL : rd_kafka_error_new(err, "%s", errstr);
}

rd_kafka_error_t *rd_kafka_AdminOptions_set_match_consumer_group_states(
    rd_kafka_AdminOptions_t *options,
    const rd_kafka_consumer_group_state_t *consumer_group_states,
    size_t consumer_group_states_cnt) {
        size_t i;
        char errstr[512];
        rd_kafka_resp_err_t err;
        rd_list_t *states_list = rd_list_new(0, NULL);
        rd_list_init_int32(states_list, consumer_group_states_cnt);
        uint64_t states_bitmask = 0;

        if (RD_KAFKA_CONSUMER_GROUP_STATE__CNT >= 64) {
                rd_assert("BUG: cannot handle states with a bitmask anymore");
        }

        for (i = 0; i < consumer_group_states_cnt; i++) {
                uint64_t state_bit;
                rd_kafka_consumer_group_state_t state =
                    consumer_group_states[i];

                if (state < 0 || state >= RD_KAFKA_CONSUMER_GROUP_STATE__CNT) {
                        rd_list_destroy(states_list);
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Invalid group state value");
                }

                state_bit = 1 << state;
                if (states_bitmask & state_bit) {
                        rd_list_destroy(states_list);
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate states not allowed");
                } else {
                        states_bitmask = states_bitmask | state_bit;
                        rd_list_set_int32(states_list, (int32_t)i, state);
                }
        }
        err = rd_kafka_confval_set_type(&options->match_consumer_group_states,
                                        RD_KAFKA_CONFVAL_PTR, states_list,
                                        errstr, sizeof(errstr));
        if (err) {
                rd_list_destroy(states_list);
        }
        return !err ? NULL : rd_kafka_error_new(err, "%s", errstr);
}

rd_kafka_error_t *rd_kafka_AdminOptions_set_match_consumer_group_types(
    rd_kafka_AdminOptions_t *options,
    const rd_kafka_consumer_group_type_t *consumer_group_types,
    size_t consumer_group_types_cnt) {
        size_t i;
        char errstr[512];
        rd_kafka_resp_err_t err;
        rd_list_t *types_list  = rd_list_new(0, NULL);
        uint64_t types_bitmask = 0;

        rd_list_init_int32(types_list, consumer_group_types_cnt);

        if (RD_KAFKA_CONSUMER_GROUP_TYPE__CNT >= 64) {
                rd_assert("BUG: cannot handle types with a bitmask anymore");
        }

        for (i = 0; i < consumer_group_types_cnt; i++) {
                uint64_t type_bit;
                rd_kafka_consumer_group_type_t type = consumer_group_types[i];

                if (type < RD_KAFKA_CONSUMER_GROUP_TYPE_UNKNOWN ||
                    type >= RD_KAFKA_CONSUMER_GROUP_TYPE__CNT) {
                        rd_list_destroy(types_list);
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Only a valid type is allowed");
                } else if (type == RD_KAFKA_CONSUMER_GROUP_TYPE_UNKNOWN) {
                        rd_list_destroy(types_list);
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "UNKNOWN type is not allowed");
                }

                type_bit = 1 << type;
                if (types_bitmask & type_bit) {
                        rd_list_destroy(types_list);
                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate types not allowed");
                } else {
                        types_bitmask = types_bitmask | type_bit;
                        rd_list_set_int32(types_list, (int32_t)i, type);
                }
        }

        err = rd_kafka_confval_set_type(&options->match_consumer_group_types,
                                        RD_KAFKA_CONFVAL_PTR, types_list,
                                        errstr, sizeof(errstr));
        if (err) {
                rd_list_destroy(types_list);
        }
        return !err ? NULL : rd_kafka_error_new(err, "%s", errstr);
}

void rd_kafka_AdminOptions_set_opaque(rd_kafka_AdminOptions_t *options,
                                      void *opaque) {
        rd_kafka_confval_set_type(&options->opaque, RD_KAFKA_CONFVAL_PTR,
                                  opaque, NULL, 0);
}


/**
 * @brief Initialize and set up defaults for AdminOptions
 */
static void rd_kafka_AdminOptions_init(rd_kafka_t *rk,
                                       rd_kafka_AdminOptions_t *options) {
        rd_kafka_confval_init_int(&options->request_timeout, "request_timeout",
                                  0, 3600 * 1000,
                                  rk->rk_conf.admin.request_timeout_ms);

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_CREATETOPICS ||
            options->for_api == RD_KAFKA_ADMIN_OP_DELETETOPICS ||
            options->for_api == RD_KAFKA_ADMIN_OP_CREATEPARTITIONS ||
            options->for_api == RD_KAFKA_ADMIN_OP_DELETERECORDS ||
            options->for_api == RD_KAFKA_ADMIN_OP_LISTOFFSETS ||
            options->for_api == RD_KAFKA_ADMIN_OP_ELECTLEADERS)
                rd_kafka_confval_init_int(&options->operation_timeout,
                                          "operation_timeout", -1, 3600 * 1000,
                                          rk->rk_conf.admin.request_timeout_ms);
        else
                rd_kafka_confval_disable(&options->operation_timeout,
                                         "operation_timeout");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_CREATETOPICS ||
            options->for_api == RD_KAFKA_ADMIN_OP_CREATEPARTITIONS ||
            options->for_api == RD_KAFKA_ADMIN_OP_ALTERCONFIGS ||
            options->for_api == RD_KAFKA_ADMIN_OP_INCREMENTALALTERCONFIGS)
                rd_kafka_confval_init_int(&options->validate_only,
                                          "validate_only", 0, 1, 0);
        else
                rd_kafka_confval_disable(&options->validate_only,
                                         "validate_only");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_LISTCONSUMERGROUPOFFSETS)
                rd_kafka_confval_init_int(&options->require_stable_offsets,
                                          "require_stable_offsets", 0, 1, 0);
        else
                rd_kafka_confval_disable(&options->require_stable_offsets,
                                         "require_stable_offsets");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_DESCRIBECONSUMERGROUPS ||
            options->for_api == RD_KAFKA_ADMIN_OP_DESCRIBECLUSTER ||
            options->for_api == RD_KAFKA_ADMIN_OP_DESCRIBETOPICS)
                rd_kafka_confval_init_int(
                    &options->include_authorized_operations,
                    "include_authorized_operations", 0, 1, 0);
        else
                rd_kafka_confval_disable(
                    &options->include_authorized_operations,
                    "include_authorized_operations");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_LISTCONSUMERGROUPS)
                rd_kafka_confval_init_ptr(&options->match_consumer_group_states,
                                          "match_consumer_group_states");
        else
                rd_kafka_confval_disable(&options->match_consumer_group_states,
                                         "match_consumer_group_states");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_LISTCONSUMERGROUPS)
                rd_kafka_confval_init_ptr(&options->match_consumer_group_types,
                                          "match_consumer_group_types");
        else
                rd_kafka_confval_disable(&options->match_consumer_group_types,
                                         "match_consumer_group_types");

        if (options->for_api == RD_KAFKA_ADMIN_OP_ANY ||
            options->for_api == RD_KAFKA_ADMIN_OP_LISTOFFSETS)
                rd_kafka_confval_init_int(&options->isolation_level,
                                          "isolation_level", 0, 1, 0);
        else
                rd_kafka_confval_disable(&options->isolation_level,
                                         "isolation_level");

        rd_kafka_confval_init_int(&options->broker, "broker", 0, INT32_MAX, -1);
        rd_kafka_confval_init_ptr(&options->opaque, "opaque");
}

/**
 * @brief Copy contents of \p src to \p dst.
 *        Deep copy every pointer confval.
 *
 * @param dst The destination AdminOptions.
 * @param src The source AdminOptions.
 */
static void rd_kafka_AdminOptions_copy_to(rd_kafka_AdminOptions_t *dst,
                                          const rd_kafka_AdminOptions_t *src) {
        *dst = *src;
        if (src->match_consumer_group_states.u.PTR) {
                char errstr[512];
                rd_list_t *states_list_copy = rd_list_copy_preallocated(
                    src->match_consumer_group_states.u.PTR, NULL);

                rd_kafka_resp_err_t err = rd_kafka_confval_set_type(
                    &dst->match_consumer_group_states, RD_KAFKA_CONFVAL_PTR,
                    states_list_copy, errstr, sizeof(errstr));
                rd_assert(!err);
        }
        if (src->match_consumer_group_types.u.PTR) {
                char errstr[512];
                rd_list_t *types_list_copy = rd_list_copy_preallocated(
                    src->match_consumer_group_types.u.PTR, NULL);

                rd_kafka_resp_err_t err = rd_kafka_confval_set_type(
                    &dst->match_consumer_group_types, RD_KAFKA_CONFVAL_PTR,
                    types_list_copy, errstr, sizeof(errstr));
                rd_assert(!err);
        }
}


rd_kafka_AdminOptions_t *
rd_kafka_AdminOptions_new(rd_kafka_t *rk, rd_kafka_admin_op_t for_api) {
        rd_kafka_AdminOptions_t *options;

        if ((int)for_api < 0 || for_api >= RD_KAFKA_ADMIN_OP__CNT)
                return NULL;

        options = rd_calloc(1, sizeof(*options));

        options->for_api = for_api;

        rd_kafka_AdminOptions_init(rk, options);

        return options;
}

void rd_kafka_AdminOptions_destroy(rd_kafka_AdminOptions_t *options) {
        if (options->match_consumer_group_states.u.PTR) {
                rd_list_destroy(options->match_consumer_group_states.u.PTR);
        }
        if (options->match_consumer_group_types.u.PTR) {
                rd_list_destroy(options->match_consumer_group_types.u.PTR);
        }
        rd_free(options);
}

/**@}*/



/**
 * @name CreateTopics
 * @{
 *
 *
 *
 */



rd_kafka_NewTopic_t *rd_kafka_NewTopic_new(const char *topic,
                                           int num_partitions,
                                           int replication_factor,
                                           char *errstr,
                                           size_t errstr_size) {
        rd_kafka_NewTopic_t *new_topic;

        if (!topic) {
                rd_snprintf(errstr, errstr_size, "Invalid topic name");
                return NULL;
        }

        if (num_partitions < -1 || num_partitions > RD_KAFKAP_PARTITIONS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "num_partitions out of "
                            "expected range %d..%d or -1 for broker default",
                            1, RD_KAFKAP_PARTITIONS_MAX);
                return NULL;
        }

        if (replication_factor < -1 ||
            replication_factor > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "replication_factor out of expected range %d..%d",
                            -1, RD_KAFKAP_BROKERS_MAX);
                return NULL;
        }

        new_topic                     = rd_calloc(1, sizeof(*new_topic));
        new_topic->topic              = rd_strdup(topic);
        new_topic->num_partitions     = num_partitions;
        new_topic->replication_factor = replication_factor;

        /* List of int32 lists */
        rd_list_init(&new_topic->replicas, 0, rd_list_destroy_free);
        rd_list_prealloc_elems(&new_topic->replicas, 0,
                               num_partitions == -1 ? 0 : num_partitions,
                               0 /*nozero*/);

        /* List of ConfigEntrys */
        rd_list_init(&new_topic->config, 0, rd_kafka_ConfigEntry_free);

        return new_topic;
}


/**
 * @brief Topic name comparator for NewTopic_t
 */
static int rd_kafka_NewTopic_cmp(const void *_a, const void *_b) {
        const rd_kafka_NewTopic_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}



/**
 * @brief Allocate a new NewTopic and make a copy of \p src
 */
static rd_kafka_NewTopic_t *
rd_kafka_NewTopic_copy(const rd_kafka_NewTopic_t *src) {
        rd_kafka_NewTopic_t *dst;

        dst = rd_kafka_NewTopic_new(src->topic, src->num_partitions,
                                    src->replication_factor, NULL, 0);
        rd_assert(dst);

        rd_list_destroy(&dst->replicas); /* created in .._new() */
        rd_list_init_copy(&dst->replicas, &src->replicas);
        rd_list_copy_to(&dst->replicas, &src->replicas,
                        rd_list_copy_preallocated, NULL);

        rd_list_init_copy(&dst->config, &src->config);
        rd_list_copy_to(&dst->config, &src->config,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}

void rd_kafka_NewTopic_destroy(rd_kafka_NewTopic_t *new_topic) {
        rd_list_destroy(&new_topic->replicas);
        rd_list_destroy(&new_topic->config);
        rd_free(new_topic->topic);
        rd_free(new_topic);
}

static void rd_kafka_NewTopic_free(void *ptr) {
        rd_kafka_NewTopic_destroy(ptr);
}

void rd_kafka_NewTopic_destroy_array(rd_kafka_NewTopic_t **new_topics,
                                     size_t new_topic_cnt) {
        size_t i;
        for (i = 0; i < new_topic_cnt; i++)
                rd_kafka_NewTopic_destroy(new_topics[i]);
}


rd_kafka_resp_err_t
rd_kafka_NewTopic_set_replica_assignment(rd_kafka_NewTopic_t *new_topic,
                                         int32_t partition,
                                         int32_t *broker_ids,
                                         size_t broker_id_cnt,
                                         char *errstr,
                                         size_t errstr_size) {
        rd_list_t *rl;
        int i;

        if (new_topic->replication_factor != -1) {
                rd_snprintf(errstr, errstr_size,
                            "Specifying a replication factor and "
                            "a replica assignment are mutually exclusive");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        } else if (new_topic->num_partitions == -1) {
                rd_snprintf(errstr, errstr_size,
                            "Specifying a default partition count and a "
                            "replica assignment are mutually exclusive");
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        /* Replica partitions must be added consecutively starting from 0. */
        if (partition != rd_list_cnt(&new_topic->replicas)) {
                rd_snprintf(errstr, errstr_size,
                            "Partitions must be added in order, "
                            "starting at 0: expecting partition %d, "
                            "not %" PRId32,
                            rd_list_cnt(&new_topic->replicas), partition);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        if (broker_id_cnt > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "Too many brokers specified "
                            "(RD_KAFKAP_BROKERS_MAX=%d)",
                            RD_KAFKAP_BROKERS_MAX);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }


        rl = rd_list_init_int32(rd_list_new(0, NULL), (int)broker_id_cnt);

        for (i = 0; i < (int)broker_id_cnt; i++)
                rd_list_set_int32(rl, i, broker_ids[i]);

        rd_list_add(&new_topic->replicas, rl);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Generic constructor of ConfigEntry which is also added to \p rl
 */
static rd_kafka_resp_err_t
rd_kafka_admin_add_config0(rd_list_t *rl, const char *name, const char *value) {
        rd_kafka_ConfigEntry_t *entry;

        if (!name)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        entry     = rd_calloc(1, sizeof(*entry));
        entry->kv = rd_strtup_new(name, value);

        rd_list_add(rl, entry);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Generic constructor of ConfigEntry for Incremental Alter Operations
 * which is also added to \p rl
 */
static rd_kafka_error_t *
rd_kafka_admin_incremental_add_config0(rd_list_t *rl,
                                       const char *name,
                                       rd_kafka_AlterConfigOpType_t op_type,
                                       const char *value) {
        rd_kafka_ConfigEntry_t *entry;

        if (!name) {
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "Config name is required");
        }

        entry            = rd_calloc(1, sizeof(*entry));
        entry->kv        = rd_strtup_new(name, value);
        entry->a.op_type = op_type;

        rd_list_add(rl, entry);

        return NULL;
}


rd_kafka_resp_err_t rd_kafka_NewTopic_set_config(rd_kafka_NewTopic_t *new_topic,
                                                 const char *name,
                                                 const char *value) {
        return rd_kafka_admin_add_config0(&new_topic->config, name, value);
}



/**
 * @brief Parse CreateTopicsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_CreateTopicsResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t topic_cnt;
        int i;

        if (rd_kafka_buf_ApiVersion(reply) >= 2) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(reply, &Throttle_Time);
                rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);
        }

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " topics in response "
                    "when only %d were requested",
                    topic_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));


        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0; i < (int)topic_cnt; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                rd_kafkap_str_t error_msg = RD_KAFKAP_STR_INITIALIZER;
                char *this_errstr         = NULL;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);

                if (rd_kafka_buf_ApiVersion(reply) >= 1)
                        rd_kafka_buf_read_str(reply, &error_msg);

                /* For non-blocking CreateTopicsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.admin_request
                                                  .options.operation_timeout) <=
                        0) {
                        error_code  = RD_KAFKA_RESP_ERR_NO_ERROR;
                        this_errstr = NULL;
                }

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                this_errstr =
                                    (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&this_errstr, &error_msg);
                }

                terr = rd_kafka_topic_result_new(ktopic.str,
                                                 RD_KAFKAP_STR_LEN(&ktopic),
                                                 error_code, this_errstr);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos   = rd_list_index(&rko_result->rko_u.admin_result.args,
                                           &skel, rd_kafka_NewTopic_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned topic %.*s that was not "
                            "included in the original request",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply, "Broker returned topic %.*s multiple times",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "CreateTopics response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}


void rd_kafka_CreateTopics(rd_kafka_t *rk,
                           rd_kafka_NewTopic_t **new_topics,
                           size_t new_topic_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_CreateTopicsRequest,
            rd_kafka_CreateTopicsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_CREATETOPICS,
                                            RD_KAFKA_EVENT_CREATETOPICS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)new_topic_cnt,
                     rd_kafka_NewTopic_free);

        for (i = 0; i < new_topic_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_NewTopic_copy(new_topics[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a CreateTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **rd_kafka_CreateTopics_result_topics(
    const rd_kafka_CreateTopics_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/



/**
 * @name Delete topics
 * @{
 *
 *
 *
 *
 */

rd_kafka_DeleteTopic_t *rd_kafka_DeleteTopic_new(const char *topic) {
        size_t tsize = strlen(topic) + 1;
        rd_kafka_DeleteTopic_t *del_topic;

        /* Single allocation */
        del_topic        = rd_malloc(sizeof(*del_topic) + tsize);
        del_topic->topic = del_topic->data;
        memcpy(del_topic->topic, topic, tsize);

        return del_topic;
}

void rd_kafka_DeleteTopic_destroy(rd_kafka_DeleteTopic_t *del_topic) {
        rd_free(del_topic);
}

static void rd_kafka_DeleteTopic_free(void *ptr) {
        rd_kafka_DeleteTopic_destroy(ptr);
}


void rd_kafka_DeleteTopic_destroy_array(rd_kafka_DeleteTopic_t **del_topics,
                                        size_t del_topic_cnt) {
        size_t i;
        for (i = 0; i < del_topic_cnt; i++)
                rd_kafka_DeleteTopic_destroy(del_topics[i]);
}


/**
 * @brief Topic name comparator for DeleteTopic_t
 */
static int rd_kafka_DeleteTopic_cmp(const void *_a, const void *_b) {
        const rd_kafka_DeleteTopic_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}

/**
 * @brief Allocate a new DeleteTopic and make a copy of \p src
 */
static rd_kafka_DeleteTopic_t *
rd_kafka_DeleteTopic_copy(const rd_kafka_DeleteTopic_t *src) {
        return rd_kafka_DeleteTopic_new(src->topic);
}



/**
 * @brief Parse DeleteTopicsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DeleteTopicsResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t topic_cnt;
        int i;

        if (rd_kafka_buf_ApiVersion(reply) >= 1) {
                int32_t Throttle_Time;
                rd_kafka_buf_read_i32(reply, &Throttle_Time);
                rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);
        }

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " topics in response "
                    "when only %d were requested",
                    topic_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0; i < (int)topic_cnt; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);

                /* For non-blocking DeleteTopicsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.admin_request
                                                  .options.operation_timeout) <=
                        0) {
                        error_code = RD_KAFKA_RESP_ERR_NO_ERROR;
                }

                terr = rd_kafka_topic_result_new(
                    ktopic.str, RD_KAFKAP_STR_LEN(&ktopic), error_code,
                    error_code ? rd_kafka_err2str(error_code) : NULL);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos   = rd_list_index(&rko_result->rko_u.admin_result.args,
                                           &skel, rd_kafka_DeleteTopic_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned topic %.*s that was not "
                            "included in the original request",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply, "Broker returned topic %.*s multiple times",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DeleteTopics response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}



void rd_kafka_DeleteTopics(rd_kafka_t *rk,
                           rd_kafka_DeleteTopic_t **del_topics,
                           size_t del_topic_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DeleteTopicsRequest,
            rd_kafka_DeleteTopicsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_DELETETOPICS,
                                            RD_KAFKA_EVENT_DELETETOPICS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)del_topic_cnt,
                     rd_kafka_DeleteTopic_free);

        for (i = 0; i < del_topic_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_DeleteTopic_copy(del_topics[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a DeleteTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **rd_kafka_DeleteTopics_result_topics(
    const rd_kafka_DeleteTopics_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}



/**
 * @name Create partitions
 * @{
 *
 *
 *
 *
 */

rd_kafka_NewPartitions_t *rd_kafka_NewPartitions_new(const char *topic,
                                                     size_t new_total_cnt,
                                                     char *errstr,
                                                     size_t errstr_size) {
        size_t tsize = strlen(topic) + 1;
        rd_kafka_NewPartitions_t *newps;

        if (new_total_cnt < 1 || new_total_cnt > RD_KAFKAP_PARTITIONS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "new_total_cnt out of "
                            "expected range %d..%d",
                            1, RD_KAFKAP_PARTITIONS_MAX);
                return NULL;
        }

        /* Single allocation */
        newps            = rd_malloc(sizeof(*newps) + tsize);
        newps->total_cnt = new_total_cnt;
        newps->topic     = newps->data;
        memcpy(newps->topic, topic, tsize);

        /* List of int32 lists */
        rd_list_init(&newps->replicas, 0, rd_list_destroy_free);
        rd_list_prealloc_elems(&newps->replicas, 0, new_total_cnt,
                               0 /*nozero*/);

        return newps;
}

/**
 * @brief Topic name comparator for NewPartitions_t
 */
static int rd_kafka_NewPartitions_cmp(const void *_a, const void *_b) {
        const rd_kafka_NewPartitions_t *a = _a, *b = _b;
        return strcmp(a->topic, b->topic);
}


/**
 * @brief Allocate a new CreatePartitions and make a copy of \p src
 */
static rd_kafka_NewPartitions_t *
rd_kafka_NewPartitions_copy(const rd_kafka_NewPartitions_t *src) {
        rd_kafka_NewPartitions_t *dst;

        dst = rd_kafka_NewPartitions_new(src->topic, src->total_cnt, NULL, 0);

        rd_list_destroy(&dst->replicas); /* created in .._new() */
        rd_list_init_copy(&dst->replicas, &src->replicas);
        rd_list_copy_to(&dst->replicas, &src->replicas,
                        rd_list_copy_preallocated, NULL);

        return dst;
}

void rd_kafka_NewPartitions_destroy(rd_kafka_NewPartitions_t *newps) {
        rd_list_destroy(&newps->replicas);
        rd_free(newps);
}

static void rd_kafka_NewPartitions_free(void *ptr) {
        rd_kafka_NewPartitions_destroy(ptr);
}


void rd_kafka_NewPartitions_destroy_array(rd_kafka_NewPartitions_t **newps,
                                          size_t newps_cnt) {
        size_t i;
        for (i = 0; i < newps_cnt; i++)
                rd_kafka_NewPartitions_destroy(newps[i]);
}



rd_kafka_resp_err_t
rd_kafka_NewPartitions_set_replica_assignment(rd_kafka_NewPartitions_t *newp,
                                              int32_t new_partition_idx,
                                              int32_t *broker_ids,
                                              size_t broker_id_cnt,
                                              char *errstr,
                                              size_t errstr_size) {
        rd_list_t *rl;
        int i;

        /* Replica partitions must be added consecutively starting from 0. */
        if (new_partition_idx != rd_list_cnt(&newp->replicas)) {
                rd_snprintf(errstr, errstr_size,
                            "Partitions must be added in order, "
                            "starting at 0: expecting partition "
                            "index %d, not %" PRId32,
                            rd_list_cnt(&newp->replicas), new_partition_idx);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        if (broker_id_cnt > RD_KAFKAP_BROKERS_MAX) {
                rd_snprintf(errstr, errstr_size,
                            "Too many brokers specified "
                            "(RD_KAFKAP_BROKERS_MAX=%d)",
                            RD_KAFKAP_BROKERS_MAX);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        rl = rd_list_init_int32(rd_list_new(0, NULL), (int)broker_id_cnt);

        for (i = 0; i < (int)broker_id_cnt; i++)
                rd_list_set_int32(rl, i, broker_ids[i]);

        rd_list_add(&newp->replicas, rl);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Parse CreatePartitionsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_CreatePartitionsResponse_parse(rd_kafka_op_t *rko_req,
                                        rd_kafka_op_t **rko_resultp,
                                        rd_kafka_buf_t *reply,
                                        char *errstr,
                                        size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t topic_cnt;
        int i;
        int32_t Throttle_Time;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        /* #topics */
        rd_kafka_buf_read_i32(reply, &topic_cnt);

        if (topic_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " topics in response "
                    "when only %d were requested",
                    topic_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, topic_cnt,
                     rd_kafka_topic_result_free);

        for (i = 0; i < (int)topic_cnt; i++) {
                rd_kafkap_str_t ktopic;
                int16_t error_code;
                char *this_errstr = NULL;
                rd_kafka_topic_result_t *terr;
                rd_kafka_NewTopic_t skel;
                rd_kafkap_str_t error_msg;
                int orig_pos;

                rd_kafka_buf_read_str(reply, &ktopic);
                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);

                /* For non-blocking CreatePartitionsRequests the broker
                 * will returned REQUEST_TIMED_OUT for topics
                 * that were triggered for creation -
                 * we hide this error code from the application
                 * since the topic creation is in fact in progress. */
                if (error_code == RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT &&
                    rd_kafka_confval_get_int(&rko_req->rko_u.admin_request
                                                  .options.operation_timeout) <=
                        0) {
                        error_code = RD_KAFKA_RESP_ERR_NO_ERROR;
                }

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                this_errstr =
                                    (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&this_errstr, &error_msg);
                }

                terr = rd_kafka_topic_result_new(
                    ktopic.str, RD_KAFKAP_STR_LEN(&ktopic), error_code,
                    error_code ? this_errstr : NULL);

                /* As a convenience to the application we insert topic result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.topic = terr->topic;
                orig_pos   = rd_list_index(&rko_result->rko_u.admin_result.args,
                                           &skel, rd_kafka_NewPartitions_cmp);
                if (orig_pos == -1) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned topic %.*s that was not "
                            "included in the original request",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_topic_result_destroy(terr);
                        rd_kafka_buf_parse_fail(
                            reply, "Broker returned topic %.*s multiple times",
                            RD_KAFKAP_STR_PR(&ktopic));
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            terr);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "CreatePartitions response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}



void rd_kafka_CreatePartitions(rd_kafka_t *rk,
                               rd_kafka_NewPartitions_t **newps,
                               size_t newps_cnt,
                               const rd_kafka_AdminOptions_t *options,
                               rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_CreatePartitionsRequest,
            rd_kafka_CreatePartitionsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_CREATEPARTITIONS,
            RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)newps_cnt,
                     rd_kafka_NewPartitions_free);

        for (i = 0; i < newps_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_NewPartitions_copy(newps[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of topic results from a CreatePartitions result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_topic_result_t **rd_kafka_CreatePartitions_result_topics(
    const rd_kafka_CreatePartitions_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_topics((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/



/**
 * @name ConfigEntry
 * @{
 *
 *
 *
 */

static void rd_kafka_ConfigEntry_destroy(rd_kafka_ConfigEntry_t *entry) {
        rd_strtup_destroy(entry->kv);
        rd_list_destroy(&entry->synonyms);
        rd_free(entry);
}


static void rd_kafka_ConfigEntry_free(void *ptr) {
        rd_kafka_ConfigEntry_destroy((rd_kafka_ConfigEntry_t *)ptr);
}


/**
 * @brief Create new ConfigEntry
 *
 * @param name Config entry name
 * @param name_len Length of name, or -1 to use strlen()
 * @param value Config entry value, or NULL
 * @param value_len Length of value, or -1 to use strlen()
 */
static rd_kafka_ConfigEntry_t *rd_kafka_ConfigEntry_new0(const char *name,
                                                         size_t name_len,
                                                         const char *value,
                                                         size_t value_len) {
        rd_kafka_ConfigEntry_t *entry;

        if (!name)
                return NULL;

        entry     = rd_calloc(1, sizeof(*entry));
        entry->kv = rd_strtup_new0(name, name_len, value, value_len);

        rd_list_init(&entry->synonyms, 0, rd_kafka_ConfigEntry_free);

        entry->a.source = RD_KAFKA_CONFIG_SOURCE_UNKNOWN_CONFIG;

        return entry;
}

/**
 * @sa rd_kafka_ConfigEntry_new0
 */
static rd_kafka_ConfigEntry_t *rd_kafka_ConfigEntry_new(const char *name,
                                                        const char *value) {
        return rd_kafka_ConfigEntry_new0(name, -1, value, -1);
}



/**
 * @brief Allocate a new AlterConfigs and make a copy of \p src
 */
static rd_kafka_ConfigEntry_t *
rd_kafka_ConfigEntry_copy(const rd_kafka_ConfigEntry_t *src) {
        rd_kafka_ConfigEntry_t *dst;

        dst    = rd_kafka_ConfigEntry_new(src->kv->name, src->kv->value);
        dst->a = src->a;

        rd_list_destroy(&dst->synonyms); /* created in .._new() */
        rd_list_init_copy(&dst->synonyms, &src->synonyms);
        rd_list_copy_to(&dst->synonyms, &src->synonyms,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}

static void *rd_kafka_ConfigEntry_list_copy(const void *src, void *opaque) {
        return rd_kafka_ConfigEntry_copy((const rd_kafka_ConfigEntry_t *)src);
}


const char *rd_kafka_ConfigEntry_name(const rd_kafka_ConfigEntry_t *entry) {
        return entry->kv->name;
}

const char *rd_kafka_ConfigEntry_value(const rd_kafka_ConfigEntry_t *entry) {
        return entry->kv->value;
}

rd_kafka_ConfigSource_t
rd_kafka_ConfigEntry_source(const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.source;
}

int rd_kafka_ConfigEntry_is_read_only(const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_readonly;
}

int rd_kafka_ConfigEntry_is_default(const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_default;
}

int rd_kafka_ConfigEntry_is_sensitive(const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_sensitive;
}

int rd_kafka_ConfigEntry_is_synonym(const rd_kafka_ConfigEntry_t *entry) {
        return entry->a.is_synonym;
}

const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigEntry_synonyms(const rd_kafka_ConfigEntry_t *entry,
                              size_t *cntp) {
        *cntp = rd_list_cnt(&entry->synonyms);
        if (!*cntp)
                return NULL;
        return (const rd_kafka_ConfigEntry_t **)entry->synonyms.rl_elems;
}


/**@}*/



/**
 * @name ConfigSource
 * @{
 *
 *
 *
 */

const char *rd_kafka_ConfigSource_name(rd_kafka_ConfigSource_t confsource) {
        static const char *names[] = {
            "UNKNOWN_CONFIG",
            "DYNAMIC_TOPIC_CONFIG",
            "DYNAMIC_BROKER_CONFIG",
            "DYNAMIC_DEFAULT_BROKER_CONFIG",
            "STATIC_BROKER_CONFIG",
            "DEFAULT_CONFIG",
            "DYNAMIC_BROKER_LOGGER_CONFIG",
            "CLIENT_METRICS_CONFIG",
            "GROUP_CONFIG",
        };

        if ((unsigned int)confsource >=
            (unsigned int)RD_KAFKA_CONFIG_SOURCE__CNT)
                return "UNSUPPORTED";

        return names[confsource];
}

/**@}*/



/**
 * @name ConfigResource
 * @{
 *
 *
 *
 */

const char *rd_kafka_ResourcePatternType_name(
    rd_kafka_ResourcePatternType_t resource_pattern_type) {
        static const char *names[] = {"UNKNOWN", "ANY", "MATCH", "LITERAL",
                                      "PREFIXED"};

        if ((unsigned int)resource_pattern_type >=
            (unsigned int)RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT)
                return "UNSUPPORTED";

        return names[resource_pattern_type];
}

const char *rd_kafka_ResourceType_name(rd_kafka_ResourceType_t restype) {
        static const char *names[] = {"UNKNOWN", "ANY",    "TOPIC",
                                      "GROUP",   "BROKER", "TRANSACTIONAL_ID"};

        if ((unsigned int)restype >= (unsigned int)RD_KAFKA_RESOURCE__CNT)
                return "UNSUPPORTED";

        return names[restype];
}


rd_kafka_ConfigResourceType_t
rd_kafka_ResourceType_to_ConfigResourceType(rd_kafka_ResourceType_t restype) {
        switch (restype) {
        case RD_KAFKA_RESOURCE_TOPIC:
                return RD_KAFKA_CONFIG_RESOURCE_TOPIC;
        case RD_KAFKA_RESOURCE_BROKER:
                return RD_KAFKA_CONFIG_RESOURCE_BROKER;
        case RD_KAFKA_RESOURCE_GROUP:
                return RD_KAFKA_CONFIG_RESOURCE_GROUP;
        default:
                return RD_KAFKA_CONFIG_RESOURCE_UNKNOWN;
        }
}

rd_kafka_ResourceType_t rd_kafka_ConfigResourceType_to_ResourceType(
    rd_kafka_ConfigResourceType_t config_resource_type) {
        switch (config_resource_type) {
        case RD_KAFKA_CONFIG_RESOURCE_TOPIC:
                return RD_KAFKA_RESOURCE_TOPIC;
        case RD_KAFKA_CONFIG_RESOURCE_BROKER:
                return RD_KAFKA_RESOURCE_BROKER;
        case RD_KAFKA_CONFIG_RESOURCE_GROUP:
                return RD_KAFKA_RESOURCE_GROUP;
        default:
                return RD_KAFKA_RESOURCE_UNKNOWN;
        }
}


rd_kafka_ConfigResource_t *
rd_kafka_ConfigResource_new(rd_kafka_ResourceType_t restype,
                            const char *resname) {
        rd_kafka_ConfigResource_t *config;
        size_t namesz = resname ? strlen(resname) : 0;

        if (!namesz || (int)restype < 0)
                return NULL;

        config       = rd_calloc(1, sizeof(*config) + namesz + 1);
        config->name = config->data;
        memcpy(config->name, resname, namesz + 1);
        config->restype = restype;

        rd_list_init(&config->config, 8, rd_kafka_ConfigEntry_free);

        return config;
}

void rd_kafka_ConfigResource_destroy(rd_kafka_ConfigResource_t *config) {
        rd_list_destroy(&config->config);
        if (config->errstr)
                rd_free(config->errstr);
        rd_free(config);
}

static void rd_kafka_ConfigResource_free(void *ptr) {
        rd_kafka_ConfigResource_destroy((rd_kafka_ConfigResource_t *)ptr);
}


void rd_kafka_ConfigResource_destroy_array(rd_kafka_ConfigResource_t **config,
                                           size_t config_cnt) {
        size_t i;
        for (i = 0; i < config_cnt; i++)
                rd_kafka_ConfigResource_destroy(config[i]);
}


/**
 * @brief Type and name comparator for ConfigResource_t
 */
static int rd_kafka_ConfigResource_cmp(const void *_a, const void *_b) {
        const rd_kafka_ConfigResource_t *a = _a, *b = _b;
        int r = RD_CMP(a->restype, b->restype);
        if (r)
                return r;
        return strcmp(a->name, b->name);
}

/**
 * @brief Allocate a new AlterConfigs and make a copy of \p src
 */
static rd_kafka_ConfigResource_t *
rd_kafka_ConfigResource_copy(const rd_kafka_ConfigResource_t *src) {
        rd_kafka_ConfigResource_t *dst;

        dst = rd_kafka_ConfigResource_new(src->restype, src->name);

        rd_list_destroy(&dst->config); /* created in .._new() */
        rd_list_init_copy(&dst->config, &src->config);
        rd_list_copy_to(&dst->config, &src->config,
                        rd_kafka_ConfigEntry_list_copy, NULL);

        return dst;
}


static void
rd_kafka_ConfigResource_add_ConfigEntry(rd_kafka_ConfigResource_t *config,
                                        rd_kafka_ConfigEntry_t *entry) {
        rd_list_add(&config->config, entry);
}

rd_kafka_resp_err_t
rd_kafka_ConfigResource_set_config(rd_kafka_ConfigResource_t *config,
                                   const char *name,
                                   const char *value) {
        if (!name || !*name || !value)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        return rd_kafka_admin_add_config0(&config->config, name, value);
}


rd_kafka_error_t *rd_kafka_ConfigResource_add_incremental_config(
    rd_kafka_ConfigResource_t *config,
    const char *name,
    rd_kafka_AlterConfigOpType_t op_type,
    const char *value) {
        if (op_type < 0 || op_type >= RD_KAFKA_ALTER_CONFIG_OP_TYPE__CNT) {
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "Invalid alter config operation type");
        }

        if (!name || !*name) {
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          !name
                                              ? "Config name is required"
                                              : "Config name mustn't be empty");
        }

        if (op_type != RD_KAFKA_ALTER_CONFIG_OP_TYPE_DELETE && !value) {
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "Config value is required");
        }

        return rd_kafka_admin_incremental_add_config0(&config->config, name,
                                                      op_type, value);
}


const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigResource_configs(const rd_kafka_ConfigResource_t *config,
                                size_t *cntp) {
        *cntp = rd_list_cnt(&config->config);
        if (!*cntp)
                return NULL;
        return (const rd_kafka_ConfigEntry_t **)config->config.rl_elems;
}



rd_kafka_ResourceType_t
rd_kafka_ConfigResource_type(const rd_kafka_ConfigResource_t *config) {
        return config->restype;
}

const char *
rd_kafka_ConfigResource_name(const rd_kafka_ConfigResource_t *config) {
        return config->name;
}

rd_kafka_resp_err_t
rd_kafka_ConfigResource_error(const rd_kafka_ConfigResource_t *config) {
        return config->err;
}

const char *
rd_kafka_ConfigResource_error_string(const rd_kafka_ConfigResource_t *config) {
        if (!config->err)
                return NULL;
        if (config->errstr)
                return config->errstr;
        return rd_kafka_err2str(config->err);
}


/**
 * @brief Look in the provided ConfigResource_t* list for a resource of
 *        type BROKER and set its broker id in \p broker_id, returning
 *        RD_KAFKA_RESP_ERR_NO_ERROR.
 *
 *        If multiple BROKER resources are found RD_KAFKA_RESP_ERR__CONFLICT
 *        is returned and an error string is written to errstr.
 *
 *        If no BROKER resources are found RD_KAFKA_RESP_ERR_NO_ERROR
 *        is returned and \p broker_idp is set to use the coordinator.
 */
static rd_kafka_resp_err_t
rd_kafka_ConfigResource_get_single_broker_id(const rd_list_t *configs,
                                             int32_t *broker_idp,
                                             char *errstr,
                                             size_t errstr_size) {
        const rd_kafka_ConfigResource_t *config;
        int i;
        int32_t broker_id = RD_KAFKA_ADMIN_TARGET_CONTROLLER; /* Some default
                                                               * value that we
                                                               * can compare
                                                               * to below */

        RD_LIST_FOREACH(config, configs, i) {
                char *endptr;
                long int r;

                if (config->restype != RD_KAFKA_RESOURCE_BROKER)
                        continue;

                if (broker_id != RD_KAFKA_ADMIN_TARGET_CONTROLLER) {
                        rd_snprintf(errstr, errstr_size,
                                    "Only one ConfigResource of type BROKER "
                                    "is allowed per call");
                        return RD_KAFKA_RESP_ERR__CONFLICT;
                }

                /* Convert string broker-id to int32 */
                r = (int32_t)strtol(config->name, &endptr, 10);
                if (r == LONG_MIN || r == LONG_MAX || config->name == endptr ||
                    r < 0) {
                        rd_snprintf(errstr, errstr_size,
                                    "Expected an int32 broker_id for "
                                    "ConfigResource(type=BROKER, name=%s)",
                                    config->name);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }

                broker_id = r;

                /* Keep scanning to make sure there are no duplicate
                 * BROKER resources. */
        }

        *broker_idp = broker_id;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**@}*/



/**
 * @name AlterConfigs
 * @{
 *
 *
 *
 */



/**
 * @brief Parse AlterConfigsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_AlterConfigsResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t res_cnt;
        int i;
        int32_t Throttle_Time;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        rd_kafka_buf_read_arraycnt(reply, &res_cnt, RD_KAFKAP_CONFIGS_MAX);

        if (res_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args)) {
                rd_snprintf(errstr, errstr_size,
                            "Received %" PRId32
                            " ConfigResources in response "
                            "when only %d were requested",
                            res_cnt,
                            rd_list_cnt(&rko_req->rko_u.admin_request.args));
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0; i < (int)res_cnt; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg;
                int8_t res_type;
                int8_t config_resource_type;
                rd_kafkap_str_t kres_name;
                char *res_name;
                char *this_errstr = NULL;
                rd_kafka_ConfigResource_t *config;
                rd_kafka_ConfigResource_t skel;
                int orig_pos;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);
                rd_kafka_buf_read_i8(reply, &config_resource_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);
                rd_kafka_buf_skip_tags(reply);

                res_type = rd_kafka_ConfigResourceType_to_ResourceType(
                    config_resource_type);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                this_errstr =
                                    (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&this_errstr, &error_msg);
                }

                config = rd_kafka_ConfigResource_new(res_type, res_name);
                if (!config) {
                        rd_kafka_log(rko_req->rko_rk, LOG_ERR, "ADMIN",
                                     "AlterConfigs returned "
                                     "unsupported ConfigResource #%d with "
                                     "type %d and name \"%s\": ignoring",
                                     i, res_type, res_name);
                        continue;
                }

                config->err = error_code;
                if (this_errstr)
                        config->errstr = rd_strdup(this_errstr);

                /* As a convenience to the application we insert result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.restype = config->restype;
                skel.name    = config->name;
                orig_pos = rd_list_index(&rko_result->rko_u.admin_result.args,
                                         &skel, rd_kafka_ConfigResource_cmp);
                if (orig_pos == -1) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "that was not "
                            "included in the original request",
                            res_type, res_name);
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "multiple times",
                            res_type, res_name);
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            config);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "AlterConfigs response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}



void rd_kafka_AlterConfigs(rd_kafka_t *rk,
                           rd_kafka_ConfigResource_t **configs,
                           size_t config_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        rd_kafka_resp_err_t err;
        char errstr[256];
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_AlterConfigsRequest,
            rd_kafka_AlterConfigsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_ALTERCONFIGS,
                                            RD_KAFKA_EVENT_ALTERCONFIGS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)config_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0; i < config_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_ConfigResource_copy(configs[i]));

        /* If there's a BROKER resource in the list we need to
         * speak directly to that broker rather than the controller.
         *
         * Multiple BROKER resources are not allowed.
         */
        err = rd_kafka_ConfigResource_get_single_broker_id(
            &rko->rko_u.admin_request.args, &rko->rko_u.admin_request.broker_id,
            errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_kafka_q_enq(rk->rk_ops, rko);
}


const rd_kafka_ConfigResource_t **rd_kafka_AlterConfigs_result_resources(
    const rd_kafka_AlterConfigs_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_resources(
            (const rd_kafka_op_t *)result, cntp);
}

/**@}*/



/**
 * @name IncrementalAlterConfigs
 * @{
 *
 *
 *
 */



/**
 * @brief Parse IncrementalAlterConfigsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_IncrementalAlterConfigsResponse_parse(rd_kafka_op_t *rko_req,
                                               rd_kafka_op_t **rko_resultp,
                                               rd_kafka_buf_t *reply,
                                               char *errstr,
                                               size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t res_cnt;
        int i;
        int32_t Throttle_Time;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        rd_kafka_buf_read_arraycnt(reply, &res_cnt, RD_KAFKAP_CONFIGS_MAX);

        if (res_cnt != rd_list_cnt(&rko_req->rko_u.admin_request.args)) {
                rd_snprintf(errstr, errstr_size,
                            "Received %" PRId32
                            " ConfigResources in response "
                            "when %d were requested",
                            res_cnt,
                            rd_list_cnt(&rko_req->rko_u.admin_request.args));
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0; i < (int)res_cnt; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg;
                int8_t res_type;
                int8_t config_resource_type;
                rd_kafkap_str_t kres_name;
                char *res_name;
                char *this_errstr = NULL;
                rd_kafka_ConfigResource_t *config;
                rd_kafka_ConfigResource_t skel;
                int orig_pos;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);
                rd_kafka_buf_read_i8(reply, &config_resource_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);
                rd_kafka_buf_skip_tags(reply);

                res_type = rd_kafka_ConfigResourceType_to_ResourceType(
                    config_resource_type);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                this_errstr =
                                    (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&this_errstr, &error_msg);
                }

                config = rd_kafka_ConfigResource_new(res_type, res_name);
                if (!config) {
                        rd_kafka_log(rko_req->rko_rk, LOG_ERR, "ADMIN",
                                     "IncrementalAlterConfigs returned "
                                     "unsupported ConfigResource #%d with "
                                     "type %d and name \"%s\": ignoring",
                                     i, res_type, res_name);
                        continue;
                }

                config->err = error_code;
                if (this_errstr)
                        config->errstr = rd_strdup(this_errstr);

                /* As a convenience to the application we insert result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.restype = config->restype;
                skel.name    = config->name;
                orig_pos = rd_list_index(&rko_result->rko_u.admin_result.args,
                                         &skel, rd_kafka_ConfigResource_cmp);
                if (orig_pos == -1) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "that was not "
                            "included in the original request",
                            res_type, res_name);
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_ConfigResource_destroy(config);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "multiple times",
                            res_type, res_name);
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            config);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(
            errstr, errstr_size,
            "IncrementalAlterConfigs response protocol parse failure: %s",
            rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

typedef RD_MAP_TYPE(const char *, const rd_bool_t *) map_str_bool;


void rd_kafka_IncrementalAlterConfigs(rd_kafka_t *rk,
                                      rd_kafka_ConfigResource_t **configs,
                                      size_t config_cnt,
                                      const rd_kafka_AdminOptions_t *options,
                                      rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        rd_kafka_resp_err_t err;
        char errstr[256];
        rd_bool_t value = rd_true;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_IncrementalAlterConfigsRequest,
            rd_kafka_IncrementalAlterConfigsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_INCREMENTALALTERCONFIGS,
            RD_KAFKA_EVENT_INCREMENTALALTERCONFIGS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)config_cnt,
                     rd_kafka_ConfigResource_free);

        /* Check duplicate ConfigResource */
        map_str_bool configs_map = RD_MAP_INITIALIZER(
            config_cnt, rd_map_str_cmp, rd_map_str_hash, NULL, NULL);

        for (i = 0; i < config_cnt; i++) {
                /* 2 chars for the decimal restype + 1 for the comma
                 * + 1 for the trailing zero. */
                size_t len = 4 + strlen(configs[i]->name);
                char *key  = rd_alloca(len);
                const rd_kafka_ConfigEntry_t **entries;
                size_t entry_cnt, j;

                rd_snprintf(key, len - 1, "%d,%s", configs[i]->restype,
                            configs[i]->name);
                if (RD_MAP_GET(&configs_map, key)) {
                        /* Duplicate ConfigResource found */
                        break;
                }
                RD_MAP_SET(&configs_map, key, &value);
                entries =
                    rd_kafka_ConfigResource_configs(configs[i], &entry_cnt);

                /* Check duplicate ConfigEntry */
                map_str_bool entries_map = RD_MAP_INITIALIZER(
                    entry_cnt, rd_map_str_cmp, rd_map_str_hash, NULL, NULL);

                for (j = 0; j < entry_cnt; j++) {
                        const rd_kafka_ConfigEntry_t *entry = entries[j];
                        const char *key = rd_kafka_ConfigEntry_name(entry);

                        if (RD_MAP_GET(&entries_map, key)) {
                                /* Duplicate ConfigEntry found */
                                break;
                        }
                        RD_MAP_SET(&entries_map, key, &value);
                }
                RD_MAP_DESTROY(&entries_map);

                if (j != entry_cnt) {
                        RD_MAP_DESTROY(&configs_map);
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate ConfigEntry found");
                        rd_kafka_admin_common_worker_destroy(
                            rk, rko, rd_true /*destroy*/);
                        return;
                }

                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_ConfigResource_copy(configs[i]));
        }

        RD_MAP_DESTROY(&configs_map);

        if (i != config_cnt) {
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Duplicate ConfigResource found");
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        /* If there's a BROKER resource in the list we need to
         * speak directly to that broker rather than the controller.
         *
         * Multiple BROKER resources are not allowed.
         */
        err = rd_kafka_ConfigResource_get_single_broker_id(
            &rko->rko_u.admin_request.args, &rko->rko_u.admin_request.broker_id,
            errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }
        if (rko->rko_u.admin_request.broker_id !=
            RD_KAFKA_ADMIN_TARGET_CONTROLLER) {
                /* Revert broker option to default if altering
                 * broker configs. */
                err = rd_kafka_confval_set_type(
                    &rko->rko_u.admin_request.options.broker,
                    RD_KAFKA_CONFVAL_INT, NULL, errstr, sizeof(errstr));
                if (err) {
                        rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                        rd_kafka_admin_common_worker_destroy(
                            rk, rko, rd_true /*destroy*/);
                        return;
                }
        }

        rd_kafka_q_enq(rk->rk_ops, rko);
}


const rd_kafka_ConfigResource_t **
rd_kafka_IncrementalAlterConfigs_result_resources(
    const rd_kafka_IncrementalAlterConfigs_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_resources(
            (const rd_kafka_op_t *)result, cntp);
}

/**@}*/



/**
 * @name DescribeConfigs
 * @{
 *
 *
 *
 */


/**
 * @brief Parse DescribeConfigsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeConfigsResponse_parse(rd_kafka_op_t *rko_req,
                                       rd_kafka_op_t **rko_resultp,
                                       rd_kafka_buf_t *reply,
                                       char *errstr,
                                       size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_t *rk              = rkb->rkb_rk;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t res_cnt;
        int i;
        int32_t Throttle_Time;
        rd_kafka_ConfigResource_t *config = NULL;
        rd_kafka_ConfigEntry_t *entry     = NULL;

        rd_kafka_buf_read_i32(reply, &Throttle_Time);
        rd_kafka_op_throttle_time(rkb, rk->rk_rep, Throttle_Time);

        /* #resources */
        rd_kafka_buf_read_i32(reply, &res_cnt);

        if (res_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " ConfigResources in response "
                    "when only %d were requested",
                    res_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0; i < (int)res_cnt; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg;
                int8_t config_resource_type;
                int8_t res_type;
                rd_kafkap_str_t kres_name;
                char *res_name;
                char *this_errstr = NULL;
                rd_kafka_ConfigResource_t skel;
                int orig_pos;
                int32_t entry_cnt;
                int ci;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);
                rd_kafka_buf_read_i8(reply, &config_resource_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);

                res_type = rd_kafka_ConfigResourceType_to_ResourceType(
                    config_resource_type);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                this_errstr =
                                    (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&this_errstr, &error_msg);
                }

                config = rd_kafka_ConfigResource_new(res_type, res_name);
                if (!config) {
                        rd_kafka_log(rko_req->rko_rk, LOG_ERR, "ADMIN",
                                     "DescribeConfigs returned "
                                     "unsupported ConfigResource #%d with "
                                     "type %d and name \"%s\": ignoring",
                                     i, res_type, res_name);
                        continue;
                }

                config->err = error_code;
                if (this_errstr)
                        config->errstr = rd_strdup(this_errstr);

                /* #config_entries */
                rd_kafka_buf_read_i32(reply, &entry_cnt);

                for (ci = 0; ci < (int)entry_cnt; ci++) {
                        rd_kafkap_str_t config_name, config_value;
                        int32_t syn_cnt;
                        int si;

                        rd_kafka_buf_read_str(reply, &config_name);
                        rd_kafka_buf_read_str(reply, &config_value);

                        entry = rd_kafka_ConfigEntry_new0(
                            config_name.str, RD_KAFKAP_STR_LEN(&config_name),
                            config_value.str, RD_KAFKAP_STR_LEN(&config_value));

                        rd_kafka_buf_read_bool(reply, &entry->a.is_readonly);

                        /* ApiVersion 0 has is_default field, while
                         * ApiVersion 1 has source field.
                         * Convert between the two so they look the same
                         * to the caller. */
                        if (rd_kafka_buf_ApiVersion(reply) == 0) {
                                rd_kafka_buf_read_bool(reply,
                                                       &entry->a.is_default);
                                if (entry->a.is_default)
                                        entry->a.source =
                                            RD_KAFKA_CONFIG_SOURCE_DEFAULT_CONFIG;
                        } else {
                                int8_t config_source;
                                rd_kafka_buf_read_i8(reply, &config_source);
                                entry->a.source = config_source;

                                if (entry->a.source ==
                                    RD_KAFKA_CONFIG_SOURCE_DEFAULT_CONFIG)
                                        entry->a.is_default = 1;
                        }

                        rd_kafka_buf_read_bool(reply, &entry->a.is_sensitive);


                        if (rd_kafka_buf_ApiVersion(reply) == 1) {
                                /* #config_synonyms (ApiVersion 1) */
                                rd_kafka_buf_read_i32(reply, &syn_cnt);

                                if (syn_cnt > 100000)
                                        rd_kafka_buf_parse_fail(
                                            reply,
                                            "Broker returned %" PRId32
                                            " config synonyms for "
                                            "ConfigResource %d,%s: "
                                            "limit is 100000",
                                            syn_cnt, config->restype,
                                            config->name);

                                if (syn_cnt > 0)
                                        rd_list_grow(&entry->synonyms, syn_cnt);

                        } else {
                                /* No synonyms in ApiVersion 0 */
                                syn_cnt = 0;
                        }



                        /* Read synonyms (ApiVersion 1) */
                        for (si = 0; si < (int)syn_cnt; si++) {
                                rd_kafkap_str_t syn_name, syn_value;
                                int8_t syn_source;
                                rd_kafka_ConfigEntry_t *syn_entry;

                                rd_kafka_buf_read_str(reply, &syn_name);
                                rd_kafka_buf_read_str(reply, &syn_value);
                                rd_kafka_buf_read_i8(reply, &syn_source);

                                syn_entry = rd_kafka_ConfigEntry_new0(
                                    syn_name.str, RD_KAFKAP_STR_LEN(&syn_name),
                                    syn_value.str,
                                    RD_KAFKAP_STR_LEN(&syn_value));
                                if (!syn_entry)
                                        rd_kafka_buf_parse_fail(
                                            reply,
                                            "Broker returned invalid "
                                            "synonym #%d "
                                            "for ConfigEntry #%d (%s) "
                                            "and ConfigResource %d,%s: "
                                            "syn_name.len %d, "
                                            "syn_value.len %d",
                                            si, ci, entry->kv->name,
                                            config->restype, config->name,
                                            (int)syn_name.len,
                                            (int)syn_value.len);

                                syn_entry->a.source     = syn_source;
                                syn_entry->a.is_synonym = 1;

                                rd_list_add(&entry->synonyms, syn_entry);
                        }

                        rd_kafka_ConfigResource_add_ConfigEntry(config, entry);
                        entry = NULL;
                }

                /* As a convenience to the application we insert result
                 * in the same order as they were requested. The broker
                 * does not maintain ordering unfortunately. */
                skel.restype = config->restype;
                skel.name    = config->name;
                orig_pos = rd_list_index(&rko_result->rko_u.admin_result.args,
                                         &skel, rd_kafka_ConfigResource_cmp);
                if (orig_pos == -1)
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "that was not "
                            "included in the original request",
                            res_type, res_name);

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL)
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned ConfigResource %d,%s "
                            "multiple times",
                            res_type, res_name);

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            config);
                config = NULL;
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (entry)
                rd_kafka_ConfigEntry_destroy(entry);
        if (config)
                rd_kafka_ConfigResource_destroy(config);

        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DescribeConfigs response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}



void rd_kafka_DescribeConfigs(rd_kafka_t *rk,
                              rd_kafka_ConfigResource_t **configs,
                              size_t config_cnt,
                              const rd_kafka_AdminOptions_t *options,
                              rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        rd_kafka_resp_err_t err;
        char errstr[256];
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DescribeConfigsRequest,
            rd_kafka_DescribeConfigsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DESCRIBECONFIGS,
            RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT, &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)config_cnt,
                     rd_kafka_ConfigResource_free);

        for (i = 0; i < config_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_ConfigResource_copy(configs[i]));

        /* If there's a BROKER resource in the list we need to
         * speak directly to that broker rather than the controller.
         *
         * Multiple BROKER resources are not allowed.
         */
        err = rd_kafka_ConfigResource_get_single_broker_id(
            &rko->rko_u.admin_request.args, &rko->rko_u.admin_request.broker_id,
            errstr, sizeof(errstr));
        if (err) {
                rd_kafka_admin_result_fail(rko, err, "%s", errstr);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_kafka_q_enq(rk->rk_ops, rko);
}



const rd_kafka_ConfigResource_t **rd_kafka_DescribeConfigs_result_resources(
    const rd_kafka_DescribeConfigs_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_resources(
            (const rd_kafka_op_t *)result, cntp);
}

/**@}*/

/**
 * @name Delete Records
 * @{
 *
 *
 *
 *
 */

rd_kafka_DeleteRecords_t *rd_kafka_DeleteRecords_new(
    const rd_kafka_topic_partition_list_t *before_offsets) {
        rd_kafka_DeleteRecords_t *del_records;

        del_records = rd_calloc(1, sizeof(*del_records));
        del_records->offsets =
            rd_kafka_topic_partition_list_copy(before_offsets);

        return del_records;
}

void rd_kafka_DeleteRecords_destroy(rd_kafka_DeleteRecords_t *del_records) {
        rd_kafka_topic_partition_list_destroy(del_records->offsets);
        rd_free(del_records);
}

void rd_kafka_DeleteRecords_destroy_array(
    rd_kafka_DeleteRecords_t **del_records,
    size_t del_record_cnt) {
        size_t i;
        for (i = 0; i < del_record_cnt; i++)
                rd_kafka_DeleteRecords_destroy(del_records[i]);
}



/** @brief Merge the DeleteRecords response from a single broker
 *         into the user response list.
 */
static void
rd_kafka_DeleteRecords_response_merge(rd_kafka_op_t *rko_fanout,
                                      const rd_kafka_op_t *rko_partial) {
        rd_kafka_t *rk = rko_fanout->rko_rk;
        const rd_kafka_topic_partition_list_t *partitions;
        rd_kafka_topic_partition_list_t *respartitions;
        const rd_kafka_topic_partition_t *partition;

        rd_assert(rko_partial->rko_evtype ==
                  RD_KAFKA_EVENT_DELETERECORDS_RESULT);

        /* All partitions (offsets) from the DeleteRecords() call */
        respartitions =
            rd_list_elem(&rko_fanout->rko_u.admin_request.fanout.results, 0);

        if (rko_partial->rko_err) {
                /* If there was a request-level error, set the error on
                 * all requested partitions for this request. */
                const rd_kafka_topic_partition_list_t *reqpartitions;
                rd_kafka_topic_partition_t *reqpartition;

                /* Partitions (offsets) from this DeleteRecordsRequest */
                reqpartitions =
                    rd_list_elem(&rko_partial->rko_u.admin_result.args, 0);

                RD_KAFKA_TPLIST_FOREACH(reqpartition, reqpartitions) {
                        rd_kafka_topic_partition_t *respart;

                        /* Find result partition */
                        respart = rd_kafka_topic_partition_list_find(
                            respartitions, reqpartition->topic,
                            reqpartition->partition);

                        rd_assert(respart || !*"respart not found");

                        respart->err = rko_partial->rko_err;
                }

                return;
        }

        /* Partitions from the DeleteRecordsResponse */
        partitions = rd_list_elem(&rko_partial->rko_u.admin_result.results, 0);

        RD_KAFKA_TPLIST_FOREACH(partition, partitions) {
                rd_kafka_topic_partition_t *respart;


                /* Find result partition */
                respart = rd_kafka_topic_partition_list_find(
                    respartitions, partition->topic, partition->partition);
                if (unlikely(!respart)) {
                        rd_dassert(!*"partition not found");

                        rd_kafka_log(rk, LOG_WARNING, "DELETERECORDS",
                                     "DeleteRecords response contains "
                                     "unexpected %s [%" PRId32
                                     "] which "
                                     "was not in the request list: ignored",
                                     partition->topic, partition->partition);
                        continue;
                }

                respart->offset = partition->offset;
                respart->err    = partition->err;
        }
}



/**
 * @brief Parse DeleteRecordsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DeleteRecordsResponse_parse(rd_kafka_op_t *rko_req,
                                     rd_kafka_op_t **rko_resultp,
                                     rd_kafka_buf_t *reply,
                                     char *errstr,
                                     size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko_result;
        rd_kafka_topic_partition_list_t *offsets;

        rd_kafka_buf_read_throttle_time(reply);


        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_OFFSET,
            RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        offsets = rd_kafka_buf_read_topic_partitions(
            reply, rd_false /*don't use topic_id*/, rd_true, 0, fields);
        if (!offsets)
                rd_kafka_buf_parse_fail(reply,
                                        "Failed to parse topic partitions");


        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_topic_partition_list_destroy_free);
        rd_list_add(&rko_result->rko_u.admin_result.results, offsets);
        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        rd_snprintf(errstr, errstr_size,
                    "DeleteRecords response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

/**
 * @brief Creates a ListOffsetsResultInfo with the topic and parition and
 *        returns the ListOffsetsResultInfo.
 */
rd_kafka_ListOffsetsResultInfo_t *
rd_kafka_ListOffsetsResultInfo_new(rd_kafka_topic_partition_t *rktpar,
                                   rd_ts_t timestamp) {
        rd_kafka_ListOffsetsResultInfo_t *result_info;
        result_info                  = rd_calloc(1, sizeof(*result_info));
        result_info->timestamp       = timestamp;
        result_info->topic_partition = rd_kafka_topic_partition_copy(rktpar);
        return result_info;
}

/**
 * @brief Copies the ListOffsetsResultInfo.
 */
static rd_kafka_ListOffsetsResultInfo_t *rd_kafka_ListOffsetsResultInfo_copy(
    const rd_kafka_ListOffsetsResultInfo_t *result_info) {
        return rd_kafka_ListOffsetsResultInfo_new(result_info->topic_partition,
                                                  result_info->timestamp);
}

/**
 * @brief Same as rd_kafka_ListOffsetsResultInfo_copy() but suitable for
 *        rd_list_copy(). The \p opaque is ignored.
 */
static void *rd_kafka_ListOffsetsResultInfo_copy_opaque(const void *element,
                                                        void *opaque) {
        return rd_kafka_ListOffsetsResultInfo_copy(element);
}

/**
 * @brief Returns the topic partition of the passed \p result_info.
 */
const rd_kafka_topic_partition_t *
rd_kafka_ListOffsetsResultInfo_topic_partition(
    const rd_kafka_ListOffsetsResultInfo_t *result_info) {
        return result_info->topic_partition;
}

/**
 * @brief Returns the timestamp specified for the offset of the
 *        rd_kafka_ListOffsetsResultInfo_t.
 */
int64_t rd_kafka_ListOffsetsResultInfo_timestamp(
    const rd_kafka_ListOffsetsResultInfo_t *result_info) {
        return result_info->timestamp;
}

static void rd_kafka_ListOffsetsResultInfo_destroy(
    rd_kafka_ListOffsetsResultInfo_t *element) {
        rd_kafka_topic_partition_destroy(element->topic_partition);
        rd_free(element);
}

static void rd_kafka_ListOffsetsResultInfo_destroy_free(void *element) {
        rd_kafka_ListOffsetsResultInfo_destroy(element);
}

/**
 * @brief Merges the response of the partial request made for ListOffsets via
 *        the \p rko_partial into the \p rko_fanout responsible for the
 *        ListOffsets request.
 * @param rko_fanout The rd_kafka_op_t corresponding to the whole original
 *                   ListOffsets request.
 * @param rko_partial The rd_kafka_op_t corresponding to the leader specific
 *                    ListOffset request sent after leaders querying.
 */
static void
rd_kafka_ListOffsets_response_merge(rd_kafka_op_t *rko_fanout,
                                    const rd_kafka_op_t *rko_partial) {
        size_t partition_cnt;
        size_t total_partitions;
        size_t i, j;
        rd_assert(rko_partial->rko_evtype == RD_KAFKA_EVENT_LISTOFFSETS_RESULT);

        partition_cnt = rd_list_cnt(&rko_partial->rko_u.admin_result.results);
        total_partitions =
            rd_list_cnt(&rko_fanout->rko_u.admin_request.fanout.results);

        for (i = 0; i < partition_cnt; i++) {
                rd_kafka_ListOffsetsResultInfo_t *partial_result_info =
                    rd_list_elem(&rko_partial->rko_u.admin_result.results, i);
                for (j = 0; j < total_partitions; j++) {
                        rd_kafka_ListOffsetsResultInfo_t *result_info =
                            rd_list_elem(
                                &rko_fanout->rko_u.admin_request.fanout.results,
                                j);
                        if (rd_kafka_topic_partition_cmp(
                                result_info->topic_partition,
                                partial_result_info->topic_partition) == 0) {
                                result_info->timestamp =
                                    partial_result_info->timestamp;
                                rd_kafka_topic_partition_destroy(
                                    result_info->topic_partition);
                                result_info->topic_partition =
                                    rd_kafka_topic_partition_copy(
                                        partial_result_info->topic_partition);
                                break;
                        }
                }
        }
}

/**
 * @brief Returns the array of pointers of rd_kafka_ListOffsetsResultInfo_t
 * given rd_kafka_ListOffsets_result_t and populates the size of the array.
 */
const rd_kafka_ListOffsetsResultInfo_t **
rd_kafka_ListOffsets_result_infos(const rd_kafka_ListOffsets_result_t *result,
                                  size_t *cntp) {
        *cntp = rd_list_cnt(&result->rko_u.admin_result.results);
        return (const rd_kafka_ListOffsetsResultInfo_t **)
            result->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Admin compatible API to parse the ListOffsetResponse buffer
 *        provided in \p reply.
 */
static rd_kafka_resp_err_t
rd_kafka_ListOffsetsResponse_parse(rd_kafka_op_t *rko_req,
                                   rd_kafka_op_t **rko_resultp,
                                   rd_kafka_buf_t *reply,
                                   char *errstr,
                                   size_t errstr_size) {
        rd_list_t *result_list =
            rd_list_new(1, rd_kafka_ListOffsetsResultInfo_destroy_free);
        rd_kafka_op_t *rko_result;
        rd_kafka_parse_ListOffsets(reply, NULL, result_list);
        if (reply->rkbuf_err) {
                rd_snprintf(errstr, errstr_size,
                            "Error parsing ListOffsets response: %s",
                            rd_kafka_err2str(reply->rkbuf_err));
                return reply->rkbuf_err;
        }

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init_copy(&rko_result->rko_u.admin_result.results, result_list);
        rd_list_copy_to(&rko_result->rko_u.admin_result.results, result_list,
                        rd_kafka_ListOffsetsResultInfo_copy_opaque, NULL);
        rd_list_destroy(result_list);

        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Should the received error code cause a metadata refresh?
 */
static rd_bool_t rd_kafka_admin_result_err_refresh(rd_kafka_resp_err_t err) {
        switch (err) {
        case RD_KAFKA_RESP_ERR_NOT_LEADER_OR_FOLLOWER:
        case RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE:
                return rd_true;
        default:
                return rd_false;
        }
}

/**
 * @brief ListOffsets result handler for internal side effects.
 */
static void rd_kafka_ListOffsets_handle_result(rd_kafka_op_t *rko_result) {
        rd_kafka_topic_partition_list_t *rktpars;
        rd_kafka_ListOffsetsResultInfo_t *result_info;
        rd_kafka_t *rk;
        rd_kafka_resp_err_t err, rktpar_err;
        rd_kafka_topic_partition_t *rktpar;
        size_t i;

        err = rko_result->rko_err;
        if (rd_list_empty(&rko_result->rko_u.admin_result.args) ||
            rd_list_empty(&rko_result->rko_u.admin_result.results))
                return;

        rk      = rko_result->rko_rk;
        rktpars = rd_list_elem(&rko_result->rko_u.admin_result.args, 0);
        rd_kafka_wrlock(rk);
        i = 0;
        RD_KAFKA_TPLIST_FOREACH(rktpar, rktpars) {
                result_info =
                    rd_list_elem(&rko_result->rko_u.admin_result.results, i);
                rktpar_err = err ? err : result_info->topic_partition->err;

                if (rd_kafka_admin_result_err_refresh(rktpar_err)) {
                        rd_kafka_metadata_cache_delete_by_name(rk,
                                                               rktpar->topic);
                }
                i++;
        }
        rd_kafka_wrunlock(rk);
}

/**
 * @brief Call when leaders have been queried to progress the ListOffsets
 *        admin op to its next phase, sending ListOffsets to partition
 *        leaders.
 */
static rd_kafka_op_res_t
rd_kafka_ListOffsets_leaders_queried_cb(rd_kafka_t *rk,
                                        rd_kafka_q_t *rkq,
                                        rd_kafka_op_t *reply) {

        rd_kafka_resp_err_t err = reply->rko_err;
        const rd_list_t *leaders =
            reply->rko_u.leaders.leaders; /* Possibly NULL (on err) */
        rd_kafka_topic_partition_list_t *partitions =
            reply->rko_u.leaders.partitions; /* Possibly NULL (on err) */
        rd_kafka_op_t *rko_fanout = reply->rko_u.leaders.opaque;
        rd_kafka_topic_partition_list_t *topic_partitions;
        rd_kafka_topic_partition_t *rktpar;
        size_t partition_cnt;
        const struct rd_kafka_partition_leader *leader;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_ListOffsetsRequest_admin,
            rd_kafka_ListOffsetsResponse_parse,
        };

        rd_assert((rko_fanout->rko_type & ~RD_KAFKA_OP_FLAGMASK) ==
                  RD_KAFKA_OP_ADMIN_FANOUT);

        if (err) {
                rd_kafka_admin_result_fail(
                    rko_fanout, err, "Failed to query partition leaders: %s",
                    err == RD_KAFKA_RESP_ERR__NOENT ? "No leaders found"
                                                    : rd_kafka_err2str(err));
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        /* Create fanout results */
        topic_partitions =
            rd_list_elem(&rko_fanout->rko_u.admin_request.args, 0);
        partition_cnt = topic_partitions->cnt;
        rd_list_init(&rko_fanout->rko_u.admin_request.fanout.results,
                     partition_cnt,
                     rd_kafka_ListOffsetsResultInfo_destroy_free);

        for (i = 0; i < partition_cnt; i++) {
                rd_kafka_topic_partition_t *topic_partition =
                    &topic_partitions->elems[i];
                rd_kafka_ListOffsetsResultInfo_t *result_element =
                    rd_kafka_ListOffsetsResultInfo_new(topic_partition, -1);
                rd_kafka_topic_partition_set_from_fetch_pos(
                    result_element->topic_partition,
                    RD_KAFKA_FETCH_POS(RD_KAFKA_OFFSET_INVALID, -1));
                result_element->topic_partition->err =
                    RD_KAFKA_RESP_ERR_NO_ERROR;
                rd_list_add(&rko_fanout->rko_u.admin_request.fanout.results,
                            result_element);
        }

        /* Set errors to corresponding result partitions */
        RD_KAFKA_TPLIST_FOREACH(rktpar, partitions) {
                rd_kafka_ListOffsetsResultInfo_t *result_element;
                if (!rktpar->err)
                        continue;
                result_element = NULL;
                for (i = 0; i < partition_cnt; i++) {
                        result_element = rd_list_elem(
                            &rko_fanout->rko_u.admin_request.fanout.results, i);
                        if (rd_kafka_topic_partition_cmp(
                                result_element->topic_partition, rktpar) == 0)
                                break;
                }
                result_element->topic_partition->err = rktpar->err;
        }

        /* For each leader send a request for its partitions */
        rko_fanout->rko_u.admin_request.fanout.outstanding =
            rd_list_cnt(leaders);

        RD_LIST_FOREACH(leader, leaders, i) {
                rd_kafka_op_t *rko = rd_kafka_admin_request_op_new(
                    rk, RD_KAFKA_OP_LISTOFFSETS,
                    RD_KAFKA_EVENT_LISTOFFSETS_RESULT, &cbs,
                    &rko_fanout->rko_u.admin_request.options, rk->rk_ops);

                rko->rko_u.admin_request.fanout_parent = rko_fanout;
                rko->rko_u.admin_request.broker_id = leader->rkb->rkb_nodeid;

                rd_kafka_topic_partition_list_sort_by_topic(leader->partitions);
                rd_list_init(&rko->rko_u.admin_request.args, 1,
                             rd_kafka_topic_partition_list_destroy_free);
                rd_list_add(
                    &rko->rko_u.admin_request.args,
                    rd_kafka_topic_partition_list_copy(leader->partitions));

                /* Enqueue op for admin_worker() to transition to next state */
                rd_kafka_q_enq(rk->rk_ops, rko);
        }

        return RD_KAFKA_OP_RES_HANDLED;
}

/**
 * @brief Call when leaders have been queried to progress the DeleteRecords
 *        admin op to its next phase, sending DeleteRecords to partition
 *        leaders.
 */
static rd_kafka_op_res_t
rd_kafka_DeleteRecords_leaders_queried_cb(rd_kafka_t *rk,
                                          rd_kafka_q_t *rkq,
                                          rd_kafka_op_t *reply) {
        rd_kafka_resp_err_t err = reply->rko_err;
        const rd_list_t *leaders =
            reply->rko_u.leaders.leaders; /* Possibly NULL (on err) */
        rd_kafka_topic_partition_list_t *partitions =
            reply->rko_u.leaders.partitions; /* Possibly NULL (on err) */
        rd_kafka_op_t *rko_fanout = reply->rko_u.leaders.opaque;
        rd_kafka_topic_partition_t *rktpar;
        rd_kafka_topic_partition_list_t *offsets;
        const struct rd_kafka_partition_leader *leader;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DeleteRecordsRequest,
            rd_kafka_DeleteRecordsResponse_parse,
        };
        int i;

        rd_assert((rko_fanout->rko_type & ~RD_KAFKA_OP_FLAGMASK) ==
                  RD_KAFKA_OP_ADMIN_FANOUT);

        if (err == RD_KAFKA_RESP_ERR__DESTROY)
                goto err;

        /* Requested offsets */
        offsets = rd_list_elem(&rko_fanout->rko_u.admin_request.args, 0);

        /* Update the error field of each partition from the
         * leader-queried partition list so that ERR_UNKNOWN_TOPIC_OR_PART
         * and similar are propagated, since those partitions are not
         * included in the leaders list. */
        RD_KAFKA_TPLIST_FOREACH(rktpar, partitions) {
                rd_kafka_topic_partition_t *rktpar2;

                if (!rktpar->err)
                        continue;

                rktpar2 = rd_kafka_topic_partition_list_find(
                    offsets, rktpar->topic, rktpar->partition);
                rd_assert(rktpar2);
                rktpar2->err = rktpar->err;
        }


        if (err) {
        err:
                rd_kafka_admin_result_fail(
                    rko_fanout, err, "Failed to query partition leaders: %s",
                    err == RD_KAFKA_RESP_ERR__NOENT ? "No leaders found"
                                                    : rd_kafka_err2str(err));
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        /* The response lists is one element deep and that element is a
         * rd_kafka_topic_partition_list_t with the results of the deletes. */
        rd_list_init(&rko_fanout->rko_u.admin_request.fanout.results, 1,
                     rd_kafka_topic_partition_list_destroy_free);
        rd_list_add(&rko_fanout->rko_u.admin_request.fanout.results,
                    rd_kafka_topic_partition_list_copy(offsets));

        rko_fanout->rko_u.admin_request.fanout.outstanding =
            rd_list_cnt(leaders);

        rd_assert(rd_list_cnt(leaders) > 0);

        /* For each leader send a request for its partitions */
        RD_LIST_FOREACH(leader, leaders, i) {
                rd_kafka_op_t *rko = rd_kafka_admin_request_op_new(
                    rk, RD_KAFKA_OP_DELETERECORDS,
                    RD_KAFKA_EVENT_DELETERECORDS_RESULT, &cbs,
                    &rko_fanout->rko_u.admin_request.options, rk->rk_ops);
                rko->rko_u.admin_request.fanout_parent = rko_fanout;
                rko->rko_u.admin_request.broker_id = leader->rkb->rkb_nodeid;

                rd_kafka_topic_partition_list_sort_by_topic(leader->partitions);

                rd_list_init(&rko->rko_u.admin_request.args, 1,
                             rd_kafka_topic_partition_list_destroy_free);
                rd_list_add(
                    &rko->rko_u.admin_request.args,
                    rd_kafka_topic_partition_list_copy(leader->partitions));

                /* Enqueue op for admin_worker() to transition to next state */
                rd_kafka_q_enq(rk->rk_ops, rko);
        }

        return RD_KAFKA_OP_RES_HANDLED;
}


void rd_kafka_DeleteRecords(rd_kafka_t *rk,
                            rd_kafka_DeleteRecords_t **del_records,
                            size_t del_record_cnt,
                            const rd_kafka_AdminOptions_t *options,
                            rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko_fanout;
        static const struct rd_kafka_admin_fanout_worker_cbs fanout_cbs = {
            rd_kafka_DeleteRecords_response_merge,
            rd_kafka_topic_partition_list_copy_opaque,
        };
        const rd_kafka_topic_partition_list_t *offsets;
        rd_kafka_topic_partition_list_t *copied_offsets;

        rd_assert(rkqu);

        rko_fanout = rd_kafka_admin_fanout_op_new(
            rk, RD_KAFKA_OP_DELETERECORDS, RD_KAFKA_EVENT_DELETERECORDS_RESULT,
            &fanout_cbs, options, rkqu->rkqu_q);

        if (del_record_cnt != 1) {
                /* We only support one DeleteRecords per call since there
                 * is no point in passing multiples, but the API still
                 * needs to be extensible/future-proof. */
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Exactly one DeleteRecords must be "
                                           "passed");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        offsets = del_records[0]->offsets;

        if (offsets == NULL || offsets->cnt == 0) {
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "No records to delete");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        /* Copy offsets list and store it on the request op */
        copied_offsets = rd_kafka_topic_partition_list_copy(offsets);
        if (rd_kafka_topic_partition_list_has_duplicates(
                copied_offsets, rd_false /*check partition*/)) {
                rd_kafka_topic_partition_list_destroy(copied_offsets);
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Duplicate partitions not allowed");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        /* Set default error on each partition so that if any of the partitions
         * never get a request sent we have an error to indicate it. */
        rd_kafka_topic_partition_list_set_err(copied_offsets,
                                              RD_KAFKA_RESP_ERR__NOOP);

        rd_list_init(&rko_fanout->rko_u.admin_request.args, 1,
                     rd_kafka_topic_partition_list_destroy_free);
        rd_list_add(&rko_fanout->rko_u.admin_request.args, copied_offsets);

        /* Async query for partition leaders */
        rd_kafka_topic_partition_list_query_leaders_async(
            rk, copied_offsets, rd_kafka_admin_timeout_remains(rko_fanout),
            RD_KAFKA_REPLYQ(rk->rk_ops, 0),
            rd_kafka_DeleteRecords_leaders_queried_cb, rko_fanout);
}


void rd_kafka_ListOffsets(rd_kafka_t *rk,
                          rd_kafka_topic_partition_list_t *topic_partitions,
                          const rd_kafka_AdminOptions_t *options,
                          rd_kafka_queue_t *rkqu) {
        int i;
        rd_kafka_op_t *rko_fanout;
        rd_kafka_topic_partition_list_t *copied_topic_partitions;
        rd_list_t *topic_partitions_sorted = NULL;

        static const struct rd_kafka_admin_fanout_worker_cbs fanout_cbs = {
            rd_kafka_ListOffsets_response_merge,
            rd_kafka_ListOffsetsResultInfo_copy_opaque,
            rd_kafka_topic_partition_list_copy_opaque};

        rko_fanout = rd_kafka_admin_fanout_op_new(
            rk, RD_KAFKA_OP_LISTOFFSETS, RD_KAFKA_EVENT_LISTOFFSETS_RESULT,
            &fanout_cbs, options, rkqu->rkqu_q);

        rd_kafka_admin_request_op_result_cb_set(
            rko_fanout, rd_kafka_ListOffsets_handle_result);

        if (topic_partitions->cnt) {
                for (i = 0; i < topic_partitions->cnt; i++) {
                        if (!topic_partitions->elems[i].topic[0]) {
                                rd_kafka_admin_result_fail(
                                    rko_fanout, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                    "Partition topic name at index %d must be "
                                    "non-empty",
                                    i);
                                goto err;
                        }
                        if (topic_partitions->elems[i].partition < 0) {
                                rd_kafka_admin_result_fail(
                                    rko_fanout, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                    "Partition at index %d cannot be negative",
                                    i);
                                goto err;
                        }
                }


                topic_partitions_sorted =
                    rd_list_new(topic_partitions->cnt,
                                rd_kafka_topic_partition_destroy_free);
                for (i = 0; i < topic_partitions->cnt; i++)
                        rd_list_add(topic_partitions_sorted,
                                    rd_kafka_topic_partition_copy(
                                        &topic_partitions->elems[i]));

                rd_list_sort(topic_partitions_sorted,
                             rd_kafka_topic_partition_cmp);
                if (rd_list_find_duplicate(topic_partitions_sorted,
                                           rd_kafka_topic_partition_cmp)) {

                        rd_kafka_admin_result_fail(
                            rko_fanout, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Partitions must not contain duplicates");
                        goto err;
                }
        }

        for (i = 0; i < topic_partitions->cnt; i++) {
                rd_kafka_topic_partition_t *partition =
                    &topic_partitions->elems[i];
                if (partition->offset < RD_KAFKA_OFFSET_SPEC_MAX_TIMESTAMP) {
                        rd_kafka_admin_result_fail(
                            rko_fanout, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Partition %d has an invalid offset %" PRId64, i,
                            partition->offset);
                        goto err;
                }
        }

        copied_topic_partitions =
            rd_kafka_topic_partition_list_copy(topic_partitions);
        rd_list_init(&rko_fanout->rko_u.admin_request.args, 1,
                     rd_kafka_topic_partition_list_destroy_free);
        rd_list_add(&rko_fanout->rko_u.admin_request.args,
                    copied_topic_partitions);

        if (topic_partitions->cnt) {
                /* Async query for partition leaders */
                rd_kafka_topic_partition_list_query_leaders_async(
                    rk, copied_topic_partitions,
                    rd_kafka_admin_timeout_remains(rko_fanout),
                    RD_KAFKA_REPLYQ(rk->rk_ops, 0),
                    rd_kafka_ListOffsets_leaders_queried_cb, rko_fanout);
        } else {
                /* Empty list */
                rd_kafka_op_t *rko_result =
                    rd_kafka_admin_result_new(rko_fanout);
                /* Enqueue empty result on application queue, we're done. */
                rd_kafka_admin_result_enq(rko_fanout, rko_result);
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
        }

        RD_IF_FREE(topic_partitions_sorted, rd_list_destroy);
        return;
err:
        RD_IF_FREE(topic_partitions_sorted, rd_list_destroy);
        rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                             rd_true /*destroy*/);
}

/**
 * @brief Get the list of offsets from a DeleteRecords result.
 *
 * The returned \p offsets life-time is the same as the \p result object.
 */
const rd_kafka_topic_partition_list_t *rd_kafka_DeleteRecords_result_offsets(
    const rd_kafka_DeleteRecords_result_t *result) {
        const rd_kafka_topic_partition_list_t *offsets;
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        size_t cnt;

        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DELETERECORDS);

        cnt = rd_list_cnt(&rko->rko_u.admin_result.results);

        rd_assert(cnt == 1);

        offsets = (const rd_kafka_topic_partition_list_t *)rd_list_elem(
            &rko->rko_u.admin_result.results, 0);

        rd_assert(offsets);

        return offsets;
}

/**@}*/

/**
 * @name Delete groups
 * @{
 *
 *
 *
 *
 */

rd_kafka_DeleteGroup_t *rd_kafka_DeleteGroup_new(const char *group) {
        size_t tsize = strlen(group) + 1;
        rd_kafka_DeleteGroup_t *del_group;

        /* Single allocation */
        del_group        = rd_malloc(sizeof(*del_group) + tsize);
        del_group->group = del_group->data;
        memcpy(del_group->group, group, tsize);

        return del_group;
}

void rd_kafka_DeleteGroup_destroy(rd_kafka_DeleteGroup_t *del_group) {
        rd_free(del_group);
}

static void rd_kafka_DeleteGroup_free(void *ptr) {
        rd_kafka_DeleteGroup_destroy(ptr);
}

void rd_kafka_DeleteGroup_destroy_array(rd_kafka_DeleteGroup_t **del_groups,
                                        size_t del_group_cnt) {
        size_t i;
        for (i = 0; i < del_group_cnt; i++)
                rd_kafka_DeleteGroup_destroy(del_groups[i]);
}

/**
 * @brief Group name comparator for DeleteGroup_t
 */
static int rd_kafka_DeleteGroup_cmp(const void *_a, const void *_b) {
        const rd_kafka_DeleteGroup_t *a = _a, *b = _b;
        return strcmp(a->group, b->group);
}

/**
 * @brief Allocate a new DeleteGroup and make a copy of \p src
 */
static rd_kafka_DeleteGroup_t *
rd_kafka_DeleteGroup_copy(const rd_kafka_DeleteGroup_t *src) {
        return rd_kafka_DeleteGroup_new(src->group);
}


/**
 * @brief Parse DeleteGroupsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DeleteGroupsResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        int32_t group_cnt;
        int i;
        rd_kafka_op_t *rko_result = NULL;

        rd_kafka_buf_read_throttle_time(reply);

        /* #group_error_codes */
        rd_kafka_buf_read_i32(reply, &group_cnt);

        if (group_cnt > rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " groups in response "
                    "when only %d were requested",
                    group_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, group_cnt,
                     rd_kafka_group_result_free);

        for (i = 0; i < (int)group_cnt; i++) {
                rd_kafkap_str_t kgroup;
                int16_t error_code;
                rd_kafka_group_result_t *groupres;

                rd_kafka_buf_read_str(reply, &kgroup);
                rd_kafka_buf_read_i16(reply, &error_code);

                groupres = rd_kafka_group_result_new(
                    kgroup.str, RD_KAFKAP_STR_LEN(&kgroup), NULL,
                    error_code ? rd_kafka_error_new(error_code, NULL) : NULL);

                rd_list_add(&rko_result->rko_u.admin_result.results, groupres);
        }

        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DeleteGroups response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

/** @brief Merge the DeleteGroups response from a single broker
 *         into the user response list.
 */
void rd_kafka_DeleteGroups_response_merge(rd_kafka_op_t *rko_fanout,
                                          const rd_kafka_op_t *rko_partial) {
        const rd_kafka_group_result_t *groupres = NULL;
        rd_kafka_group_result_t *newgroupres;
        const rd_kafka_DeleteGroup_t *grp =
            rko_partial->rko_u.admin_result.opaque;
        int orig_pos;

        rd_assert(rko_partial->rko_evtype ==
                  RD_KAFKA_EVENT_DELETEGROUPS_RESULT);

        if (!rko_partial->rko_err) {
                /* Proper results.
                 * We only send one group per request, make sure it matches */
                groupres =
                    rd_list_elem(&rko_partial->rko_u.admin_result.results, 0);
                rd_assert(groupres);
                rd_assert(!strcmp(groupres->group, grp->group));
                newgroupres = rd_kafka_group_result_copy(groupres);
        } else {
                /* Op errored, e.g. timeout */
                newgroupres = rd_kafka_group_result_new(
                    grp->group, -1, NULL,
                    rd_kafka_error_new(rko_partial->rko_err, NULL));
        }

        /* As a convenience to the application we insert group result
         * in the same order as they were requested. */
        orig_pos = rd_list_index(&rko_fanout->rko_u.admin_request.args, grp,
                                 rd_kafka_DeleteGroup_cmp);
        rd_assert(orig_pos != -1);

        /* Make sure result is not already set */
        rd_assert(rd_list_elem(&rko_fanout->rko_u.admin_request.fanout.results,
                               orig_pos) == NULL);

        rd_list_set(&rko_fanout->rko_u.admin_request.fanout.results, orig_pos,
                    newgroupres);
}

void rd_kafka_DeleteGroups(rd_kafka_t *rk,
                           rd_kafka_DeleteGroup_t **del_groups,
                           size_t del_group_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko_fanout;
        rd_list_t dup_list;
        size_t i;
        static const struct rd_kafka_admin_fanout_worker_cbs fanout_cbs = {
            rd_kafka_DeleteGroups_response_merge,
            rd_kafka_group_result_copy_opaque,
        };

        rd_assert(rkqu);

        rko_fanout = rd_kafka_admin_fanout_op_new(
            rk, RD_KAFKA_OP_DELETEGROUPS, RD_KAFKA_EVENT_DELETEGROUPS_RESULT,
            &fanout_cbs, options, rkqu->rkqu_q);

        if (del_group_cnt == 0) {
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "No groups to delete");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        /* Copy group list and store it on the request op.
         * Maintain original ordering. */
        rd_list_init(&rko_fanout->rko_u.admin_request.args, (int)del_group_cnt,
                     rd_kafka_DeleteGroup_free);
        for (i = 0; i < del_group_cnt; i++)
                rd_list_add(&rko_fanout->rko_u.admin_request.args,
                            rd_kafka_DeleteGroup_copy(del_groups[i]));

        /* Check for duplicates.
         * Make a temporary copy of the group list and sort it to check for
         * duplicates, we don't want the original list sorted since we want
         * to maintain ordering. */
        rd_list_init(&dup_list,
                     rd_list_cnt(&rko_fanout->rko_u.admin_request.args), NULL);
        rd_list_copy_to(&dup_list, &rko_fanout->rko_u.admin_request.args, NULL,
                        NULL);
        rd_list_sort(&dup_list, rd_kafka_DeleteGroup_cmp);
        if (rd_list_find_duplicate(&dup_list, rd_kafka_DeleteGroup_cmp)) {
                rd_list_destroy(&dup_list);
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Duplicate groups not allowed");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_list_destroy(&dup_list);

        /* Prepare results list where fanned out op's results will be
         * accumulated. */
        rd_list_init(&rko_fanout->rko_u.admin_request.fanout.results,
                     (int)del_group_cnt, rd_kafka_group_result_free);
        rko_fanout->rko_u.admin_request.fanout.outstanding = (int)del_group_cnt;

        /* Create individual request ops for each group.
         * FIXME: A future optimization is to coalesce all groups for a single
         *        coordinator into one op. */
        for (i = 0; i < del_group_cnt; i++) {
                static const struct rd_kafka_admin_worker_cbs cbs = {
                    rd_kafka_DeleteGroupsRequest,
                    rd_kafka_DeleteGroupsResponse_parse,
                };
                rd_kafka_DeleteGroup_t *grp =
                    rd_list_elem(&rko_fanout->rko_u.admin_request.args, (int)i);
                rd_kafka_op_t *rko = rd_kafka_admin_request_op_new(
                    rk, RD_KAFKA_OP_DELETEGROUPS,
                    RD_KAFKA_EVENT_DELETEGROUPS_RESULT, &cbs, options,
                    rk->rk_ops);

                rko->rko_u.admin_request.fanout_parent = rko_fanout;
                rko->rko_u.admin_request.broker_id =
                    RD_KAFKA_ADMIN_TARGET_COORDINATOR;
                rko->rko_u.admin_request.coordtype = RD_KAFKA_COORD_GROUP;
                rko->rko_u.admin_request.coordkey  = rd_strdup(grp->group);

                /* Set the group name as the opaque so the fanout worker use it
                 * to fill in errors.
                 * References rko_fanout's memory, which will always outlive
                 * the fanned out op. */
                rd_kafka_AdminOptions_set_opaque(
                    &rko->rko_u.admin_request.options, grp);

                rd_list_init(&rko->rko_u.admin_request.args, 1,
                             rd_kafka_DeleteGroup_free);
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_DeleteGroup_copy(del_groups[i]));

                rd_kafka_q_enq(rk->rk_ops, rko);
        }
}


/**
 * @brief Get an array of group results from a DeleteGroups result.
 *
 * The returned \p groups life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_group_result_t **rd_kafka_DeleteGroups_result_groups(
    const rd_kafka_DeleteGroups_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_groups((const rd_kafka_op_t *)result,
                                                cntp);
}


/**@}*/


/**
 * @name Delete consumer group offsets (committed offsets)
 * @{
 *
 *
 *
 *
 */

rd_kafka_DeleteConsumerGroupOffsets_t *rd_kafka_DeleteConsumerGroupOffsets_new(
    const char *group,
    const rd_kafka_topic_partition_list_t *partitions) {
        size_t tsize = strlen(group) + 1;
        rd_kafka_DeleteConsumerGroupOffsets_t *del_grpoffsets;

        rd_assert(partitions);

        /* Single allocation */
        del_grpoffsets        = rd_malloc(sizeof(*del_grpoffsets) + tsize);
        del_grpoffsets->group = del_grpoffsets->data;
        memcpy(del_grpoffsets->group, group, tsize);
        del_grpoffsets->partitions =
            rd_kafka_topic_partition_list_copy(partitions);

        return del_grpoffsets;
}

void rd_kafka_DeleteConsumerGroupOffsets_destroy(
    rd_kafka_DeleteConsumerGroupOffsets_t *del_grpoffsets) {
        rd_kafka_topic_partition_list_destroy(del_grpoffsets->partitions);
        rd_free(del_grpoffsets);
}

static void rd_kafka_DeleteConsumerGroupOffsets_free(void *ptr) {
        rd_kafka_DeleteConsumerGroupOffsets_destroy(ptr);
}

void rd_kafka_DeleteConsumerGroupOffsets_destroy_array(
    rd_kafka_DeleteConsumerGroupOffsets_t **del_grpoffsets,
    size_t del_grpoffsets_cnt) {
        size_t i;
        for (i = 0; i < del_grpoffsets_cnt; i++)
                rd_kafka_DeleteConsumerGroupOffsets_destroy(del_grpoffsets[i]);
}


/**
 * @brief Allocate a new DeleteGroup and make a copy of \p src
 */
static rd_kafka_DeleteConsumerGroupOffsets_t *
rd_kafka_DeleteConsumerGroupOffsets_copy(
    const rd_kafka_DeleteConsumerGroupOffsets_t *src) {
        return rd_kafka_DeleteConsumerGroupOffsets_new(src->group,
                                                       src->partitions);
}


/**
 * @brief Parse OffsetDeleteResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_OffsetDeleteResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko_result;
        int16_t ErrorCode;
        rd_kafka_topic_partition_list_t *partitions = NULL;
        const rd_kafka_DeleteConsumerGroupOffsets_t *del_grpoffsets;

        rd_kafka_buf_read_i16(reply, &ErrorCode);
        if (ErrorCode) {
                rd_snprintf(errstr, errstr_size,
                            "OffsetDelete response error: %s",
                            rd_kafka_err2str(ErrorCode));
                return ErrorCode;
        }

        rd_kafka_buf_read_throttle_time(reply);


        const rd_kafka_topic_partition_field_t fields[] = {
            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
            RD_KAFKA_TOPIC_PARTITION_FIELD_ERR,
            RD_KAFKA_TOPIC_PARTITION_FIELD_END};
        partitions = rd_kafka_buf_read_topic_partitions(
            reply, rd_false /*don't use topic_id*/, rd_true, 16, fields);
        if (!partitions) {
                rd_snprintf(errstr, errstr_size,
                            "Failed to parse OffsetDeleteResponse partitions");
                return RD_KAFKA_RESP_ERR__BAD_MSG;
        }


        /* Create result op and group_result_t */
        rko_result     = rd_kafka_admin_result_new(rko_req);
        del_grpoffsets = rd_list_elem(&rko_result->rko_u.admin_result.args, 0);

        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_group_result_free);
        rd_list_add(&rko_result->rko_u.admin_result.results,
                    rd_kafka_group_result_new(del_grpoffsets->group, -1,
                                              partitions, NULL));
        rd_kafka_topic_partition_list_destroy(partitions);

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        rd_snprintf(errstr, errstr_size,
                    "OffsetDelete response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));
        return reply->rkbuf_err;
}


void rd_kafka_DeleteConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_DeleteConsumerGroupOffsets_t **del_grpoffsets,
    size_t del_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu) {
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_OffsetDeleteRequest,
            rd_kafka_OffsetDeleteResponse_parse,
        };
        rd_kafka_op_t *rko;

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS,
            RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        if (del_grpoffsets_cnt != 1) {
                /* For simplicity we only support one single group for now */
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Exactly one "
                                           "DeleteConsumerGroupOffsets must "
                                           "be passed");
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }


        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_COORDINATOR;
        rko->rko_u.admin_request.coordtype = RD_KAFKA_COORD_GROUP;
        rko->rko_u.admin_request.coordkey = rd_strdup(del_grpoffsets[0]->group);

        /* Store copy of group on request so the group name can be reached
         * from the response parser. */
        rd_list_init(&rko->rko_u.admin_request.args, 1,
                     rd_kafka_DeleteConsumerGroupOffsets_free);
        rd_list_add(
            &rko->rko_u.admin_request.args,
            rd_kafka_DeleteConsumerGroupOffsets_copy(del_grpoffsets[0]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}


/**
 * @brief Get an array of group results from a DeleteGroups result.
 *
 * The returned \p groups life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_group_result_t **
rd_kafka_DeleteConsumerGroupOffsets_result_groups(
    const rd_kafka_DeleteConsumerGroupOffsets_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_groups((const rd_kafka_op_t *)result,
                                                cntp);
}

void rd_kafka_DeleteConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_DeleteConsumerGroupOffsets_t **del_grpoffsets,
    size_t del_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu);

/**@}*/
/**
 * @name CreateAcls
 * @{
 *
 *
 *
 */

const char *rd_kafka_AclOperation_name(rd_kafka_AclOperation_t operation) {
        static const char *names[] = {"UNKNOWN",
                                      "ANY",
                                      "ALL",
                                      "READ",
                                      "WRITE",
                                      "CREATE",
                                      "DELETE",
                                      "ALTER",
                                      "DESCRIBE",
                                      "CLUSTER_ACTION",
                                      "DESCRIBE_CONFIGS",
                                      "ALTER_CONFIGS",
                                      "IDEMPOTENT_WRITE"};

        if ((unsigned int)operation >=
            (unsigned int)RD_KAFKA_ACL_OPERATION__CNT)
                return "UNSUPPORTED";

        return names[operation];
}

const char *
rd_kafka_AclPermissionType_name(rd_kafka_AclPermissionType_t permission_type) {
        static const char *names[] = {"UNKNOWN", "ANY", "DENY", "ALLOW"};

        if ((unsigned int)permission_type >=
            (unsigned int)RD_KAFKA_ACL_PERMISSION_TYPE__CNT)
                return "UNSUPPORTED";

        return names[permission_type];
}

static rd_kafka_AclBinding_t *
rd_kafka_AclBinding_new0(rd_kafka_ResourceType_t restype,
                         const char *name,
                         rd_kafka_ResourcePatternType_t resource_pattern_type,
                         const char *principal,
                         const char *host,
                         rd_kafka_AclOperation_t operation,
                         rd_kafka_AclPermissionType_t permission_type,
                         rd_kafka_resp_err_t err,
                         const char *errstr) {
        rd_kafka_AclBinding_t *acl_binding;

        acl_binding       = rd_calloc(1, sizeof(*acl_binding));
        acl_binding->name = name != NULL ? rd_strdup(name) : NULL;
        acl_binding->principal =
            principal != NULL ? rd_strdup(principal) : NULL;
        acl_binding->host    = host != NULL ? rd_strdup(host) : NULL;
        acl_binding->restype = restype;
        acl_binding->resource_pattern_type = resource_pattern_type;
        acl_binding->operation             = operation;
        acl_binding->permission_type       = permission_type;
        if (err)
                acl_binding->error = rd_kafka_error_new(err, "%s", errstr);

        return acl_binding;
}

rd_kafka_AclBinding_t *
rd_kafka_AclBinding_new(rd_kafka_ResourceType_t restype,
                        const char *name,
                        rd_kafka_ResourcePatternType_t resource_pattern_type,
                        const char *principal,
                        const char *host,
                        rd_kafka_AclOperation_t operation,
                        rd_kafka_AclPermissionType_t permission_type,
                        char *errstr,
                        size_t errstr_size) {
        if (!name) {
                rd_snprintf(errstr, errstr_size, "Invalid resource name");
                return NULL;
        }
        if (!principal) {
                rd_snprintf(errstr, errstr_size, "Invalid principal");
                return NULL;
        }
        if (!host) {
                rd_snprintf(errstr, errstr_size, "Invalid host");
                return NULL;
        }

        if (restype == RD_KAFKA_RESOURCE_ANY ||
            restype <= RD_KAFKA_RESOURCE_UNKNOWN ||
            restype >= RD_KAFKA_RESOURCE__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid resource type");
                return NULL;
        }

        if (resource_pattern_type == RD_KAFKA_RESOURCE_PATTERN_ANY ||
            resource_pattern_type == RD_KAFKA_RESOURCE_PATTERN_MATCH ||
            resource_pattern_type <= RD_KAFKA_RESOURCE_PATTERN_UNKNOWN ||
            resource_pattern_type >= RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid resource pattern type");
                return NULL;
        }

        if (operation == RD_KAFKA_ACL_OPERATION_ANY ||
            operation <= RD_KAFKA_ACL_OPERATION_UNKNOWN ||
            operation >= RD_KAFKA_ACL_OPERATION__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid operation");
                return NULL;
        }

        if (permission_type == RD_KAFKA_ACL_PERMISSION_TYPE_ANY ||
            permission_type <= RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN ||
            permission_type >= RD_KAFKA_ACL_PERMISSION_TYPE__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid permission type");
                return NULL;
        }

        return rd_kafka_AclBinding_new0(
            restype, name, resource_pattern_type, principal, host, operation,
            permission_type, RD_KAFKA_RESP_ERR_NO_ERROR, NULL);
}

rd_kafka_AclBindingFilter_t *rd_kafka_AclBindingFilter_new(
    rd_kafka_ResourceType_t restype,
    const char *name,
    rd_kafka_ResourcePatternType_t resource_pattern_type,
    const char *principal,
    const char *host,
    rd_kafka_AclOperation_t operation,
    rd_kafka_AclPermissionType_t permission_type,
    char *errstr,
    size_t errstr_size) {


        if (restype <= RD_KAFKA_RESOURCE_UNKNOWN ||
            restype >= RD_KAFKA_RESOURCE__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid resource type");
                return NULL;
        }

        if (resource_pattern_type <= RD_KAFKA_RESOURCE_PATTERN_UNKNOWN ||
            resource_pattern_type >= RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT) {
                rd_snprintf(errstr, errstr_size,
                            "Invalid resource pattern type");
                return NULL;
        }

        if (operation <= RD_KAFKA_ACL_OPERATION_UNKNOWN ||
            operation >= RD_KAFKA_ACL_OPERATION__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid operation");
                return NULL;
        }

        if (permission_type <= RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN ||
            permission_type >= RD_KAFKA_ACL_PERMISSION_TYPE__CNT) {
                rd_snprintf(errstr, errstr_size, "Invalid permission type");
                return NULL;
        }

        return rd_kafka_AclBinding_new0(
            restype, name, resource_pattern_type, principal, host, operation,
            permission_type, RD_KAFKA_RESP_ERR_NO_ERROR, NULL);
}

rd_kafka_ResourceType_t
rd_kafka_AclBinding_restype(const rd_kafka_AclBinding_t *acl) {
        return acl->restype;
}

const char *rd_kafka_AclBinding_name(const rd_kafka_AclBinding_t *acl) {
        return acl->name;
}

const char *rd_kafka_AclBinding_principal(const rd_kafka_AclBinding_t *acl) {
        return acl->principal;
}

const char *rd_kafka_AclBinding_host(const rd_kafka_AclBinding_t *acl) {
        return acl->host;
}

rd_kafka_AclOperation_t
rd_kafka_AclBinding_operation(const rd_kafka_AclBinding_t *acl) {
        return acl->operation;
}

rd_kafka_AclPermissionType_t
rd_kafka_AclBinding_permission_type(const rd_kafka_AclBinding_t *acl) {
        return acl->permission_type;
}

rd_kafka_ResourcePatternType_t
rd_kafka_AclBinding_resource_pattern_type(const rd_kafka_AclBinding_t *acl) {
        return acl->resource_pattern_type;
}

const rd_kafka_error_t *
rd_kafka_AclBinding_error(const rd_kafka_AclBinding_t *acl) {
        return acl->error;
}

/**
 * @brief Allocate a new AclBinding and make a copy of \p src
 */
static rd_kafka_AclBinding_t *
rd_kafka_AclBinding_copy(const rd_kafka_AclBinding_t *src) {
        rd_kafka_AclBinding_t *dst;

        dst = rd_kafka_AclBinding_new(
            src->restype, src->name, src->resource_pattern_type, src->principal,
            src->host, src->operation, src->permission_type, NULL, 0);
        rd_assert(dst);
        return dst;
}

/**
 * @brief Allocate a new AclBindingFilter and make a copy of \p src
 */
static rd_kafka_AclBindingFilter_t *
rd_kafka_AclBindingFilter_copy(const rd_kafka_AclBindingFilter_t *src) {
        rd_kafka_AclBindingFilter_t *dst;

        dst = rd_kafka_AclBindingFilter_new(
            src->restype, src->name, src->resource_pattern_type, src->principal,
            src->host, src->operation, src->permission_type, NULL, 0);
        rd_assert(dst);
        return dst;
}

void rd_kafka_AclBinding_destroy(rd_kafka_AclBinding_t *acl_binding) {
        if (acl_binding->name)
                rd_free(acl_binding->name);
        if (acl_binding->principal)
                rd_free(acl_binding->principal);
        if (acl_binding->host)
                rd_free(acl_binding->host);
        if (acl_binding->error)
                rd_kafka_error_destroy(acl_binding->error);
        rd_free(acl_binding);
}

static void rd_kafka_AclBinding_free(void *ptr) {
        rd_kafka_AclBinding_destroy(ptr);
}


void rd_kafka_AclBinding_destroy_array(rd_kafka_AclBinding_t **acl_bindings,
                                       size_t acl_bindings_cnt) {
        size_t i;
        for (i = 0; i < acl_bindings_cnt; i++)
                rd_kafka_AclBinding_destroy(acl_bindings[i]);
}

/**
 * @brief Parse CreateAclsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_CreateAclsResponse_parse(rd_kafka_op_t *rko_req,
                                  rd_kafka_op_t **rko_resultp,
                                  rd_kafka_buf_t *reply,
                                  char *errstr,
                                  size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t acl_cnt;
        int i;

        rd_kafka_buf_read_throttle_time(reply);

        rd_kafka_buf_read_arraycnt(reply, &acl_cnt, 100000);

        if (acl_cnt != rd_list_cnt(&rko_req->rko_u.admin_request.args))
                rd_kafka_buf_parse_fail(
                    reply,
                    "Received %" PRId32
                    " acls in response, but %d were requested",
                    acl_cnt, rd_list_cnt(&rko_req->rko_u.admin_request.args));

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, acl_cnt,
                     rd_kafka_acl_result_free);

        for (i = 0; i < (int)acl_cnt; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg = RD_KAFKAP_STR_INITIALIZER;
                rd_kafka_acl_result_t *acl_res;
                char *errstr = NULL;

                rd_kafka_buf_read_i16(reply, &error_code);

                rd_kafka_buf_read_str(reply, &error_msg);

                if (error_code) {
                        if (RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
                }

                acl_res = rd_kafka_acl_result_new(
                    error_code ? rd_kafka_error_new(error_code, "%s", errstr)
                               : NULL);

                rd_list_set(&rko_result->rko_u.admin_result.results, i,
                            acl_res);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "CreateAcls response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}

void rd_kafka_CreateAcls(rd_kafka_t *rk,
                         rd_kafka_AclBinding_t **new_acls,
                         size_t new_acls_cnt,
                         const rd_kafka_AdminOptions_t *options,
                         rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_CreateAclsRequest, rd_kafka_CreateAclsResponse_parse};

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_CREATEACLS,
                                            RD_KAFKA_EVENT_CREATEACLS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)new_acls_cnt,
                     rd_kafka_AclBinding_free);

        for (i = 0; i < new_acls_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_AclBinding_copy(new_acls[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}

/**
 * @brief Get an array of rd_kafka_acl_result_t from a CreateAcls result.
 *
 * The returned \p rd_kafka_acl_result_t life-time is the same as the \p result
 * object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_acl_result_t **
rd_kafka_CreateAcls_result_acls(const rd_kafka_CreateAcls_result_t *result,
                                size_t *cntp) {
        return rd_kafka_admin_result_ret_acl_results(
            (const rd_kafka_op_t *)result, cntp);
}

/**@}*/

/**
 * @name DescribeAcls
 * @{
 *
 *
 *
 */

/**
 * @brief Parse DescribeAclsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeAclsResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR_NO_ERROR;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t res_cnt;
        int i;
        int j;
        rd_kafka_AclBinding_t *acl = NULL;
        int16_t error_code;
        rd_kafkap_str_t error_msg;

        rd_kafka_buf_read_throttle_time(reply);

        rd_kafka_buf_read_i16(reply, &error_code);
        rd_kafka_buf_read_str(reply, &error_msg);

        if (error_code) {
                if (RD_KAFKAP_STR_LEN(&error_msg) == 0)
                        errstr = (char *)rd_kafka_err2str(error_code);
                else
                        RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
        }

        /* #resources */
        rd_kafka_buf_read_arraycnt(reply, &res_cnt, 100000);

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_AclBinding_free);

        for (i = 0; i < (int)res_cnt; i++) {
                int8_t res_type = RD_KAFKA_RESOURCE_UNKNOWN;
                rd_kafkap_str_t kres_name;
                char *res_name;
                int8_t resource_pattern_type =
                    RD_KAFKA_RESOURCE_PATTERN_LITERAL;
                int32_t acl_cnt;

                rd_kafka_buf_read_i8(reply, &res_type);
                rd_kafka_buf_read_str(reply, &kres_name);
                RD_KAFKAP_STR_DUPA(&res_name, &kres_name);

                if (rd_kafka_buf_ApiVersion(reply) >= 1) {
                        rd_kafka_buf_read_i8(reply, &resource_pattern_type);
                }

                if (res_type <= RD_KAFKA_RESOURCE_UNKNOWN ||
                    res_type >= RD_KAFKA_RESOURCE__CNT) {
                        rd_rkb_log(rkb, LOG_WARNING, "DESCRIBEACLSRESPONSE",
                                   "DescribeAclsResponse returned unknown "
                                   "resource type %d",
                                   res_type);
                        res_type = RD_KAFKA_RESOURCE_UNKNOWN;
                }
                if (resource_pattern_type <=
                        RD_KAFKA_RESOURCE_PATTERN_UNKNOWN ||
                    resource_pattern_type >=
                        RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT) {
                        rd_rkb_log(rkb, LOG_WARNING, "DESCRIBEACLSRESPONSE",
                                   "DescribeAclsResponse returned unknown "
                                   "resource pattern type %d",
                                   resource_pattern_type);
                        resource_pattern_type =
                            RD_KAFKA_RESOURCE_PATTERN_UNKNOWN;
                }

                /* #resources */
                rd_kafka_buf_read_arraycnt(reply, &acl_cnt, 100000);

                for (j = 0; j < (int)acl_cnt; j++) {
                        rd_kafkap_str_t kprincipal;
                        rd_kafkap_str_t khost;
                        int8_t operation = RD_KAFKA_ACL_OPERATION_UNKNOWN;
                        int8_t permission_type =
                            RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN;
                        char *principal;
                        char *host;

                        rd_kafka_buf_read_str(reply, &kprincipal);
                        rd_kafka_buf_read_str(reply, &khost);
                        rd_kafka_buf_read_i8(reply, &operation);
                        rd_kafka_buf_read_i8(reply, &permission_type);
                        RD_KAFKAP_STR_DUPA(&principal, &kprincipal);
                        RD_KAFKAP_STR_DUPA(&host, &khost);

                        if (operation <= RD_KAFKA_ACL_OPERATION_UNKNOWN ||
                            operation >= RD_KAFKA_ACL_OPERATION__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DESCRIBEACLSRESPONSE",
                                           "DescribeAclsResponse returned "
                                           "unknown acl operation %d",
                                           operation);
                                operation = RD_KAFKA_ACL_OPERATION_UNKNOWN;
                        }
                        if (permission_type <=
                                RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN ||
                            permission_type >=
                                RD_KAFKA_ACL_PERMISSION_TYPE__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DESCRIBEACLSRESPONSE",
                                           "DescribeAclsResponse returned "
                                           "unknown acl permission type %d",
                                           permission_type);
                                permission_type =
                                    RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN;
                        }

                        acl = rd_kafka_AclBinding_new0(
                            res_type, res_name, resource_pattern_type,
                            principal, host, operation, permission_type,
                            RD_KAFKA_RESP_ERR_NO_ERROR, NULL);

                        rd_list_add(&rko_result->rko_u.admin_result.results,
                                    acl);
                }
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DescribeAcls response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}

void rd_kafka_DescribeAcls(rd_kafka_t *rk,
                           rd_kafka_AclBindingFilter_t *acl_filter,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DescribeAclsRequest,
            rd_kafka_DescribeAclsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_DESCRIBEACLS,
                                            RD_KAFKA_EVENT_DESCRIBEACLS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, 1,
                     rd_kafka_AclBinding_free);

        rd_list_add(&rko->rko_u.admin_request.args,
                    rd_kafka_AclBindingFilter_copy(acl_filter));

        rd_kafka_q_enq(rk->rk_ops, rko);
}

struct rd_kafka_ScramCredentialInfo_s {
        rd_kafka_ScramMechanism_t mechanism;
        int32_t iterations;
};

rd_kafka_ScramMechanism_t rd_kafka_ScramCredentialInfo_mechanism(
    const rd_kafka_ScramCredentialInfo_t *scram_credential_info) {
        return scram_credential_info->mechanism;
}

int32_t rd_kafka_ScramCredentialInfo_iterations(
    const rd_kafka_ScramCredentialInfo_t *scram_credential_info) {
        return scram_credential_info->iterations;
}

struct rd_kafka_UserScramCredentialsDescription_s {
        char *user;
        rd_kafka_error_t *error;
        size_t credential_info_cnt;
        rd_kafka_ScramCredentialInfo_t *credential_infos;
};

rd_kafka_UserScramCredentialsDescription_t *
rd_kafka_UserScramCredentialsDescription_new(const char *username,
                                             size_t num_credentials) {
        rd_kafka_UserScramCredentialsDescription_t *description;
        description                      = rd_calloc(1, sizeof(*description));
        description->user                = rd_strdup(username);
        description->error               = NULL;
        description->credential_info_cnt = num_credentials;
        description->credential_infos    = NULL;
        if (num_credentials > 0) {
                rd_kafka_ScramCredentialInfo_t *credentialinfo;
                description->credential_infos =
                    rd_calloc(num_credentials, sizeof(*credentialinfo));
        }
        return description;
}

void rd_kafka_UserScramCredentialsDescription_destroy(
    rd_kafka_UserScramCredentialsDescription_t *description) {
        if (!description)
                return;
        rd_free(description->user);
        rd_kafka_error_destroy(description->error);
        if (description->credential_infos)
                rd_free(description->credential_infos);
        rd_free(description);
}

void rd_kafka_UserScramCredentialsDescription_destroy_free(void *description) {
        rd_kafka_UserScramCredentialsDescription_destroy(description);
}

void rd_kafka_UserScramCredentailsDescription_set_error(
    rd_kafka_UserScramCredentialsDescription_t *description,
    rd_kafka_resp_err_t errorcode,
    const char *err) {
        rd_kafka_error_destroy(description->error);
        description->error = rd_kafka_error_new(errorcode, "%s", err);
}

const char *rd_kafka_UserScramCredentialsDescription_user(
    const rd_kafka_UserScramCredentialsDescription_t *description) {
        return description->user;
}

const rd_kafka_error_t *rd_kafka_UserScramCredentialsDescription_error(
    const rd_kafka_UserScramCredentialsDescription_t *description) {
        return description->error;
}

size_t rd_kafka_UserScramCredentialsDescription_scramcredentialinfo_count(
    const rd_kafka_UserScramCredentialsDescription_t *description) {
        return description->credential_info_cnt;
}

const rd_kafka_ScramCredentialInfo_t *
rd_kafka_UserScramCredentialsDescription_scramcredentialinfo(
    const rd_kafka_UserScramCredentialsDescription_t *description,
    size_t idx) {
        return &description->credential_infos[idx];
}

const rd_kafka_UserScramCredentialsDescription_t **
rd_kafka_DescribeUserScramCredentials_result_descriptions(
    const rd_kafka_DescribeUserScramCredentials_result_t *result,
    size_t *cntp) {
        *cntp = rd_list_cnt(&result->rko_u.admin_result.results);
        return (const rd_kafka_UserScramCredentialsDescription_t **)
            result->rko_u.admin_result.results.rl_elems;
}

rd_kafka_resp_err_t
rd_kafka_DescribeUserScramCredentialsRequest(rd_kafka_broker_t *rkb,
                                             const rd_list_t *userlist,
                                             rd_kafka_AdminOptions_t *options,
                                             char *errstr,
                                             size_t errstr_size,
                                             rd_kafka_replyq_t replyq,
                                             rd_kafka_resp_cb_t *resp_cb,
                                             void *opaque) {
        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        size_t i;
        size_t num_users;

        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DescribeUserScramCredentials, 0, 0, &features);
        if (ApiVersion == -1) {
                rd_snprintf(
                    errstr, errstr_size,
                    "DescribeUserScramCredentials API (KIP-554) not supported "
                    "by broker");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        num_users = rd_list_cnt(userlist);

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_DescribeUserScramCredentials, 1, num_users * 25,
            rd_true);
        /* #Users */
        rd_kafka_buf_write_arraycnt(rkbuf, num_users);
        for (i = 0; i < num_users; i++) {
                rd_kafkap_str_t *user = rd_list_elem(userlist, i);
                /* Name */
                rd_kafka_buf_write_str(rkbuf, user->str, user->len);
                rd_kafka_buf_write_tags_empty(rkbuf);
        }
        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);
        /* Last Tag buffer included automatically*/
        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

static rd_kafka_resp_err_t
rd_kafka_DescribeUserScramCredentialsResponse_parse(rd_kafka_op_t *rko_req,
                                                    rd_kafka_op_t **rko_resultp,
                                                    rd_kafka_buf_t *reply,
                                                    char *errstr,
                                                    size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t num_users;
        int16_t ErrorCode;
        rd_kafkap_str_t ErrorMessage = RD_KAFKAP_STR_INITIALIZER;
        int32_t i;

        rko_result = rd_kafka_admin_result_new(rko_req);

        /* ThrottleTimeMs */
        rd_kafka_buf_read_throttle_time(reply);

        /* ErrorCode */
        rd_kafka_buf_read_i16(reply, &ErrorCode);
        rko_result->rko_err = ErrorCode; /*Request Level Error Code */

        /* ErrorMessage */
        rd_kafka_buf_read_str(reply, &ErrorMessage);
        if (ErrorCode) {
                if (RD_KAFKAP_STR_LEN(&ErrorMessage) == 0)
                        errstr = (char *)rd_kafka_err2str(ErrorCode);
                else
                        RD_KAFKAP_STR_DUPA(&errstr, &ErrorMessage);
                rko_result->rko_u.admin_result.errstr =
                    errstr; /* Request Level Error string*/
        }

        /* #Results */
        rd_kafka_buf_read_arraycnt(reply, &num_users, 10000);
        rd_list_init(&rko_result->rko_u.admin_result.results, num_users,
                     rd_kafka_UserScramCredentialsDescription_destroy_free);

        for (i = 0; i < num_users; i++) {
                rd_kafkap_str_t User;
                int16_t ErrorCode;
                rd_kafkap_str_t ErrorMessage = RD_KAFKAP_STR_INITIALIZER;
                size_t itr;
                /* User */
                rd_kafka_buf_read_str(reply, &User);
                /* ErrorCode */
                rd_kafka_buf_read_i16(reply, &ErrorCode);
                /* ErrorMessage */
                rd_kafka_buf_read_str(reply, &ErrorMessage);

                int32_t num_credentials;
                /* #CredentialInfos */
                rd_kafka_buf_read_arraycnt(reply, &num_credentials, 10000);
                rd_kafka_UserScramCredentialsDescription_t *description =
                    rd_kafka_UserScramCredentialsDescription_new(
                        User.str, num_credentials);
                rd_kafka_UserScramCredentailsDescription_set_error(
                    description, ErrorCode, ErrorMessage.str);
                for (itr = 0; itr < (size_t)num_credentials; itr++) {
                        int8_t Mechanism;
                        int32_t Iterations;
                        /* Mechanism */
                        rd_kafka_buf_read_i8(reply, &Mechanism);
                        /* Iterations */
                        rd_kafka_buf_read_i32(reply, &Iterations);
                        rd_kafka_buf_skip_tags(reply);
                        rd_kafka_ScramCredentialInfo_t *scram_credential =
                            &description->credential_infos[itr];
                        scram_credential->mechanism  = Mechanism;
                        scram_credential->iterations = Iterations;
                }
                rd_kafka_buf_skip_tags(reply);
                rd_list_add(&rko_result->rko_u.admin_result.results,
                            description);
        }
        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(
            errstr, errstr_size,
            "DescribeUserScramCredentials response protocol parse failure: %s",
            rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

void rd_kafka_DescribeUserScramCredentials(
    rd_kafka_t *rk,
    const char **users,
    size_t user_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu) {

        rd_kafka_op_t *rko;
        size_t i;
        rd_list_t *userlist = NULL;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DescribeUserScramCredentialsRequest,
            rd_kafka_DescribeUserScramCredentialsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DESCRIBEUSERSCRAMCREDENTIALS,
            RD_KAFKA_EVENT_DESCRIBEUSERSCRAMCREDENTIALS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        /* Check empty strings */
        for (i = 0; i < user_cnt; i++) {
                if (!*users[i]) {
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Empty users aren't allowed, "
                            "index %" PRIusz,
                            i);
                        goto err;
                }
        }

        /* Check Duplicates */
        if (user_cnt > 1) {
                userlist = rd_list_new(user_cnt, rd_free);
                for (i = 0; i < user_cnt; i++) {
                        rd_list_add(userlist, rd_strdup(users[i]));
                }
                rd_list_sort(userlist, rd_strcmp2);
                if (rd_list_find_duplicate(userlist, rd_strcmp2)) {
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate users aren't allowed "
                            "in the same request");
                        goto err;
                }
                rd_list_destroy(userlist);
        }

        rd_list_init(&rko->rko_u.admin_request.args, user_cnt, rd_free);
        for (i = 0; i < user_cnt; i++) {
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafkap_str_new(users[i], -1));
        }
        rd_kafka_q_enq(rk->rk_ops, rko);
        return;
err:
        RD_IF_FREE(userlist, rd_list_destroy);
        rd_kafka_admin_common_worker_destroy(rk, rko, rd_true /*destroy*/);
}

/**
 * @enum rd_kafka_UserScramCredentialAlteration_type_t
 * @brief Types of user SCRAM alterations.
 */
typedef enum rd_kafka_UserScramCredentialAlteration_type_s {
        RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT = 0,
        RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_DELETE = 1,
        RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE__CNT
} rd_kafka_UserScramCredentialAlteration_type_t;

struct rd_kafka_UserScramCredentialAlteration_s {
        char *user;
        rd_kafka_UserScramCredentialAlteration_type_t alteration_type;
        union {
                struct {
                        rd_kafka_ScramCredentialInfo_t credential_info;
                        rd_kafkap_bytes_t *salt;
                        rd_kafkap_bytes_t *password;
                } upsertion;
                struct {
                        rd_kafka_ScramMechanism_t mechanism;
                } deletion;
        } alteration;
};

rd_kafka_UserScramCredentialAlteration_t *
rd_kafka_UserScramCredentialUpsertion_new(const char *username,
                                          rd_kafka_ScramMechanism_t mechanism,
                                          int32_t iterations,
                                          const unsigned char *password,
                                          size_t password_size,
                                          const unsigned char *salt,
                                          size_t salt_size) {
        rd_kafka_UserScramCredentialAlteration_t *alteration;
        alteration       = rd_calloc(1, sizeof(*alteration));
        alteration->user = rd_strdup(username);
        alteration->alteration_type =
            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT;
        alteration->alteration.upsertion.credential_info.mechanism = mechanism;
        alteration->alteration.upsertion.credential_info.iterations =
            iterations;

        alteration->alteration.upsertion.password =
            rd_kafkap_bytes_new(password, password_size);
        if (salt_size != 0) {
                alteration->alteration.upsertion.salt =
                    rd_kafkap_bytes_new(salt, salt_size);
        } else {
#if WITH_SSL && OPENSSL_VERSION_NUMBER >= 0x10101000L
                unsigned char random_salt[64];
                if (RAND_priv_bytes(random_salt, sizeof(random_salt)) == 1) {
                        alteration->alteration.upsertion.salt =
                            rd_kafkap_bytes_new(random_salt,
                                                sizeof(random_salt));
                }
#endif
        }
        return alteration;
}

rd_kafka_UserScramCredentialAlteration_t *
rd_kafka_UserScramCredentialDeletion_new(const char *username,
                                         rd_kafka_ScramMechanism_t mechanism) {
        rd_kafka_UserScramCredentialAlteration_t *alteration;
        alteration       = rd_calloc(1, sizeof(*alteration));
        alteration->user = rd_strdup(username);
        alteration->alteration_type =
            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_DELETE;
        alteration->alteration.deletion.mechanism = mechanism;
        return alteration;
}

void rd_kafka_UserScramCredentialAlteration_destroy(
    rd_kafka_UserScramCredentialAlteration_t *alteration) {
        if (!alteration)
                return;
        rd_free(alteration->user);
        if (alteration->alteration_type ==
            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT) {
                rd_kafkap_bytes_destroy(alteration->alteration.upsertion.salt);
                rd_kafkap_bytes_destroy(
                    alteration->alteration.upsertion.password);
        }
        rd_free(alteration);
}

void rd_kafka_UserScramCredentialAlteration_destroy_free(void *alteration) {
        rd_kafka_UserScramCredentialAlteration_destroy(alteration);
}

void rd_kafka_UserScramCredentialAlteration_destroy_array(
    rd_kafka_UserScramCredentialAlteration_t **alterations,
    size_t alteration_cnt) {
        size_t i;
        for (i = 0; i < alteration_cnt; i++)
                rd_kafka_UserScramCredentialAlteration_destroy(alterations[i]);
}

static rd_kafka_UserScramCredentialAlteration_t *
rd_kafka_UserScramCredentialAlteration_copy(
    const rd_kafka_UserScramCredentialAlteration_t *alteration) {
        rd_kafka_UserScramCredentialAlteration_t *copied_alteration =
            rd_calloc(1, sizeof(*alteration));
        copied_alteration->user            = rd_strdup(alteration->user);
        copied_alteration->alteration_type = alteration->alteration_type;

        if (alteration->alteration_type ==
            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT /*Upsert*/) {
                copied_alteration->alteration.upsertion.salt =
                    rd_kafkap_bytes_copy(alteration->alteration.upsertion.salt);
                copied_alteration->alteration.upsertion.password =
                    rd_kafkap_bytes_copy(
                        alteration->alteration.upsertion.password);
                copied_alteration->alteration.upsertion.credential_info
                    .mechanism =
                    alteration->alteration.upsertion.credential_info.mechanism;
                copied_alteration->alteration.upsertion.credential_info
                    .iterations =
                    alteration->alteration.upsertion.credential_info.iterations;
        } else if (
            alteration->alteration_type ==
            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_DELETE /*Delete*/) {
                copied_alteration->alteration.deletion.mechanism =
                    alteration->alteration.deletion.mechanism;
        }

        return copied_alteration;
}

struct rd_kafka_AlterUserScramCredentials_result_response_s {
        char *user;
        rd_kafka_error_t *error;
};

rd_kafka_AlterUserScramCredentials_result_response_t *
rd_kafka_AlterUserScramCredentials_result_response_new(const char *username) {
        rd_kafka_AlterUserScramCredentials_result_response_t *response;
        response        = rd_calloc(1, sizeof(*response));
        response->user  = rd_strdup(username);
        response->error = NULL;
        return response;
}

void rd_kafka_AlterUserScramCredentials_result_response_destroy(
    rd_kafka_AlterUserScramCredentials_result_response_t *response) {
        if (response->user)
                rd_free(response->user);
        rd_kafka_error_destroy(response->error);
        rd_free(response);
}

void rd_kafka_AlterUserScramCredentials_result_response_destroy_free(
    void *response) {
        rd_kafka_AlterUserScramCredentials_result_response_destroy(response);
}

void rd_kafka_AlterUserScramCredentials_result_response_set_error(
    rd_kafka_AlterUserScramCredentials_result_response_t *response,
    rd_kafka_resp_err_t errorcode,
    const char *errstr) {
        rd_kafka_error_destroy(response->error);
        response->error = rd_kafka_error_new(errorcode, "%s", errstr);
}

const char *rd_kafka_AlterUserScramCredentials_result_response_user(
    const rd_kafka_AlterUserScramCredentials_result_response_t *response) {
        return response->user;
}

const rd_kafka_error_t *
rd_kafka_AlterUserScramCredentials_result_response_error(
    const rd_kafka_AlterUserScramCredentials_result_response_t *response) {
        return response->error;
}

const rd_kafka_AlterUserScramCredentials_result_response_t **
rd_kafka_AlterUserScramCredentials_result_responses(
    const rd_kafka_AlterUserScramCredentials_result_t *result,
    size_t *cntp) {
        *cntp = rd_list_cnt(&result->rko_u.admin_result.results);
        return (const rd_kafka_AlterUserScramCredentials_result_response_t **)
            result->rko_u.admin_result.results.rl_elems;
}


#if WITH_SSL
static rd_kafkap_bytes_t *
rd_kafka_AlterUserScramCredentialsRequest_salted_password(
    rd_kafka_broker_t *rkb,
    rd_kafkap_bytes_t *salt,
    rd_kafkap_bytes_t *password,
    rd_kafka_ScramMechanism_t mechanism,
    int32_t iterations) {
        rd_chariov_t saltedpassword_chariov = {.ptr =
                                                   rd_alloca(EVP_MAX_MD_SIZE)};

        rd_chariov_t salt_chariov;
        salt_chariov.ptr  = (char *)salt->data;
        salt_chariov.size = RD_KAFKAP_BYTES_LEN(salt);

        rd_chariov_t password_chariov;
        password_chariov.ptr  = (char *)password->data;
        password_chariov.size = RD_KAFKAP_BYTES_LEN(password);

        const EVP_MD *evp = NULL;
        if (mechanism == RD_KAFKA_SCRAM_MECHANISM_SHA_256)
                evp = EVP_sha256();
        else if (mechanism == RD_KAFKA_SCRAM_MECHANISM_SHA_512)
                evp = EVP_sha512();
        rd_assert(evp != NULL);

        rd_kafka_ssl_hmac(rkb, evp, &password_chariov, &salt_chariov,
                          iterations, &saltedpassword_chariov);

        return rd_kafkap_bytes_new(
            (const unsigned char *)saltedpassword_chariov.ptr,
            saltedpassword_chariov.size);
}
#endif

rd_kafka_resp_err_t rd_kafka_AlterUserScramCredentialsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *user_scram_credential_alterations,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {

        rd_kafka_buf_t *rkbuf;
        int16_t ApiVersion = 0;
        int features;
        size_t num_deletions = 0;
        size_t i;
        size_t num_alterations;
        size_t of_deletions;
        ApiVersion = rd_kafka_broker_ApiVersion_supported(
            rkb, RD_KAFKAP_DescribeUserScramCredentials, 0, 0, &features);
        if (ApiVersion == -1) {
                rd_snprintf(
                    errstr, errstr_size,
                    "AlterUserScramCredentials API (KIP-554) not supported "
                    "by broker");
                return RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE;
        }

        num_alterations = rd_list_cnt(user_scram_credential_alterations);

        rkbuf = rd_kafka_buf_new_flexver_request(
            rkb, RD_KAFKAP_AlterUserScramCredentials, 1, num_alterations * 100,
            rd_true);

        /* Deletion scram requests*/

        /* #Deletions */
        of_deletions = rd_kafka_buf_write_arraycnt_pos(rkbuf);

        for (i = 0; i < num_alterations; i++) {
                rd_kafka_UserScramCredentialAlteration_t *alteration =
                    rd_list_elem(user_scram_credential_alterations, i);
                if (alteration->alteration_type !=
                    RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_DELETE)
                        continue;

                num_deletions++;
                /* Name */
                rd_kafka_buf_write_str(rkbuf, alteration->user,
                                       strlen(alteration->user));
                /* Mechanism */
                rd_kafka_buf_write_i8(
                    rkbuf, alteration->alteration.deletion.mechanism);
                rd_kafka_buf_write_tags_empty(rkbuf);
        }
        rd_kafka_buf_finalize_arraycnt(rkbuf, of_deletions, num_deletions);

        /* Upsertion scram request*/

        /* #Upsertions */
        rd_kafka_buf_write_arraycnt(rkbuf, num_alterations - num_deletions);
        for (i = 0; i < num_alterations; i++) {
                rd_kafka_UserScramCredentialAlteration_t *alteration =
                    rd_list_elem(user_scram_credential_alterations, i);
                if (alteration->alteration_type !=
                    RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT)
                        continue;

#if !WITH_SSL
                rd_assert(!*"OpenSSL is required for upsertions");
#else
                char *user      = alteration->user;
                size_t usersize = strlen(user);
                rd_kafka_ScramMechanism_t mechanism =
                    alteration->alteration.upsertion.credential_info.mechanism;
                int32_t iterations =
                    alteration->alteration.upsertion.credential_info.iterations;
                /* Name */
                rd_kafka_buf_write_str(rkbuf, user, usersize);

                /* Mechanism */
                rd_kafka_buf_write_i8(rkbuf, mechanism);

                /* Iterations */
                rd_kafka_buf_write_i32(rkbuf, iterations);

                /* Salt */
                rd_kafka_buf_write_kbytes(
                    rkbuf, alteration->alteration.upsertion.salt);

                rd_kafkap_bytes_t *password_bytes =
                    rd_kafka_AlterUserScramCredentialsRequest_salted_password(
                        rkb, alteration->alteration.upsertion.salt,
                        alteration->alteration.upsertion.password, mechanism,
                        iterations);

                /* SaltedPassword */
                rd_kafka_buf_write_kbytes(rkbuf, password_bytes);
                rd_kafkap_bytes_destroy(password_bytes);
                rd_kafka_buf_write_tags_empty(rkbuf);
#endif
        }

        rd_kafka_buf_write_tags_empty(rkbuf);
        rd_kafka_buf_ApiVersion_set(rkbuf, ApiVersion, 0);
        rd_kafka_broker_buf_enq_replyq(rkb, rkbuf, replyq, resp_cb, opaque);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

rd_kafka_resp_err_t
rd_kafka_AlterUserScramCredentialsResponse_parse(rd_kafka_op_t *rko_req,
                                                 rd_kafka_op_t **rko_resultp,
                                                 rd_kafka_buf_t *reply,
                                                 char *errstr,
                                                 size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko_result   = NULL;
        int32_t num_results;
        int32_t i;

        rko_result = rd_kafka_admin_result_new(rko_req);

        /* ThrottleTimeMs */
        rd_kafka_buf_read_throttle_time(reply);

        /* #Results */
        rd_kafka_buf_read_arraycnt(reply, &num_results, 10000);

        rd_list_init(
            &rko_result->rko_u.admin_result.results, num_results,
            rd_kafka_AlterUserScramCredentials_result_response_destroy_free);
        for (i = 0; i < num_results; i++) {
                rd_kafkap_str_t User;
                int16_t ErrorCode;
                rd_kafkap_str_t ErrorMessage = RD_KAFKAP_STR_INITIALIZER;

                /* User */
                rd_kafka_buf_read_str(reply, &User);

                /* ErrorCode */
                rd_kafka_buf_read_i16(reply, &ErrorCode);

                /* ErrorMessage */
                rd_kafka_buf_read_str(reply, &ErrorMessage);

                rd_kafka_buf_skip_tags(reply);

                rd_kafka_AlterUserScramCredentials_result_response_t *response =
                    rd_kafka_AlterUserScramCredentials_result_response_new(
                        User.str);
                rd_kafka_AlterUserScramCredentials_result_response_set_error(
                    response, ErrorCode, ErrorMessage.str);
                rd_list_add(&rko_result->rko_u.admin_result.results, response);
        }
        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(
            errstr, errstr_size,
            "AlterUserScramCredentials response protocol parse failure: %s",
            rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

void rd_kafka_AlterUserScramCredentials(
    rd_kafka_t *rk,
    rd_kafka_UserScramCredentialAlteration_t **alterations,
    size_t alteration_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu) {

        rd_kafka_op_t *rko;
        size_t i;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_AlterUserScramCredentialsRequest,
            rd_kafka_AlterUserScramCredentialsResponse_parse,
        };

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_ALTERUSERSCRAMCREDENTIALS,
            RD_KAFKA_EVENT_ALTERUSERSCRAMCREDENTIALS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        if (alteration_cnt > 0) {
                const char *errstr = NULL;
                for (i = 0; i < alteration_cnt; i++) {
                        rd_bool_t is_upsert =
                            alterations[i]->alteration_type ==
                            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_UPSERT;
                        rd_bool_t is_delete =
                            alterations[i]->alteration_type ==
                            RD_KAFKA_USER_SCRAM_CREDENTIAL_ALTERATION_TYPE_DELETE;

                        if ((is_upsert || is_delete) &&
                            alterations[i]
                                    ->alteration.upsertion.credential_info
                                    .mechanism ==
                                RD_KAFKA_SCRAM_MECHANISM_UNKNOWN) {
                                errstr =
                                    "SCRAM mechanism must be specified at "
                                    "index %" PRIusz;
                                break;
                        }


                        if (!alterations[i]->user || !*alterations[i]->user) {
                                errstr = "Empty user at index %" PRIusz;
                                break;
                        }

                        if (is_upsert) {
#if !WITH_SSL
                                errstr =
                                    "OpenSSL required for upsertion at index "
                                    "%" PRIusz;
                                break;
#endif
                                if (RD_KAFKAP_BYTES_LEN(
                                        alterations[i]
                                            ->alteration.upsertion.password) ==
                                    0) {
                                        errstr =
                                            "Empty password at index %" PRIusz;
                                        break;
                                }

                                if (!alterations[i]
                                         ->alteration.upsertion.salt ||
                                    RD_KAFKAP_BYTES_LEN(
                                        alterations[i]
                                            ->alteration.upsertion.salt) == 0) {
                                        errstr = "Empty salt at index %" PRIusz;
                                        break;
                                }

                                if (alterations[i]
                                        ->alteration.upsertion.credential_info
                                        .iterations <= 0) {
                                        errstr =
                                            "Non-positive iterations at index "
                                            "%" PRIusz;
                                        break;
                                }
                        }
                }

                if (errstr) {
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG, errstr, i);
                        rd_kafka_admin_common_worker_destroy(
                            rk, rko, rd_true /*destroy*/);
                        return;
                }
        } else {
                rd_kafka_admin_result_fail(
                    rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "At least one alteration is required");
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_list_init(&rko->rko_u.admin_request.args, alteration_cnt,
                     rd_kafka_UserScramCredentialAlteration_destroy_free);

        for (i = 0; i < alteration_cnt; i++) {
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_UserScramCredentialAlteration_copy(
                                alterations[i]));
        }
        rd_kafka_q_enq(rk->rk_ops, rko);
        return;
}

/**
 * @brief Get an array of rd_kafka_AclBinding_t from a DescribeAcls result.
 *
 * The returned \p rd_kafka_AclBinding_t life-time is the same as the \p result
 * object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_AclBinding_t **
rd_kafka_DescribeAcls_result_acls(const rd_kafka_DescribeAcls_result_t *result,
                                  size_t *cntp) {
        return rd_kafka_admin_result_ret_acl_bindings(
            (const rd_kafka_op_t *)result, cntp);
}

/**@}*/

/**
 * @name DeleteAcls
 * @{
 *
 *
 *
 */

/**
 * @brief Allocate a new DeleteAcls result response with the given
 * \p err error code and \p errstr error message.
 */
const rd_kafka_DeleteAcls_result_response_t *
rd_kafka_DeleteAcls_result_response_new(rd_kafka_resp_err_t err, char *errstr) {
        rd_kafka_DeleteAcls_result_response_t *result_response;

        result_response = rd_calloc(1, sizeof(*result_response));
        if (err)
                result_response->error = rd_kafka_error_new(
                    err, "%s", errstr ? errstr : rd_kafka_err2str(err));

        /* List of int32 lists */
        rd_list_init(&result_response->matching_acls, 0,
                     rd_kafka_AclBinding_free);

        return result_response;
}

static void rd_kafka_DeleteAcls_result_response_destroy(
    rd_kafka_DeleteAcls_result_response_t *resp) {
        if (resp->error)
                rd_kafka_error_destroy(resp->error);
        rd_list_destroy(&resp->matching_acls);
        rd_free(resp);
}

static void rd_kafka_DeleteAcls_result_response_free(void *ptr) {
        rd_kafka_DeleteAcls_result_response_destroy(
            (rd_kafka_DeleteAcls_result_response_t *)ptr);
}

/**
 * @brief Get an array of rd_kafka_AclBinding_t from a DescribeAcls result.
 *
 * The returned \p rd_kafka_AclBinding_t life-time is the same as the \p result
 * object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_DeleteAcls_result_response_t **
rd_kafka_DeleteAcls_result_responses(const rd_kafka_DeleteAcls_result_t *result,
                                     size_t *cntp) {
        return rd_kafka_admin_result_ret_delete_acl_result_responses(
            (const rd_kafka_op_t *)result, cntp);
}

const rd_kafka_error_t *rd_kafka_DeleteAcls_result_response_error(
    const rd_kafka_DeleteAcls_result_response_t *result_response) {
        return result_response->error;
}

const rd_kafka_AclBinding_t **rd_kafka_DeleteAcls_result_response_matching_acls(
    const rd_kafka_DeleteAcls_result_response_t *result_response,
    size_t *matching_acls_cntp) {
        *matching_acls_cntp = result_response->matching_acls.rl_cnt;
        return (const rd_kafka_AclBinding_t **)
            result_response->matching_acls.rl_elems;
}

/**
 * @brief Parse DeleteAclsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DeleteAclsResponse_parse(rd_kafka_op_t *rko_req,
                                  rd_kafka_op_t **rko_resultp,
                                  rd_kafka_buf_t *reply,
                                  char *errstr,
                                  size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        rd_kafka_broker_t *rkb      = reply->rkbuf_rkb;
        rd_kafka_op_t *rko_result   = NULL;
        rd_kafka_resp_err_t err     = RD_KAFKA_RESP_ERR_NO_ERROR;
        int32_t res_cnt;
        int i;
        int j;

        rd_kafka_buf_read_throttle_time(reply);

        /* #responses */
        rd_kafka_buf_read_arraycnt(reply, &res_cnt, 100000);

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, res_cnt,
                     rd_kafka_DeleteAcls_result_response_free);

        for (i = 0; i < (int)res_cnt; i++) {
                int16_t error_code;
                rd_kafkap_str_t error_msg = RD_KAFKAP_STR_INITIALIZER;
                char *errstr              = NULL;
                const rd_kafka_DeleteAcls_result_response_t *result_response;
                int32_t matching_acls_cnt;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &error_msg);

                if (error_code) {
                        if (RD_KAFKAP_STR_IS_NULL(&error_msg) ||
                            RD_KAFKAP_STR_LEN(&error_msg) == 0)
                                errstr = (char *)rd_kafka_err2str(error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&errstr, &error_msg);
                }

                result_response =
                    rd_kafka_DeleteAcls_result_response_new(error_code, errstr);

                /* #matching_acls */
                rd_kafka_buf_read_arraycnt(reply, &matching_acls_cnt, 100000);
                for (j = 0; j < (int)matching_acls_cnt; j++) {
                        int16_t acl_error_code;
                        int8_t res_type = RD_KAFKA_RESOURCE_UNKNOWN;
                        rd_kafkap_str_t acl_error_msg =
                            RD_KAFKAP_STR_INITIALIZER;
                        rd_kafkap_str_t kres_name;
                        rd_kafkap_str_t khost;
                        rd_kafkap_str_t kprincipal;
                        int8_t resource_pattern_type =
                            RD_KAFKA_RESOURCE_PATTERN_LITERAL;
                        int8_t operation = RD_KAFKA_ACL_OPERATION_UNKNOWN;
                        int8_t permission_type =
                            RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN;
                        rd_kafka_AclBinding_t *matching_acl;
                        char *acl_errstr = NULL;
                        char *res_name;
                        char *principal;
                        char *host;

                        rd_kafka_buf_read_i16(reply, &acl_error_code);
                        rd_kafka_buf_read_str(reply, &acl_error_msg);
                        if (acl_error_code) {
                                if (RD_KAFKAP_STR_IS_NULL(&acl_error_msg) ||
                                    RD_KAFKAP_STR_LEN(&acl_error_msg) == 0)
                                        acl_errstr = (char *)rd_kafka_err2str(
                                            acl_error_code);
                                else
                                        RD_KAFKAP_STR_DUPA(&acl_errstr,
                                                           &acl_error_msg);
                        }

                        rd_kafka_buf_read_i8(reply, &res_type);
                        rd_kafka_buf_read_str(reply, &kres_name);

                        if (rd_kafka_buf_ApiVersion(reply) >= 1) {
                                rd_kafka_buf_read_i8(reply,
                                                     &resource_pattern_type);
                        }

                        rd_kafka_buf_read_str(reply, &kprincipal);
                        rd_kafka_buf_read_str(reply, &khost);
                        rd_kafka_buf_read_i8(reply, &operation);
                        rd_kafka_buf_read_i8(reply, &permission_type);
                        RD_KAFKAP_STR_DUPA(&res_name, &kres_name);
                        RD_KAFKAP_STR_DUPA(&principal, &kprincipal);
                        RD_KAFKAP_STR_DUPA(&host, &khost);

                        if (res_type <= RD_KAFKA_RESOURCE_UNKNOWN ||
                            res_type >= RD_KAFKA_RESOURCE__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DELETEACLSRESPONSE",
                                           "DeleteAclsResponse returned "
                                           "unknown resource type %d",
                                           res_type);
                                res_type = RD_KAFKA_RESOURCE_UNKNOWN;
                        }
                        if (resource_pattern_type <=
                                RD_KAFKA_RESOURCE_PATTERN_UNKNOWN ||
                            resource_pattern_type >=
                                RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DELETEACLSRESPONSE",
                                           "DeleteAclsResponse returned "
                                           "unknown resource pattern type %d",
                                           resource_pattern_type);
                                resource_pattern_type =
                                    RD_KAFKA_RESOURCE_PATTERN_UNKNOWN;
                        }
                        if (operation <= RD_KAFKA_ACL_OPERATION_UNKNOWN ||
                            operation >= RD_KAFKA_ACL_OPERATION__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DELETEACLSRESPONSE",
                                           "DeleteAclsResponse returned "
                                           "unknown acl operation %d",
                                           operation);
                                operation = RD_KAFKA_ACL_OPERATION_UNKNOWN;
                        }
                        if (permission_type <=
                                RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN ||
                            permission_type >=
                                RD_KAFKA_ACL_PERMISSION_TYPE__CNT) {
                                rd_rkb_log(rkb, LOG_WARNING,
                                           "DELETEACLSRESPONSE",
                                           "DeleteAclsResponse returned "
                                           "unknown acl permission type %d",
                                           permission_type);
                                permission_type =
                                    RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN;
                        }

                        matching_acl = rd_kafka_AclBinding_new0(
                            res_type, res_name, resource_pattern_type,
                            principal, host, operation, permission_type,
                            acl_error_code, acl_errstr);

                        rd_list_add(
                            (rd_list_t *)&result_response->matching_acls,
                            (void *)matching_acl);
                }

                rd_list_add(&rko_result->rko_u.admin_result.results,
                            (void *)result_response);
        }

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "DeleteAcls response protocol parse failure: %s",
                    rd_kafka_err2str(err));

        return err;
}


void rd_kafka_DeleteAcls(rd_kafka_t *rk,
                         rd_kafka_AclBindingFilter_t **del_acls,
                         size_t del_acls_cnt,
                         const rd_kafka_AdminOptions_t *options,
                         rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        size_t i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_DeleteAclsRequest, rd_kafka_DeleteAclsResponse_parse};

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_DELETEACLS,
                                            RD_KAFKA_EVENT_DELETEACLS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)del_acls_cnt,
                     rd_kafka_AclBinding_free);

        for (i = 0; i < del_acls_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_kafka_AclBindingFilter_copy(del_acls[i]));

        rd_kafka_q_enq(rk->rk_ops, rko);
}

/**@}*/

/**
 * @name Alter consumer group offsets (committed offsets)
 * @{
 *
 *
 *
 *
 */

rd_kafka_AlterConsumerGroupOffsets_t *rd_kafka_AlterConsumerGroupOffsets_new(
    const char *group_id,
    const rd_kafka_topic_partition_list_t *partitions) {
        rd_assert(group_id && partitions);

        size_t tsize = strlen(group_id) + 1;
        rd_kafka_AlterConsumerGroupOffsets_t *alter_grpoffsets;

        /* Single allocation */
        alter_grpoffsets = rd_malloc(sizeof(*alter_grpoffsets) + tsize);
        alter_grpoffsets->group_id = alter_grpoffsets->data;
        memcpy(alter_grpoffsets->group_id, group_id, tsize);
        alter_grpoffsets->partitions =
            rd_kafka_topic_partition_list_copy(partitions);

        return alter_grpoffsets;
}

void rd_kafka_AlterConsumerGroupOffsets_destroy(
    rd_kafka_AlterConsumerGroupOffsets_t *alter_grpoffsets) {
        rd_kafka_topic_partition_list_destroy(alter_grpoffsets->partitions);
        rd_free(alter_grpoffsets);
}

static void rd_kafka_AlterConsumerGroupOffsets_free(void *ptr) {
        rd_kafka_AlterConsumerGroupOffsets_destroy(ptr);
}

void rd_kafka_AlterConsumerGroupOffsets_destroy_array(
    rd_kafka_AlterConsumerGroupOffsets_t **alter_grpoffsets,
    size_t alter_grpoffsets_cnt) {
        size_t i;
        for (i = 0; i < alter_grpoffsets_cnt; i++)
                rd_kafka_AlterConsumerGroupOffsets_destroy(alter_grpoffsets[i]);
}

/**
 * @brief Allocate a new AlterGroup and make a copy of \p src
 */
static rd_kafka_AlterConsumerGroupOffsets_t *
rd_kafka_AlterConsumerGroupOffsets_copy(
    const rd_kafka_AlterConsumerGroupOffsets_t *src) {
        return rd_kafka_AlterConsumerGroupOffsets_new(src->group_id,
                                                      src->partitions);
}

/**
 * @brief Send a OffsetCommitRequest to \p rkb with the partitions
 *        in alter_grpoffsets (AlterConsumerGroupOffsets_t*) using
 *        \p options.
 *
 */
static rd_kafka_resp_err_t rd_kafka_AlterConsumerGroupOffsetsRequest(
    rd_kafka_broker_t *rkb,
    /* (rd_kafka_AlterConsumerGroupOffsets_t*) */
    const rd_list_t *alter_grpoffsets,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        const rd_kafka_AlterConsumerGroupOffsets_t *grpoffsets =
            rd_list_elem(alter_grpoffsets, 0);

        rd_assert(rd_list_cnt(alter_grpoffsets) == 1);

        rd_kafka_topic_partition_list_t *offsets = grpoffsets->partitions;
        rd_kafka_consumer_group_metadata_t *cgmetadata =
            rd_kafka_consumer_group_metadata_new(grpoffsets->group_id);

        int ret = rd_kafka_OffsetCommitRequest(
            rkb, cgmetadata, offsets, replyq, resp_cb, opaque,
            "rd_kafka_AlterConsumerGroupOffsetsRequest");
        rd_kafka_consumer_group_metadata_destroy(cgmetadata);
        if (ret == 0) {
                rd_snprintf(errstr, errstr_size,
                            "At least one topic-partition offset must "
                            "be >= 0");
                return RD_KAFKA_RESP_ERR__NO_OFFSET;
        }
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse OffsetCommitResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_AlterConsumerGroupOffsetsResponse_parse(rd_kafka_op_t *rko_req,
                                                 rd_kafka_op_t **rko_resultp,
                                                 rd_kafka_buf_t *reply,
                                                 char *errstr,
                                                 size_t errstr_size) {
        rd_kafka_t *rk;
        rd_kafka_broker_t *rkb;
        rd_kafka_op_t *rko_result;
        rd_kafka_topic_partition_list_t *partitions = NULL;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        const rd_kafka_AlterConsumerGroupOffsets_t *alter_grpoffsets =
            rd_list_elem(&rko_req->rko_u.admin_request.args, 0);
        partitions =
            rd_kafka_topic_partition_list_copy(alter_grpoffsets->partitions);

        rk  = rko_req->rko_rk;
        rkb = reply->rkbuf_rkb;
        err = rd_kafka_handle_OffsetCommit(rk, rkb, err, reply, NULL,
                                           partitions, rd_true);

        /* Create result op and group_result_t */
        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_group_result_free);
        rd_list_add(&rko_result->rko_u.admin_result.results,
                    rd_kafka_group_result_new(alter_grpoffsets->group_id, -1,
                                              partitions, NULL));
        rd_kafka_topic_partition_list_destroy(partitions);
        *rko_resultp = rko_result;

        if (reply->rkbuf_err)
                rd_snprintf(
                    errstr, errstr_size,
                    "AlterConsumerGroupOffset response parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

void rd_kafka_AlterConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_AlterConsumerGroupOffsets_t **alter_grpoffsets,
    size_t alter_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu) {
        int i;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_AlterConsumerGroupOffsetsRequest,
            rd_kafka_AlterConsumerGroupOffsetsResponse_parse,
        };
        rd_kafka_op_t *rko;
        rd_kafka_topic_partition_list_t *copied_offsets;

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS,
            RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        if (alter_grpoffsets_cnt != 1) {
                /* For simplicity we only support one single group for now */
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Exactly one "
                                           "AlterConsumerGroupOffsets must "
                                           "be passed");
                goto fail;
        }

        if (alter_grpoffsets[0]->partitions->cnt == 0) {
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Non-empty topic partition list "
                                           "must be present");
                goto fail;
        }

        for (i = 0; i < alter_grpoffsets[0]->partitions->cnt; i++) {
                if (alter_grpoffsets[0]->partitions->elems[i].offset < 0) {
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "All topic-partition offsets "
                            "must be >= 0");
                        goto fail;
                }
        }

        /* TODO: add group id duplication check if in future more than one
         * AlterConsumerGroupOffsets can be passed */

        /* Copy offsets list for checking duplicated */
        copied_offsets =
            rd_kafka_topic_partition_list_copy(alter_grpoffsets[0]->partitions);
        if (rd_kafka_topic_partition_list_has_duplicates(
                copied_offsets, rd_false /*check partition*/)) {
                rd_kafka_topic_partition_list_destroy(copied_offsets);
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Duplicate partitions not allowed");
                goto fail;
        }
        rd_kafka_topic_partition_list_destroy(copied_offsets);

        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_COORDINATOR;
        rko->rko_u.admin_request.coordtype = RD_KAFKA_COORD_GROUP;
        rko->rko_u.admin_request.coordkey =
            rd_strdup(alter_grpoffsets[0]->group_id);

        /* Store copy of group on request so the group name can be reached
         * from the response parser. */
        rd_list_init(&rko->rko_u.admin_request.args, 1,
                     rd_kafka_AlterConsumerGroupOffsets_free);
        rd_list_add(&rko->rko_u.admin_request.args,
                    (void *)rd_kafka_AlterConsumerGroupOffsets_copy(
                        alter_grpoffsets[0]));

        rd_kafka_q_enq(rk->rk_ops, rko);
        return;
fail:
        rd_kafka_admin_common_worker_destroy(rk, rko, rd_true /*destroy*/);
}

/**
 * @brief Get an array of group results from a AlterGroups result.
 *
 * The returned \p groups life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_group_result_t **
rd_kafka_AlterConsumerGroupOffsets_result_groups(
    const rd_kafka_AlterConsumerGroupOffsets_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_groups((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/


/**@}*/

/**
 * @name List consumer group offsets (committed offsets)
 * @{
 *
 *
 *
 *
 */

rd_kafka_ListConsumerGroupOffsets_t *rd_kafka_ListConsumerGroupOffsets_new(
    const char *group_id,
    const rd_kafka_topic_partition_list_t *partitions) {
        size_t tsize = strlen(group_id) + 1;
        rd_kafka_ListConsumerGroupOffsets_t *list_grpoffsets;

        rd_assert(group_id);

        /* Single allocation */
        list_grpoffsets = rd_calloc(1, sizeof(*list_grpoffsets) + tsize);
        list_grpoffsets->group_id = list_grpoffsets->data;
        memcpy(list_grpoffsets->group_id, group_id, tsize);
        if (partitions) {
                list_grpoffsets->partitions =
                    rd_kafka_topic_partition_list_copy(partitions);
        }

        return list_grpoffsets;
}

void rd_kafka_ListConsumerGroupOffsets_destroy(
    rd_kafka_ListConsumerGroupOffsets_t *list_grpoffsets) {
        if (list_grpoffsets->partitions != NULL) {
                rd_kafka_topic_partition_list_destroy(
                    list_grpoffsets->partitions);
        }
        rd_free(list_grpoffsets);
}

static void rd_kafka_ListConsumerGroupOffsets_free(void *ptr) {
        rd_kafka_ListConsumerGroupOffsets_destroy(ptr);
}

void rd_kafka_ListConsumerGroupOffsets_destroy_array(
    rd_kafka_ListConsumerGroupOffsets_t **list_grpoffsets,
    size_t list_grpoffsets_cnt) {
        size_t i;
        for (i = 0; i < list_grpoffsets_cnt; i++)
                rd_kafka_ListConsumerGroupOffsets_destroy(list_grpoffsets[i]);
}

/**
 * @brief Allocate a new ListGroup and make a copy of \p src
 */
static rd_kafka_ListConsumerGroupOffsets_t *
rd_kafka_ListConsumerGroupOffsets_copy(
    const rd_kafka_ListConsumerGroupOffsets_t *src) {
        return rd_kafka_ListConsumerGroupOffsets_new(src->group_id,
                                                     src->partitions);
}

/**
 * @brief Send a OffsetFetchRequest to \p rkb with the partitions
 *        in list_grpoffsets (ListConsumerGroupOffsets_t*) using
 *        \p options.
 *
 */
static rd_kafka_resp_err_t rd_kafka_ListConsumerGroupOffsetsRequest(
    rd_kafka_broker_t *rkb,
    /* (rd_kafka_ListConsumerGroupOffsets_t*) */
    const rd_list_t *list_grpoffsets,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        int op_timeout;
        rd_bool_t require_stable_offsets;
        const rd_kafka_ListConsumerGroupOffsets_t *grpoffsets =
            rd_list_elem(list_grpoffsets, 0);

        rd_assert(rd_list_cnt(list_grpoffsets) == 1);

        op_timeout = rd_kafka_confval_get_int(&options->request_timeout);
        require_stable_offsets =
            rd_kafka_confval_get_int(&options->require_stable_offsets);
        rd_kafka_OffsetFetchRequest(
            rkb, grpoffsets->group_id, grpoffsets->partitions, rd_false, -1,
            NULL, require_stable_offsets, op_timeout, replyq, resp_cb, opaque);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse OffsetFetchResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_ListConsumerGroupOffsetsResponse_parse(rd_kafka_op_t *rko_req,
                                                rd_kafka_op_t **rko_resultp,
                                                rd_kafka_buf_t *reply,
                                                char *errstr,
                                                size_t errstr_size) {
        const rd_kafka_ListConsumerGroupOffsets_t *list_grpoffsets =
            rd_list_elem(&rko_req->rko_u.admin_request.args, 0);
        rd_kafka_t *rk;
        rd_kafka_broker_t *rkb;
        rd_kafka_topic_partition_list_t *offsets = NULL;
        rd_kafka_op_t *rko_result;
        rd_kafka_resp_err_t err;

        rk  = rko_req->rko_rk;
        rkb = reply->rkbuf_rkb;
        err = rd_kafka_handle_OffsetFetch(rk, rkb, RD_KAFKA_RESP_ERR_NO_ERROR,
                                          reply, NULL, &offsets, rd_false,
                                          rd_true, rd_false);

        if (unlikely(err != RD_KAFKA_RESP_ERR_NO_ERROR)) {
                reply->rkbuf_err = err;
                goto err;
        }

        /* Create result op and group_result_t */
        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_group_result_free);
        rd_list_add(&rko_result->rko_u.admin_result.results,
                    rd_kafka_group_result_new(list_grpoffsets->group_id, -1,
                                              offsets, NULL));

        if (likely(offsets != NULL))
                rd_kafka_topic_partition_list_destroy(offsets);

        *rko_resultp = rko_result;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
err:
        if (likely(offsets != NULL))
                rd_kafka_topic_partition_list_destroy(offsets);

        rd_snprintf(errstr, errstr_size,
                    "ListConsumerGroupOffsetsResponse response failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

void rd_kafka_ListConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_ListConsumerGroupOffsets_t **list_grpoffsets,
    size_t list_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu) {
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_ListConsumerGroupOffsetsRequest,
            rd_kafka_ListConsumerGroupOffsetsResponse_parse,
        };
        rd_kafka_op_t *rko;
        rd_kafka_topic_partition_list_t *copied_offsets;

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS,
            RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT, &cbs, options,
            rkqu->rkqu_q);

        if (list_grpoffsets_cnt != 1) {
                /* For simplicity we only support one single group for now */
                rd_kafka_admin_result_fail(rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Exactly one "
                                           "ListConsumerGroupOffsets must "
                                           "be passed");
                goto fail;
        }

        if (list_grpoffsets[0]->partitions != NULL &&
            list_grpoffsets[0]->partitions->cnt == 0) {
                /* Either pass NULL for all the partitions or a non-empty list
                 */
                rd_kafka_admin_result_fail(
                    rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "NULL or "
                    "non-empty topic partition list must "
                    "be passed");
                goto fail;
        }

        /* TODO: add group id duplication check when implementing KIP-709 */
        if (list_grpoffsets[0]->partitions != NULL) {
                /* Copy offsets list for checking duplicated */
                copied_offsets = rd_kafka_topic_partition_list_copy(
                    list_grpoffsets[0]->partitions);
                if (rd_kafka_topic_partition_list_has_duplicates(
                        copied_offsets, rd_false /*check partition*/)) {
                        rd_kafka_topic_partition_list_destroy(copied_offsets);
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate partitions not allowed");
                        goto fail;
                }
                rd_kafka_topic_partition_list_destroy(copied_offsets);
        }

        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_COORDINATOR;
        rko->rko_u.admin_request.coordtype = RD_KAFKA_COORD_GROUP;
        rko->rko_u.admin_request.coordkey =
            rd_strdup(list_grpoffsets[0]->group_id);

        /* Store copy of group on request so the group name can be reached
         * from the response parser. */
        rd_list_init(&rko->rko_u.admin_request.args, 1,
                     rd_kafka_ListConsumerGroupOffsets_free);
        rd_list_add(&rko->rko_u.admin_request.args,
                    rd_kafka_ListConsumerGroupOffsets_copy(list_grpoffsets[0]));

        rd_kafka_q_enq(rk->rk_ops, rko);
        return;
fail:
        rd_kafka_admin_common_worker_destroy(rk, rko, rd_true /*destroy*/);
}


/**
 * @brief Get an array of group results from a ListConsumerGroups result.
 *
 * The returned \p groups life-time is the same as the \p result object.
 * @param cntp is updated to the number of elements in the array.
 */
const rd_kafka_group_result_t **rd_kafka_ListConsumerGroupOffsets_result_groups(
    const rd_kafka_ListConsumerGroupOffsets_result_t *result,
    size_t *cntp) {
        return rd_kafka_admin_result_ret_groups((const rd_kafka_op_t *)result,
                                                cntp);
}

/**@}*/

/**
 * @name List consumer groups
 * @{
 *
 *
 *
 *
 */

#define CONSUMER_PROTOCOL_TYPE "consumer"

/**
 * @brief Create a new ConsumerGroupListing object.
 *
 * @param group_id The group id.
 * @param is_simple_consumer_group Is the group simple?
 * @param state Group state.
 */
static rd_kafka_ConsumerGroupListing_t *
rd_kafka_ConsumerGroupListing_new(const char *group_id,
                                  rd_bool_t is_simple_consumer_group,
                                  rd_kafka_consumer_group_state_t state,
                                  rd_kafka_consumer_group_type_t type) {
        rd_kafka_ConsumerGroupListing_t *grplist;
        grplist                           = rd_calloc(1, sizeof(*grplist));
        grplist->group_id                 = rd_strdup(group_id);
        grplist->is_simple_consumer_group = is_simple_consumer_group;
        grplist->state                    = state;
        grplist->type                     = type;
        return grplist;
}

/**
 * @brief Copy \p grplist ConsumerGroupListing.
 *
 * @param grplist The group listing to copy.
 * @return A new allocated copy of the passed ConsumerGroupListing.
 */
static rd_kafka_ConsumerGroupListing_t *rd_kafka_ConsumerGroupListing_copy(
    const rd_kafka_ConsumerGroupListing_t *grplist) {
        return rd_kafka_ConsumerGroupListing_new(
            grplist->group_id, grplist->is_simple_consumer_group,
            grplist->state, grplist->type);
}

/**
 * @brief Same as rd_kafka_ConsumerGroupListing_copy() but suitable for
 *        rd_list_copy(). The \p opaque is ignored.
 */
static void *rd_kafka_ConsumerGroupListing_copy_opaque(const void *grplist,
                                                       void *opaque) {
        return rd_kafka_ConsumerGroupListing_copy(grplist);
}

static void rd_kafka_ConsumerGroupListing_destroy(
    rd_kafka_ConsumerGroupListing_t *grplist) {
        RD_IF_FREE(grplist->group_id, rd_free);
        rd_free(grplist);
}

static void rd_kafka_ConsumerGroupListing_free(void *ptr) {
        rd_kafka_ConsumerGroupListing_destroy(ptr);
}

const char *rd_kafka_ConsumerGroupListing_group_id(
    const rd_kafka_ConsumerGroupListing_t *grplist) {
        return grplist->group_id;
}

int rd_kafka_ConsumerGroupListing_is_simple_consumer_group(
    const rd_kafka_ConsumerGroupListing_t *grplist) {
        return grplist->is_simple_consumer_group;
}

rd_kafka_consumer_group_state_t rd_kafka_ConsumerGroupListing_state(
    const rd_kafka_ConsumerGroupListing_t *grplist) {
        return grplist->state;
}

rd_kafka_consumer_group_type_t rd_kafka_ConsumerGroupListing_type(
    const rd_kafka_ConsumerGroupListing_t *grplist) {
        return grplist->type;
}

/**
 * @brief Create a new ListConsumerGroupsResult object.
 *
 * @param valid
 * @param errors
 */
static rd_kafka_ListConsumerGroupsResult_t *
rd_kafka_ListConsumerGroupsResult_new(const rd_list_t *valid,
                                      const rd_list_t *errors) {
        rd_kafka_ListConsumerGroupsResult_t *res;
        res = rd_calloc(1, sizeof(*res));
        rd_list_init_copy(&res->valid, valid);
        rd_list_copy_to(&res->valid, valid,
                        rd_kafka_ConsumerGroupListing_copy_opaque, NULL);
        rd_list_init_copy(&res->errors, errors);
        rd_list_copy_to(&res->errors, errors, rd_kafka_error_copy_opaque, NULL);
        return res;
}

static void rd_kafka_ListConsumerGroupsResult_destroy(
    rd_kafka_ListConsumerGroupsResult_t *res) {
        rd_list_destroy(&res->valid);
        rd_list_destroy(&res->errors);
        rd_free(res);
}

static void rd_kafka_ListConsumerGroupsResult_free(void *ptr) {
        rd_kafka_ListConsumerGroupsResult_destroy(ptr);
}

/**
 * @brief Copy the passed ListConsumerGroupsResult.
 *
 * @param res the ListConsumerGroupsResult to copy
 * @return a newly allocated ListConsumerGroupsResult object.
 *
 * @sa Release the object with rd_kafka_ListConsumerGroupsResult_destroy().
 */
static rd_kafka_ListConsumerGroupsResult_t *
rd_kafka_ListConsumerGroupsResult_copy(
    const rd_kafka_ListConsumerGroupsResult_t *res) {
        return rd_kafka_ListConsumerGroupsResult_new(&res->valid, &res->errors);
}

/**
 * @brief Same as rd_kafka_ListConsumerGroupsResult_copy() but suitable for
 *        rd_list_copy(). The \p opaque is ignored.
 */
static void *rd_kafka_ListConsumerGroupsResult_copy_opaque(const void *list,
                                                           void *opaque) {
        return rd_kafka_ListConsumerGroupsResult_copy(list);
}

/**
 * @brief Send ListConsumerGroupsRequest. Admin worker compatible callback.
 */
static rd_kafka_resp_err_t
rd_kafka_admin_ListConsumerGroupsRequest(rd_kafka_broker_t *rkb,
                                         const rd_list_t *groups /*(char*)*/,
                                         rd_kafka_AdminOptions_t *options,
                                         char *errstr,
                                         size_t errstr_size,
                                         rd_kafka_replyq_t replyq,
                                         rd_kafka_resp_cb_t *resp_cb,
                                         void *opaque) {
        int i;
        rd_kafka_resp_err_t err;
        rd_kafka_error_t *error;
        const char **states_str = NULL;
        const char **types_str  = NULL;
        int states_str_cnt      = 0;
        rd_list_t *states =
            rd_kafka_confval_get_ptr(&options->match_consumer_group_states);
        int types_str_cnt = 0;
        rd_list_t *types =
            rd_kafka_confval_get_ptr(&options->match_consumer_group_types);


        /* Prepare list_options for consumer group state */
        if (states && rd_list_cnt(states) > 0) {
                states_str_cnt = rd_list_cnt(states);
                states_str     = rd_calloc(states_str_cnt, sizeof(*states_str));
                for (i = 0; i < states_str_cnt; i++) {
                        states_str[i] = rd_kafka_consumer_group_state_name(
                            rd_list_get_int32(states, i));
                }
        }

        /* Prepare list_options for consumer group type */
        if (types && rd_list_cnt(types) > 0) {
                types_str_cnt = rd_list_cnt(types);
                types_str     = rd_calloc(types_str_cnt, sizeof(*types_str));
                for (i = 0; i < types_str_cnt; i++) {
                        types_str[i] = rd_kafka_consumer_group_type_name(
                            rd_list_get_int32(types, i));
                }
        }
        error = rd_kafka_ListGroupsRequest(rkb, -1, states_str, states_str_cnt,
                                           types_str, types_str_cnt, replyq,
                                           resp_cb, opaque);

        if (states_str) {
                rd_free(states_str);
        }

        if (types_str) {
                rd_free(types_str);
        }

        if (error) {
                rd_snprintf(errstr, errstr_size, "%s",
                            rd_kafka_error_string(error));
                err = rd_kafka_error_code(error);
                rd_kafka_error_destroy(error);
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse ListConsumerGroupsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_ListConsumerGroupsResponse_parse(rd_kafka_op_t *rko_req,
                                          rd_kafka_op_t **rko_resultp,
                                          rd_kafka_buf_t *reply,
                                          char *errstr,
                                          size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        int i, cnt;
        int16_t error_code, api_version;
        rd_kafka_op_t *rko_result = NULL;
        rd_kafka_error_t *error   = NULL;
        rd_kafka_broker_t *rkb    = reply->rkbuf_rkb;
        rd_list_t valid, errors;
        rd_kafka_ListConsumerGroupsResult_t *list_result;
        char *group_id = NULL, *group_state = NULL, *proto_type = NULL,
             *group_type_str = NULL;

        api_version = rd_kafka_buf_ApiVersion(reply);
        if (api_version >= 1) {
                rd_kafka_buf_read_throttle_time(reply);
        }
        rd_kafka_buf_read_i16(reply, &error_code);
        if (error_code) {
                error = rd_kafka_error_new(error_code,
                                           "Broker [%d"
                                           "] "
                                           "ListConsumerGroups: %s",
                                           rd_kafka_broker_id(rkb),
                                           rd_kafka_err2str(error_code));
        }

        rd_kafka_buf_read_arraycnt(reply, &cnt, RD_KAFKAP_GROUPS_MAX);
        rd_list_init(&valid, cnt, rd_kafka_ConsumerGroupListing_free);
        rd_list_init(&errors, 8, rd_free);
        if (error)
                rd_list_add(&errors, error);

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_ListConsumerGroupsResult_free);

        for (i = 0; i < cnt; i++) {
                rd_kafkap_str_t GroupId, ProtocolType,
                    GroupState = RD_ZERO_INIT, GroupType = RD_ZERO_INIT;
                rd_kafka_ConsumerGroupListing_t *group_listing;
                rd_bool_t is_simple_consumer_group, is_consumer_protocol_type;
                rd_kafka_consumer_group_state_t state =
                    RD_KAFKA_CONSUMER_GROUP_STATE_UNKNOWN;
                rd_kafka_consumer_group_type_t type =
                    RD_KAFKA_CONSUMER_GROUP_TYPE_UNKNOWN;

                rd_kafka_buf_read_str(reply, &GroupId);
                rd_kafka_buf_read_str(reply, &ProtocolType);
                if (api_version >= 4) {
                        rd_kafka_buf_read_str(reply, &GroupState);
                }
                if (api_version >= 5) {
                        rd_kafka_buf_read_str(reply, &GroupType);
                }
                rd_kafka_buf_skip_tags(reply);

                group_id   = RD_KAFKAP_STR_DUP(&GroupId);
                proto_type = RD_KAFKAP_STR_DUP(&ProtocolType);
                if (api_version >= 4) {
                        group_state = RD_KAFKAP_STR_DUP(&GroupState);
                        state = rd_kafka_consumer_group_state_code(group_state);
                }

                if (api_version >= 5) {
                        group_type_str = RD_KAFKAP_STR_DUP(&GroupType);
                        type =
                            rd_kafka_consumer_group_type_code(group_type_str);
                }

                is_simple_consumer_group = *proto_type == '\0';
                is_consumer_protocol_type =
                    !strcmp(proto_type, CONSUMER_PROTOCOL_TYPE);
                if (is_simple_consumer_group || is_consumer_protocol_type) {
                        group_listing = rd_kafka_ConsumerGroupListing_new(
                            group_id, is_simple_consumer_group, state, type);
                        rd_list_add(&valid, group_listing);
                }

                rd_free(group_id);
                rd_free(group_state);
                rd_free(proto_type);
                rd_free(group_type_str);
                group_id       = NULL;
                group_state    = NULL;
                proto_type     = NULL;
                group_type_str = NULL;
        }
        rd_kafka_buf_skip_tags(reply);

err_parse:
        if (group_id)
                rd_free(group_id);
        if (group_state)
                rd_free(group_state);
        if (proto_type)
                rd_free(proto_type);
        if (group_type_str)
                rd_free(group_type_str);

        if (reply->rkbuf_err) {
                error_code = reply->rkbuf_err;
                error      = rd_kafka_error_new(
                    error_code,
                    "Broker [%d"
                         "] "
                         "ListConsumerGroups response protocol parse failure: %s",
                    rd_kafka_broker_id(rkb), rd_kafka_err2str(error_code));
                rd_list_add(&errors, error);
        }

        list_result = rd_kafka_ListConsumerGroupsResult_new(&valid, &errors);
        rd_list_add(&rko_result->rko_u.admin_result.results, list_result);

        *rko_resultp = rko_result;
        rd_list_destroy(&valid);
        rd_list_destroy(&errors);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/** @brief Merge the ListConsumerGroups response from a single broker
 *         into the user response list.
 */
static void
rd_kafka_ListConsumerGroups_response_merge(rd_kafka_op_t *rko_fanout,
                                           const rd_kafka_op_t *rko_partial) {
        int cnt;
        rd_kafka_ListConsumerGroupsResult_t *res = NULL;
        rd_kafka_ListConsumerGroupsResult_t *newres;
        rd_list_t new_valid, new_errors;

        rd_assert(rko_partial->rko_evtype ==
                  RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT);

        cnt = rd_list_cnt(&rko_fanout->rko_u.admin_request.fanout.results);
        if (cnt) {
                res = rd_list_elem(
                    &rko_fanout->rko_u.admin_request.fanout.results, 0);
        } else {
                rd_list_init(&new_valid, 0, rd_kafka_ConsumerGroupListing_free);
                rd_list_init(&new_errors, 0, rd_free);
                res = rd_kafka_ListConsumerGroupsResult_new(&new_valid,
                                                            &new_errors);
                rd_list_set(&rko_fanout->rko_u.admin_request.fanout.results, 0,
                            res);
                rd_list_destroy(&new_valid);
                rd_list_destroy(&new_errors);
        }
        if (!rko_partial->rko_err) {
                int new_valid_count, new_errors_count;
                const rd_list_t *new_valid_list, *new_errors_list;
                /* Read the partial result and merge the valid groups
                 * and the errors into the fanout parent result. */
                newres =
                    rd_list_elem(&rko_partial->rko_u.admin_result.results, 0);
                rd_assert(newres);
                new_valid_count  = rd_list_cnt(&newres->valid);
                new_errors_count = rd_list_cnt(&newres->errors);
                if (new_valid_count) {
                        new_valid_list = &newres->valid;
                        rd_list_grow(&res->valid, new_valid_count);
                        rd_list_copy_to(
                            &res->valid, new_valid_list,
                            rd_kafka_ConsumerGroupListing_copy_opaque, NULL);
                }
                if (new_errors_count) {
                        new_errors_list = &newres->errors;
                        rd_list_grow(&res->errors, new_errors_count);
                        rd_list_copy_to(&res->errors, new_errors_list,
                                        rd_kafka_error_copy_opaque, NULL);
                }
        } else {
                /* Op errored, e.g. timeout */
                rd_list_add(&res->errors,
                            rd_kafka_error_new(rko_partial->rko_err, NULL));
        }
}

void rd_kafka_ListConsumerGroups(rd_kafka_t *rk,
                                 const rd_kafka_AdminOptions_t *options,
                                 rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_admin_ListConsumerGroupsRequest,
            rd_kafka_ListConsumerGroupsResponse_parse};
        static const struct rd_kafka_admin_fanout_worker_cbs fanout_cbs = {
            rd_kafka_ListConsumerGroups_response_merge,
            rd_kafka_ListConsumerGroupsResult_copy_opaque,
        };

        rko = rd_kafka_admin_request_op_target_all_new(
            rk, RD_KAFKA_OP_LISTCONSUMERGROUPS,
            RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT, &cbs, &fanout_cbs,
            rd_kafka_ListConsumerGroupsResult_free, options, rkqu->rkqu_q);
        rd_kafka_q_enq(rk->rk_ops, rko);
}

const rd_kafka_ConsumerGroupListing_t **
rd_kafka_ListConsumerGroups_result_valid(
    const rd_kafka_ListConsumerGroups_result_t *result,
    size_t *cntp) {
        int list_result_cnt;
        const rd_kafka_ListConsumerGroupsResult_t *list_result;
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_LISTCONSUMERGROUPS);

        list_result_cnt = rd_list_cnt(&rko->rko_u.admin_result.results);
        rd_assert(list_result_cnt == 1);
        list_result = rd_list_elem(&rko->rko_u.admin_result.results, 0);
        *cntp       = rd_list_cnt(&list_result->valid);

        return (const rd_kafka_ConsumerGroupListing_t **)
            list_result->valid.rl_elems;
}

const rd_kafka_error_t **rd_kafka_ListConsumerGroups_result_errors(
    const rd_kafka_ListConsumerGroups_result_t *result,
    size_t *cntp) {
        int list_result_cnt, error_cnt;
        const rd_kafka_ListConsumerGroupsResult_t *list_result;
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_LISTCONSUMERGROUPS);

        list_result_cnt = rd_list_cnt(&rko->rko_u.admin_result.results);
        rd_assert(list_result_cnt == 1);
        list_result = rko->rko_u.admin_result.results.rl_elems[0];
        error_cnt   = rd_list_cnt(&list_result->errors);
        if (error_cnt == 0) {
                *cntp = 0;
                return NULL;
        }
        *cntp = error_cnt;
        return (const rd_kafka_error_t **)list_result->errors.rl_elems;
}

/**@}*/

/**
 * @name Describe consumer groups
 * @{
 *
 *
 *
 *
 */

/**
 * @brief Parse authorized_operations returned in
 * - DescribeConsumerGroups
 * - DescribeTopics
 * - DescribeCluster
 *
 * @param authorized_operations returned by RPC, containing operations encoded
 *                              per-bit.
 * @param cntp is set to the count of the operations, or -1 if the operations
 *        were not requested.
 * @returns rd_kafka_AclOperation_t *. May be NULL.
 */
static rd_kafka_AclOperation_t *
rd_kafka_AuthorizedOperations_parse(int32_t authorized_operations, int *cntp) {
        rd_kafka_AclOperation_t i;
        int j                               = 0;
        int count                           = 0;
        rd_kafka_AclOperation_t *operations = NULL;

        /* In case of authorized_operations not requested, return NULL. */
        if (authorized_operations < 0) {
                *cntp = -1;
                return NULL;
        }

        /* Count number of bits set. ALL, ANY and UNKNOWN bits are skipped as
         * they are always unset as per KIP-430. */
        for (i = RD_KAFKA_ACL_OPERATION_READ; i < RD_KAFKA_ACL_OPERATION__CNT;
             i++)
                count += ((authorized_operations >> i) & 1);
        *cntp = count;

        /* In case no operations exist, allocate 1 byte so that the returned
         * pointer is non-NULL. A NULL pointer implies that authorized
         * operations were not requested. */
        if (count == 0)
                return rd_malloc(1);

        operations = rd_malloc(sizeof(rd_kafka_AclOperation_t) * count);
        j          = 0;
        for (i = RD_KAFKA_ACL_OPERATION_READ; i < RD_KAFKA_ACL_OPERATION__CNT;
             i++) {
                if ((authorized_operations >> i) & 1) {
                        operations[j] = i;
                        j++;
                }
        }

        return operations;
}

/**
 * @brief Copy a list of rd_kafka_AclOperation_t.
 *
 * @param src Array of rd_kafka_AclOperation_t to copy from. May be NULL if
 *            authorized operations were not requested.
 * @param authorized_operations_cnt Count of \p src. May be -1 if authorized
 *                                  operations were not requested.
 * @returns Copy of \p src. May be NULL.
 */
static rd_kafka_AclOperation_t *
rd_kafka_AuthorizedOperations_copy(const rd_kafka_AclOperation_t *src,
                                   int authorized_operations_cnt) {
        size_t copy_bytes            = 0;
        rd_kafka_AclOperation_t *dst = NULL;

        if (authorized_operations_cnt == -1 || src == NULL)
                return NULL;

        /* Allocate and copy 1 byte so that the returned pointer
         * is non-NULL. A NULL pointer implies that authorized operations were
         * not requested. */
        if (authorized_operations_cnt == 0)
                copy_bytes = 1;
        else
                copy_bytes =
                    sizeof(rd_kafka_AclOperation_t) * authorized_operations_cnt;

        dst = rd_malloc(copy_bytes);
        memcpy(dst, src, copy_bytes);
        return dst;
}

/**
 * @brief Create a new MemberDescription object. This object is used for
 *        creating a ConsumerGroupDescription.
 *
 * @param client_id The client id.
 * @param consumer_id The consumer id (or member id).
 * @param group_instance_id (optional) The group instance id
 *                          for static membership.
 * @param host The consumer host.
 * @param assignment The member's assigned partitions, or NULL if none.
 *
 * @return A new allocated MemberDescription object.
 *         Use rd_kafka_MemberDescription_destroy() to free when done.
 */
static rd_kafka_MemberDescription_t *rd_kafka_MemberDescription_new(
    const char *client_id,
    const char *consumer_id,
    const char *group_instance_id,
    const char *host,
    const rd_kafka_topic_partition_list_t *assignment,
    const rd_kafka_topic_partition_list_t *target_assignment) {
        rd_kafka_MemberDescription_t *member;
        member              = rd_calloc(1, sizeof(*member));
        member->client_id   = rd_strdup(client_id);
        member->consumer_id = rd_strdup(consumer_id);
        if (group_instance_id)
                member->group_instance_id = rd_strdup(group_instance_id);
        member->host = rd_strdup(host);
        if (assignment)
                member->assignment.partitions =
                    rd_kafka_topic_partition_list_copy(assignment);
        else
                member->assignment.partitions =
                    rd_kafka_topic_partition_list_new(0);
        if (target_assignment) {
                member->target_assignment =
                    rd_calloc(1, sizeof(rd_kafka_MemberAssignment_t));
                member->target_assignment->partitions =
                    rd_kafka_topic_partition_list_copy(target_assignment);
        }
        return member;
}

/**
 * @brief Allocate a new MemberDescription, copy of \p src
 *        and return it.
 *
 * @param src The MemberDescription to copy.
 * @return A new allocated MemberDescription object,
 *         Use rd_kafka_MemberDescription_destroy() to free when done.
 */
static rd_kafka_MemberDescription_t *
rd_kafka_MemberDescription_copy(const rd_kafka_MemberDescription_t *src) {
        return rd_kafka_MemberDescription_new(
            src->client_id, src->consumer_id, src->group_instance_id, src->host,
            src->assignment.partitions,
            src->target_assignment ? src->target_assignment->partitions : NULL);
}

/**
 * @brief MemberDescription copy, compatible with rd_list_copy_to.
 *
 * @param elem The MemberDescription to copy-
 * @param opaque Not used.
 */
static void *rd_kafka_MemberDescription_list_copy(const void *elem,
                                                  void *opaque) {
        return rd_kafka_MemberDescription_copy(elem);
}

static void
rd_kafka_MemberDescription_destroy(rd_kafka_MemberDescription_t *member) {
        rd_free(member->client_id);
        rd_free(member->consumer_id);
        rd_free(member->host);
        RD_IF_FREE(member->group_instance_id, rd_free);
        RD_IF_FREE(member->assignment.partitions,
                   rd_kafka_topic_partition_list_destroy);
        if (member->target_assignment) {
                RD_IF_FREE(member->target_assignment->partitions,
                           rd_kafka_topic_partition_list_destroy);
                rd_free(member->target_assignment);
        }
        rd_free(member);
}

static void rd_kafka_MemberDescription_free(void *member) {
        rd_kafka_MemberDescription_destroy(member);
}

const char *rd_kafka_MemberDescription_client_id(
    const rd_kafka_MemberDescription_t *member) {
        return member->client_id;
}

const char *rd_kafka_MemberDescription_group_instance_id(
    const rd_kafka_MemberDescription_t *member) {
        return member->group_instance_id;
}

const char *rd_kafka_MemberDescription_consumer_id(
    const rd_kafka_MemberDescription_t *member) {
        return member->consumer_id;
}

const char *
rd_kafka_MemberDescription_host(const rd_kafka_MemberDescription_t *member) {
        return member->host;
}

const rd_kafka_MemberAssignment_t *rd_kafka_MemberDescription_assignment(
    const rd_kafka_MemberDescription_t *member) {
        return &member->assignment;
}

const rd_kafka_topic_partition_list_t *rd_kafka_MemberAssignment_partitions(
    const rd_kafka_MemberAssignment_t *assignment) {
        return assignment->partitions;
}

const rd_kafka_MemberAssignment_t *rd_kafka_MemberDescription_target_assignment(
    const rd_kafka_MemberDescription_t *member) {
        return member->target_assignment;
}


/**
 * @brief Create a new ConsumerGroupDescription object.
 *
 * @param group_id The group id.
 * @param is_simple_consumer_group Is the group simple?
 * @param members List of members (rd_kafka_MemberDescription_t) of this
 *                group.
 * @param partition_assignor (optional) Chosen assignor.
 * @param authorized_operations (optional) authorized operations.
 * @param state Group state.
 * @param coordinator (optional) Group coordinator.
 * @param error (optional) Error received for this group.
 * @return A new allocated ConsumerGroupDescription object.
 *         Use rd_kafka_ConsumerGroupDescription_destroy() to free when done.
 */
static rd_kafka_ConsumerGroupDescription_t *
rd_kafka_ConsumerGroupDescription_new(
    const char *group_id,
    rd_bool_t is_simple_consumer_group,
    const rd_list_t *members,
    const char *partition_assignor,
    const rd_kafka_AclOperation_t *authorized_operations,
    int authorized_operations_cnt,
    rd_kafka_consumer_group_state_t state,
    rd_kafka_consumer_group_type_t type,
    const rd_kafka_Node_t *coordinator,
    rd_kafka_error_t *error) {
        rd_kafka_ConsumerGroupDescription_t *grpdesc;
        grpdesc                           = rd_calloc(1, sizeof(*grpdesc));
        grpdesc->group_id                 = rd_strdup(group_id);
        grpdesc->is_simple_consumer_group = is_simple_consumer_group;
        if (members == NULL) {
                rd_list_init(&grpdesc->members, 0,
                             rd_kafka_MemberDescription_free);
        } else {
                rd_list_init_copy(&grpdesc->members, members);
                rd_list_copy_to(&grpdesc->members, members,
                                rd_kafka_MemberDescription_list_copy, NULL);
        }
        grpdesc->partition_assignor = !partition_assignor
                                          ? (char *)partition_assignor
                                          : rd_strdup(partition_assignor);

        grpdesc->authorized_operations_cnt = authorized_operations_cnt;
        grpdesc->authorized_operations     = rd_kafka_AuthorizedOperations_copy(
            authorized_operations, authorized_operations_cnt);

        grpdesc->state = state;
        grpdesc->type  = type;
        if (coordinator != NULL)
                grpdesc->coordinator = rd_kafka_Node_copy(coordinator);
        grpdesc->error =
            error != NULL ? rd_kafka_error_new(rd_kafka_error_code(error), "%s",
                                               rd_kafka_error_string(error))
                          : NULL;
        return grpdesc;
}

/**
 * @brief New instance of ConsumerGroupDescription from an error.
 *
 * @param group_id The group id.
 * @param error Error received for this group.
 * @return A new allocated ConsumerGroupDescription with the passed error.
 *         Use rd_kafka_ConsumerGroupDescription_destroy() to free when done.
 */
static rd_kafka_ConsumerGroupDescription_t *
rd_kafka_ConsumerGroupDescription_new_error(const char *group_id,
                                            rd_kafka_error_t *error) {
        return rd_kafka_ConsumerGroupDescription_new(
            group_id, rd_false, NULL, NULL, NULL, 0,
            RD_KAFKA_CONSUMER_GROUP_STATE_UNKNOWN,
            RD_KAFKA_CONSUMER_GROUP_TYPE_UNKNOWN, NULL, error);
}

/**
 * @brief Copy \p desc ConsumerGroupDescription.
 *
 * @param desc The group description to copy.
 * @return A new allocated copy of the passed ConsumerGroupDescription.
 */
static rd_kafka_ConsumerGroupDescription_t *
rd_kafka_ConsumerGroupDescription_copy(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return rd_kafka_ConsumerGroupDescription_new(
            grpdesc->group_id, grpdesc->is_simple_consumer_group,
            &grpdesc->members, grpdesc->partition_assignor,
            grpdesc->authorized_operations, grpdesc->authorized_operations_cnt,
            grpdesc->state, grpdesc->type, grpdesc->coordinator,
            grpdesc->error);
}

/**
 * @brief Same as rd_kafka_ConsumerGroupDescription_copy() but suitable for
 *        rd_list_copy(). The \p opaque is ignored.
 */
static void *rd_kafka_ConsumerGroupDescription_copy_opaque(const void *grpdesc,
                                                           void *opaque) {
        return rd_kafka_ConsumerGroupDescription_copy(grpdesc);
}

static void rd_kafka_ConsumerGroupDescription_destroy(
    rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        if (likely(grpdesc->group_id != NULL))
                rd_free(grpdesc->group_id);
        rd_list_destroy(&grpdesc->members);
        if (likely(grpdesc->partition_assignor != NULL))
                rd_free(grpdesc->partition_assignor);
        if (likely(grpdesc->error != NULL))
                rd_kafka_error_destroy(grpdesc->error);
        if (grpdesc->coordinator)
                rd_kafka_Node_destroy(grpdesc->coordinator);
        if (grpdesc->authorized_operations_cnt)
                rd_free(grpdesc->authorized_operations);
        rd_free(grpdesc);
}

static void rd_kafka_ConsumerGroupDescription_free(void *ptr) {
        rd_kafka_ConsumerGroupDescription_destroy(ptr);
}

const char *rd_kafka_ConsumerGroupDescription_group_id(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->group_id;
}

const rd_kafka_error_t *rd_kafka_ConsumerGroupDescription_error(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->error;
}


int rd_kafka_ConsumerGroupDescription_is_simple_consumer_group(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->is_simple_consumer_group;
}


const char *rd_kafka_ConsumerGroupDescription_partition_assignor(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->partition_assignor;
}

const rd_kafka_AclOperation_t *
rd_kafka_ConsumerGroupDescription_authorized_operations(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc,
    size_t *cntp) {
        *cntp = RD_MAX(grpdesc->authorized_operations_cnt, 0);
        return grpdesc->authorized_operations;
}

rd_kafka_consumer_group_state_t rd_kafka_ConsumerGroupDescription_state(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->state;
}

const rd_kafka_Node_t *rd_kafka_ConsumerGroupDescription_coordinator(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->coordinator;
}

rd_kafka_consumer_group_type_t rd_kafka_ConsumerGroupDescription_type(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return grpdesc->type;
}

size_t rd_kafka_ConsumerGroupDescription_member_count(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc) {
        return rd_list_cnt(&grpdesc->members);
}

const rd_kafka_MemberDescription_t *rd_kafka_ConsumerGroupDescription_member(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc,
    size_t idx) {
        return (rd_kafka_MemberDescription_t *)rd_list_elem(&grpdesc->members,
                                                            idx);
}

/**
 * @brief Group arguments comparator for DescribeConsumerGroups args
 */
static int rd_kafka_DescribeConsumerGroups_cmp(const void *a, const void *b) {
        return strcmp(a, b);
}


/**
 * @brief Construct and send DescribeConsumerGroupsRequest to \p rkb
 *        with the groups (char *) in \p groups, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
static rd_kafka_resp_err_t rd_kafka_admin_DescribeConsumerGroupsRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *groups /*(char*)*/,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        int i, include_authorized_operations;
        char *group;
        rd_kafka_resp_err_t err;
        int groups_cnt          = rd_list_cnt(groups);
        rd_kafka_error_t *error = NULL;
        char **groups_arr       = rd_calloc(groups_cnt, sizeof(*groups_arr));

        RD_LIST_FOREACH(group, groups, i) {
                groups_arr[i] = rd_list_elem(groups, i);
        }

        include_authorized_operations =
            rd_kafka_confval_get_int(&options->include_authorized_operations);

        error = rd_kafka_DescribeGroupsRequest(rkb, -1, groups_arr, groups_cnt,
                                               include_authorized_operations,
                                               replyq, resp_cb, opaque);
        rd_free(groups_arr);

        if (error) {
                rd_snprintf(errstr, errstr_size, "%s",
                            rd_kafka_error_string(error));
                err = rd_kafka_error_code(error);
                rd_kafka_error_destroy(error);
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Construct and send ConsumerGroupDescribeRequest to \p rkb
 *        with the groups (char *) in \p groups, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
static rd_kafka_resp_err_t
rd_kafka_admin_ConsumerGroupDescribeRequest(rd_kafka_broker_t *rkb,
                                            const rd_list_t *groups /*(char*)*/,
                                            rd_kafka_AdminOptions_t *options,
                                            char *errstr,
                                            size_t errstr_size,
                                            rd_kafka_replyq_t replyq,
                                            rd_kafka_resp_cb_t *resp_cb,
                                            void *opaque) {

        int include_authorized_operations;
        rd_kafka_resp_err_t err;
        int groups_cnt          = rd_list_cnt(groups);
        rd_kafka_error_t *error = NULL;

        include_authorized_operations =
            rd_kafka_confval_get_int(&options->include_authorized_operations);

        error = rd_kafka_ConsumerGroupDescribeRequest(
            rkb, (char **)groups->rl_elems, groups_cnt,
            include_authorized_operations, replyq, resp_cb, opaque);

        if (error) {
                rd_snprintf(errstr, errstr_size, "%s",
                            rd_kafka_error_string(error));
                err = rd_kafka_error_code(error);
                rd_kafka_error_destroy(error);
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}
/**
 * @brief Parse DescribeConsumerGroupsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeConsumerGroupsResponse_parse(rd_kafka_op_t *rko_req,
                                              rd_kafka_op_t **rko_resultp,
                                              rd_kafka_buf_t *reply,
                                              char *errstr,
                                              size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        int32_t nodeid;
        uint16_t port;
        int16_t api_version;
        int32_t cnt;
        rd_kafka_op_t *rko_result = NULL;
        rd_kafka_broker_t *rkb    = reply->rkbuf_rkb;
        rd_kafka_Node_t *node     = NULL;
        rd_kafka_error_t *error   = NULL;
        char *group_id = NULL, *group_state = NULL, *proto_type = NULL,
             *proto = NULL, *host = NULL;
        rd_kafka_AclOperation_t *operations = NULL;
        int operation_cnt                   = -1;

        api_version = rd_kafka_buf_ApiVersion(reply);
        if (api_version >= 1) {
                rd_kafka_buf_read_throttle_time(reply);
        }

        rd_kafka_buf_read_arraycnt(reply, &cnt, 100000);

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, cnt,
                     rd_kafka_ConsumerGroupDescription_free);

        nodeid = rkb->rkb_nodeid;
        rd_kafka_broker_lock(rkb);
        host = rd_strdup(rkb->rkb_origname);
        port = rkb->rkb_port;
        rd_kafka_broker_unlock(rkb);

        node = rd_kafka_Node_new(nodeid, host, port, NULL);
        while (cnt-- > 0) {
                int16_t error_code;
                int32_t authorized_operations = -1;
                rd_kafkap_str_t GroupId, GroupState, ProtocolType, ProtocolData;
                rd_bool_t is_simple_consumer_group, is_consumer_protocol_type;
                int32_t member_cnt;
                rd_list_t members;
                rd_kafka_ConsumerGroupDescription_t *grpdesc = NULL;

                rd_kafka_buf_read_i16(reply, &error_code);
                rd_kafka_buf_read_str(reply, &GroupId);
                rd_kafka_buf_read_str(reply, &GroupState);
                rd_kafka_buf_read_str(reply, &ProtocolType);
                rd_kafka_buf_read_str(reply, &ProtocolData);
                rd_kafka_buf_read_arraycnt(reply, &member_cnt, 100000);

                group_id    = RD_KAFKAP_STR_DUP(&GroupId);
                group_state = RD_KAFKAP_STR_DUP(&GroupState);
                proto_type  = RD_KAFKAP_STR_DUP(&ProtocolType);
                proto       = RD_KAFKAP_STR_DUP(&ProtocolData);

                if (error_code) {
                        error = rd_kafka_error_new(
                            error_code, "DescribeConsumerGroups: %s",
                            rd_kafka_err2str(error_code));
                }

                is_simple_consumer_group = *proto_type == '\0';
                is_consumer_protocol_type =
                    !strcmp(proto_type, CONSUMER_PROTOCOL_TYPE);
                if (error == NULL && !is_simple_consumer_group &&
                    !is_consumer_protocol_type) {
                        error = rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "GroupId %s is not a consumer group (%s).",
                            group_id, proto_type);
                }

                rd_list_init(&members, 0, rd_kafka_MemberDescription_free);

                while (member_cnt-- > 0) {
                        rd_kafkap_str_t MemberId, ClientId, ClientHost,
                            GroupInstanceId = RD_KAFKAP_STR_INITIALIZER;
                        char *member_id, *client_id, *client_host,
                            *group_instance_id = NULL;
                        rd_kafkap_bytes_t MemberMetadata, MemberAssignment;
                        rd_kafka_MemberDescription_t *member;
                        rd_kafka_topic_partition_list_t *partitions = NULL;
                        rd_kafka_buf_t *rkbuf;

                        rd_kafka_buf_read_str(reply, &MemberId);
                        if (api_version >= 4) {
                                rd_kafka_buf_read_str(reply, &GroupInstanceId);
                        }
                        rd_kafka_buf_read_str(reply, &ClientId);
                        rd_kafka_buf_read_str(reply, &ClientHost);
                        rd_kafka_buf_read_kbytes(reply, &MemberMetadata);
                        rd_kafka_buf_read_kbytes(reply, &MemberAssignment);
                        if (error != NULL)
                                continue;

                        if (RD_KAFKAP_BYTES_LEN(&MemberAssignment) != 0) {
                                int16_t version;
                                /* Parse assignment */
                                rkbuf = rd_kafka_buf_new_shadow(
                                    MemberAssignment.data,
                                    RD_KAFKAP_BYTES_LEN(&MemberAssignment),
                                    NULL);
                                /* Protocol parser needs a broker handle
                                 * to log errors on. */
                                rkbuf->rkbuf_rkb = rkb;
                                /* Decreased in rd_kafka_buf_destroy */
                                rd_kafka_broker_keep(rkb);
                                rd_kafka_buf_read_i16(rkbuf, &version);
                                const rd_kafka_topic_partition_field_t fields[] =
                                    {RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                                     RD_KAFKA_TOPIC_PARTITION_FIELD_END};
                                partitions = rd_kafka_buf_read_topic_partitions(
                                    rkbuf, rd_false /*don't use topic_id*/,
                                    rd_true, 0, fields);
                                rd_kafka_buf_destroy(rkbuf);
                                if (!partitions)
                                        rd_kafka_buf_parse_fail(
                                            reply,
                                            "Error reading topic partitions");
                        }

                        member_id = RD_KAFKAP_STR_DUP(&MemberId);
                        if (!RD_KAFKAP_STR_IS_NULL(&GroupInstanceId)) {
                                group_instance_id =
                                    RD_KAFKAP_STR_DUP(&GroupInstanceId);
                        }
                        client_id   = RD_KAFKAP_STR_DUP(&ClientId);
                        client_host = RD_KAFKAP_STR_DUP(&ClientHost);

                        /* Target Assignment is `NULL` for the `classic`
                         * protocol as there is no concept of Target Assignment
                         * there. */
                        member = rd_kafka_MemberDescription_new(
                            client_id, member_id, group_instance_id,
                            client_host, partitions,
                            NULL /* target assignment */);
                        if (partitions)
                                rd_kafka_topic_partition_list_destroy(
                                    partitions);
                        rd_list_add(&members, member);
                        rd_free(member_id);
                        rd_free(group_instance_id);
                        rd_free(client_id);
                        rd_free(client_host);
                        member_id         = NULL;
                        group_instance_id = NULL;
                        client_id         = NULL;
                        client_host       = NULL;
                }

                if (api_version >= 3) {
                        rd_kafka_buf_read_i32(reply, &authorized_operations);
                        /* Authorized_operations is INT_MIN
                         * in case of not being requested, and the list is NULL
                         * that case. */
                        operations = rd_kafka_AuthorizedOperations_parse(
                            authorized_operations, &operation_cnt);
                }

                if (error == NULL) {
                        grpdesc = rd_kafka_ConsumerGroupDescription_new(
                            group_id, is_simple_consumer_group, &members, proto,
                            operations, operation_cnt,
                            rd_kafka_consumer_group_state_code(group_state),
                            RD_KAFKA_CONSUMER_GROUP_TYPE_CLASSIC, node, error);
                } else
                        grpdesc = rd_kafka_ConsumerGroupDescription_new_error(
                            group_id, error);

                rd_list_add(&rko_result->rko_u.admin_result.results, grpdesc);

                rd_list_destroy(&members);
                rd_free(group_id);
                rd_free(group_state);
                rd_free(proto_type);
                rd_free(proto);
                RD_IF_FREE(error, rd_kafka_error_destroy);
                RD_IF_FREE(operations, rd_free);

                error       = NULL;
                group_id    = NULL;
                group_state = NULL;
                proto_type  = NULL;
                proto       = NULL;
                operations  = NULL;
        }

        if (host)
                rd_free(host);
        if (node)
                rd_kafka_Node_destroy(node);
        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        if (group_id)
                rd_free(group_id);
        if (group_state)
                rd_free(group_state);
        if (proto_type)
                rd_free(proto_type);
        if (proto)
                rd_free(proto);
        if (error)
                rd_kafka_error_destroy(error);
        if (host)
                rd_free(host);
        if (node)
                rd_kafka_Node_destroy(node);
        if (rko_result)
                rd_kafka_op_destroy(rko_result);
        RD_IF_FREE(operations, rd_free);

        rd_snprintf(
            errstr, errstr_size,
            "DescribeConsumerGroups response protocol parse failure: %s",
            rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

/**
 * @brief Parse ConsumerGroupDescriberesponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_ConsumerGroupDescribeResponse_parse(rd_kafka_op_t *rko_req,
                                             rd_kafka_op_t **rko_resultp,
                                             rd_kafka_buf_t *reply,
                                             char *errstr,
                                             size_t errstr_size) {
        const int log_decode_errors = LOG_ERR;
        int32_t groups_cnt;
        rd_kafka_op_t *rko_result = NULL;
        rd_kafka_broker_t *rkb    = reply->rkbuf_rkb;
        rd_kafka_error_t *error   = NULL;
        char *group_id = NULL, *group_state = NULL, *assignor_name = NULL,
             *host                                         = NULL;
        rd_kafka_AclOperation_t *operations                = NULL;
        rd_kafka_Node_t *node                              = NULL;
        rd_kafka_topic_partition_list_t *assignment        = NULL,
                                        *target_assignment = NULL;
        int32_t nodeid;
        uint16_t port;
        int operation_cnt = -1;
        int32_t i;

        rd_kafka_buf_read_throttle_time(reply);

        rd_kafka_buf_read_arraycnt(reply, &groups_cnt, RD_KAFKAP_GROUPS_MAX);

        nodeid = rkb->rkb_nodeid;
        rd_kafka_broker_lock(rkb);
        host = rd_strdup(rkb->rkb_origname);
        port = rkb->rkb_port;
        rd_kafka_broker_unlock(rkb);

        node = rd_kafka_Node_new(nodeid, host, port, NULL);

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, groups_cnt,
                     rd_kafka_ConsumerGroupDescription_free);

        for (i = 0; i < groups_cnt; i++) {
                int16_t ErrorCode;
                int32_t authorized_operations = -1;
                int32_t MemberCnt, j;
                int32_t GroupEpoch, AssignmentEpoch;
                rd_kafkap_str_t GroupId, GroupState, AssignorName, ErrorString;
                rd_list_t members;
                rd_kafka_ConsumerGroupDescription_t *grpdesc = NULL;

                rd_kafka_buf_read_i16(reply, &ErrorCode);
                rd_kafka_buf_read_str(reply, &ErrorString);
                rd_kafka_buf_read_str(reply, &GroupId);
                rd_kafka_buf_read_str(reply, &GroupState);
                rd_kafka_buf_read_i32(reply, &GroupEpoch);
                rd_kafka_buf_read_i32(reply, &AssignmentEpoch);
                rd_kafka_buf_read_str(reply, &AssignorName);
                rd_kafka_buf_read_arraycnt(reply, &MemberCnt, 100000);

                group_id      = RD_KAFKAP_STR_DUP(&GroupId);
                group_state   = RD_KAFKAP_STR_DUP(&GroupState);
                assignor_name = RD_KAFKAP_STR_DUP(&AssignorName);

                if (ErrorCode) {
                        error = rd_kafka_error_new(
                            ErrorCode, "ConsumerGroupDescribe: %.*s",
                            RD_KAFKAP_STR_PR(&ErrorString));
                }

                rd_list_init(&members, MemberCnt,
                             rd_kafka_MemberDescription_free);

                for (j = 0; j < MemberCnt; j++) {
                        char *member_id = NULL, *instance_id = NULL,
                             *client_id = NULL, *client_host = NULL;
                        rd_kafkap_str_t MemberId, InstanceId, RackId, ClientId,
                            ClientHost, SubscribedTopicRegex;
                        int32_t MemberEpoch, idx;
                        rd_kafka_MemberDescription_t *member;
                        int32_t SubscribedTopicNamesArrayCnt;

                        rd_kafka_buf_read_str(reply, &MemberId);
                        rd_kafka_buf_read_str(reply, &InstanceId);
                        rd_kafka_buf_read_str(reply, &RackId);
                        rd_kafka_buf_read_i32(reply, &MemberEpoch);
                        rd_kafka_buf_read_str(reply, &ClientId);
                        rd_kafka_buf_read_str(reply, &ClientHost);
                        rd_kafka_buf_read_arraycnt(
                            reply, &SubscribedTopicNamesArrayCnt, 100000);

                        for (idx = 0; idx < SubscribedTopicNamesArrayCnt;
                             idx++) {
                                rd_kafkap_str_t SubscribedTopicName;
                                rd_kafka_buf_read_str(reply,
                                                      &SubscribedTopicName);
                        }
                        rd_kafka_buf_read_str(reply, &SubscribedTopicRegex);
                        const rd_kafka_topic_partition_field_t fields[] = {
                            RD_KAFKA_TOPIC_PARTITION_FIELD_PARTITION,
                            RD_KAFKA_TOPIC_PARTITION_FIELD_END};

                        assignment = rd_kafka_buf_read_topic_partitions(
                            reply, rd_true /* use topic_id */,
                            rd_true /* use topic name*/, 0, fields);

                        /* Assignment tags */
                        rd_kafka_buf_skip_tags(reply);

                        target_assignment = rd_kafka_buf_read_topic_partitions(
                            reply, rd_true /* use topic_id */,
                            rd_true /* use topic name*/, 0, fields);

                        /* TargetAssignment tags */
                        rd_kafka_buf_skip_tags(reply);

                        /* Member tags */
                        rd_kafka_buf_skip_tags(reply);

                        member_id = RD_KAFKAP_STR_DUP(&MemberId);
                        if (!RD_KAFKAP_STR_IS_NULL(&InstanceId)) {
                                instance_id = RD_KAFKAP_STR_DUP(&InstanceId);
                        }
                        client_id   = RD_KAFKAP_STR_DUP(&ClientId);
                        client_host = RD_KAFKAP_STR_DUP(&ClientHost);

                        member = rd_kafka_MemberDescription_new(
                            client_id, member_id, instance_id, client_host,
                            assignment, target_assignment);


                        rd_list_add(&members, member);

                        RD_IF_FREE(assignment,
                                   rd_kafka_topic_partition_list_destroy);
                        RD_IF_FREE(target_assignment,
                                   rd_kafka_topic_partition_list_destroy);

                        RD_IF_FREE(member_id, rd_free);
                        RD_IF_FREE(instance_id, rd_free);
                        RD_IF_FREE(client_id, rd_free);
                        RD_IF_FREE(client_host, rd_free);
                        member_id   = NULL;
                        instance_id = NULL;
                        client_id   = NULL;
                        client_host = NULL;
                }
                rd_kafka_buf_read_i32(reply, &authorized_operations);
                operations = rd_kafka_AuthorizedOperations_parse(
                    authorized_operations, &operation_cnt);
                rd_kafka_buf_skip_tags(reply);

                /* If the error code is Group ID Not Found or Unsupported
                   Version, we will set the ConsumerGroupType to Consumer to
                   identify it for further processing with the old protocol and
                   eventually in rd_kafka_DescribeConsumerGroupsResponse_parse
                   we will set the ConsumerGroupType to Unknown */
                if (!error) {
                        grpdesc = rd_kafka_ConsumerGroupDescription_new(
                            group_id, rd_false, &members, assignor_name,
                            operations, operation_cnt,
                            rd_kafka_consumer_group_state_code(group_state),
                            RD_KAFKA_CONSUMER_GROUP_TYPE_CONSUMER, node, error);
                } else {
                        grpdesc = rd_kafka_ConsumerGroupDescription_new_error(
                            group_id, error);
                }

                rd_list_add(&rko_result->rko_u.admin_result.results, grpdesc);

                rd_list_destroy(&members);
                rd_free(group_id);
                rd_free(group_state);
                rd_free(assignor_name);
                RD_IF_FREE(error, rd_kafka_error_destroy);
                RD_IF_FREE(operations, rd_free);

                error         = NULL;
                group_id      = NULL;
                group_state   = NULL;
                assignor_name = NULL;
                operations    = NULL;
        }
        rd_kafka_buf_skip_tags(reply);
        RD_IF_FREE(host, rd_free);
        RD_IF_FREE(node, rd_kafka_Node_destroy);
        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;
err_parse:
        RD_IF_FREE(group_id, rd_free);
        RD_IF_FREE(group_state, rd_free);
        RD_IF_FREE(assignor_name, rd_free);
        RD_IF_FREE(host, rd_free);
        RD_IF_FREE(node, rd_kafka_Node_destroy);
        RD_IF_FREE(error, rd_kafka_error_destroy);
        RD_IF_FREE(operations, rd_free);
        RD_IF_FREE(assignment, rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(target_assignment, rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(rko_result, rd_kafka_op_destroy);

        rd_snprintf(
            errstr, errstr_size,
            "DescribeConsumerGroups response protocol parse failure: %s",
            rd_kafka_err2str(reply->rkbuf_err));
        return reply->rkbuf_err;
}

/**
 * @brief In case if we get an Unsupported Feature error or if it is a consumer
           group and we get errors GROUP_ID_NOT_FOUND(69) or
           UNSUPPORTED_VERSION(35) we need to send a request to the old
           protocol.
 */
static rd_bool_t rd_kafka_admin_describe_consumer_group_do_fallback_to_classic(
    rd_kafka_ConsumerGroupDescription_t *groupres) {
        return groupres->error &&
               (groupres->error->code == RD_KAFKA_RESP_ERR_GROUP_ID_NOT_FOUND ||
                groupres->error->code ==
                    RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION ||
                groupres->error->code ==
                    RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE);
}

static void rd_kafka_admin_describe_consumer_group_request(
    rd_kafka_op_t *rko_fanout,
    rd_kafka_t *rk,
    const char *group_id,
    const struct rd_kafka_admin_worker_cbs *cbs,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_q_t *rkq) {
        rd_kafka_op_t *rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DESCRIBECONSUMERGROUPS,
            RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT, cbs, options, rkq);

        rko->rko_u.admin_request.fanout_parent = rko_fanout;
        rko->rko_u.admin_request.broker_id = RD_KAFKA_ADMIN_TARGET_COORDINATOR;
        rko->rko_u.admin_request.coordtype = RD_KAFKA_COORD_GROUP;
        rko->rko_u.admin_request.coordkey  = rd_strdup(group_id);

        /* Set the group name as the opaque so the fanout worker use it
         * to fill in errors.
         * References rko_fanout's memory, which will always outlive
         * the fanned out op. */
        rd_kafka_AdminOptions_set_opaque(&rko->rko_u.admin_request.options,
                                         (void *)group_id);

        rd_list_init(&rko->rko_u.admin_request.args, 1, rd_free);
        rd_list_add(&rko->rko_u.admin_request.args, rd_strdup(group_id));

        rd_kafka_q_enq(rko_fanout->rko_rk->rk_ops, rko);
}

/** @brief Merge the DescribeConsumerGroups response from a single broker
 *         into the user response list.
 */
static void rd_kafka_DescribeConsumerGroups_response_merge(
    rd_kafka_op_t *rko_fanout,
    const rd_kafka_op_t *rko_partial) {
        rd_kafka_ConsumerGroupDescription_t *groupres = NULL;
        rd_kafka_ConsumerGroupDescription_t *newgroupres;
        const char *grp = rko_partial->rko_u.admin_result.opaque;
        int orig_pos;

        rd_assert(rko_partial->rko_evtype ==
                  RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT);

        if (!rko_partial->rko_err) {
                /* Proper results.
                 * We only send one group per request, make sure it matches */
                groupres =
                    rd_list_elem(&rko_partial->rko_u.admin_result.results, 0);
                rd_assert(groupres);
                rd_assert(!strcmp(groupres->group_id, grp));
                newgroupres = rd_kafka_ConsumerGroupDescription_copy(groupres);
        } else {
                /* Op errored, e.g. timeout */
                rd_kafka_error_t *error =
                    rd_kafka_error_new(rko_partial->rko_err, NULL);
                newgroupres =
                    rd_kafka_ConsumerGroupDescription_new_error(grp, error);
                rd_kafka_error_destroy(error);
        }

        rd_bool_t is_consumer_group_response =
            rko_partial->rko_u.admin_result.cbs->request ==
            rd_kafka_admin_ConsumerGroupDescribeRequest;

        if (is_consumer_group_response &&
            rd_kafka_admin_describe_consumer_group_do_fallback_to_classic(
                newgroupres)) {
                /* We need to send a request to the old protocol */
                rko_fanout->rko_u.admin_request.fanout.outstanding++;
                static const struct rd_kafka_admin_worker_cbs cbs = {
                    rd_kafka_admin_DescribeConsumerGroupsRequest,
                    rd_kafka_DescribeConsumerGroupsResponse_parse,
                };
                rd_kafka_admin_describe_consumer_group_request(
                    rko_fanout, rko_fanout->rko_rk, grp, &cbs,
                    &rko_fanout->rko_u.admin_request.options,
                    rko_fanout->rko_rk->rk_ops);

                rd_kafka_ConsumerGroupDescription_destroy(newgroupres);
        } else {
                /* As a convenience to the application we insert group result
                 * in the same order as they were requested. */
                orig_pos =
                    rd_list_index(&rko_fanout->rko_u.admin_request.args, grp,
                                  rd_kafka_DescribeConsumerGroups_cmp);
                rd_assert(orig_pos != -1);

                /* Make sure result is not already set */
                rd_assert(rd_list_elem(
                              &rko_fanout->rko_u.admin_request.fanout.results,
                              orig_pos) == NULL);

                rd_list_set(&rko_fanout->rko_u.admin_request.fanout.results,
                            orig_pos, newgroupres);
        }
}

void rd_kafka_DescribeConsumerGroups(rd_kafka_t *rk,
                                     const char **groups,
                                     size_t groups_cnt,
                                     const rd_kafka_AdminOptions_t *options,
                                     rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko_fanout;
        rd_list_t dup_list;
        size_t i;
        static const struct rd_kafka_admin_fanout_worker_cbs fanout_cbs = {
            rd_kafka_DescribeConsumerGroups_response_merge,
            rd_kafka_ConsumerGroupDescription_copy_opaque};

        rd_assert(rkqu);

        rko_fanout = rd_kafka_admin_fanout_op_new(
            rk, RD_KAFKA_OP_DESCRIBECONSUMERGROUPS,
            RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT, &fanout_cbs, options,
            rkqu->rkqu_q);

        if (groups_cnt == 0) {
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "No groups to describe");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        /* Copy group list and store it on the request op.
         * Maintain original ordering. */
        rd_list_init(&rko_fanout->rko_u.admin_request.args, (int)groups_cnt,
                     rd_free);
        for (i = 0; i < groups_cnt; i++)
                rd_list_add(&rko_fanout->rko_u.admin_request.args,
                            rd_strdup(groups[i]));

        /* Check for duplicates.
         * Make a temporary copy of the group list and sort it to check for
         * duplicates, we don't want the original list sorted since we want
         * to maintain ordering. */
        rd_list_init(&dup_list,
                     rd_list_cnt(&rko_fanout->rko_u.admin_request.args), NULL);
        rd_list_copy_to(&dup_list, &rko_fanout->rko_u.admin_request.args, NULL,
                        NULL);
        rd_list_sort(&dup_list, rd_kafka_DescribeConsumerGroups_cmp);
        if (rd_list_find_duplicate(&dup_list,
                                   rd_kafka_DescribeConsumerGroups_cmp)) {
                rd_list_destroy(&dup_list);
                rd_kafka_admin_result_fail(rko_fanout,
                                           RD_KAFKA_RESP_ERR__INVALID_ARG,
                                           "Duplicate groups not allowed");
                rd_kafka_admin_common_worker_destroy(rk, rko_fanout,
                                                     rd_true /*destroy*/);
                return;
        }

        rd_list_destroy(&dup_list);

        /* Prepare results list where fanned out op's results will be
         * accumulated. */
        rd_list_init(&rko_fanout->rko_u.admin_request.fanout.results,
                     (int)groups_cnt, rd_kafka_ConsumerGroupDescription_free);
        rko_fanout->rko_u.admin_request.fanout.outstanding = (int)groups_cnt;

        /* Create individual request ops for each group.
         * FIXME: A future optimization is to coalesce all groups for a single
         *        coordinator into one op. */
        for (i = 0; i < groups_cnt; i++) {
                static const struct rd_kafka_admin_worker_cbs cbs = {
                    rd_kafka_admin_ConsumerGroupDescribeRequest,
                    rd_kafka_ConsumerGroupDescribeResponse_parse,
                };
                char *grp =
                    rd_list_elem(&rko_fanout->rko_u.admin_request.args, (int)i);
                rd_kafka_admin_describe_consumer_group_request(
                    rko_fanout, rk, grp, &cbs, options, rk->rk_ops);
        }
}

const rd_kafka_ConsumerGroupDescription_t **
rd_kafka_DescribeConsumerGroups_result_groups(
    const rd_kafka_DescribeConsumerGroups_result_t *result,
    size_t *cntp) {
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DESCRIBECONSUMERGROUPS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_ConsumerGroupDescription_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**@}*/

/**
 * @name Describe Topic
 * @{
 *
 *
 *
 *
 */

rd_kafka_TopicCollection_t *
rd_kafka_TopicCollection_of_topic_names(const char **topics,
                                        size_t topics_cnt) {
        size_t i;
        rd_kafka_TopicCollection_t *ret =
            rd_calloc(1, sizeof(rd_kafka_TopicCollection_t));

        ret->topics_cnt = topics_cnt;
        if (!ret->topics_cnt)
                return ret;

        ret->topics = rd_calloc(topics_cnt, sizeof(char *));
        for (i = 0; i < topics_cnt; i++)
                ret->topics[i] = rd_strdup(topics[i]);

        return ret;
}

void rd_kafka_TopicCollection_destroy(rd_kafka_TopicCollection_t *topics) {
        size_t i;

        for (i = 0; i < topics->topics_cnt; i++)
                rd_free(topics->topics[i]);

        RD_IF_FREE(topics->topics, rd_free);
        rd_free(topics);
}

/**
 * @brief Create a new TopicPartitionInfo object.
 *
 * @return A newly allocated TopicPartitionInfo. Use
 * rd_kafka_TopicPartitionInfo_destroy() to free when done.
 */
static rd_kafka_TopicPartitionInfo_t *rd_kafka_TopicPartitionInfo_new(
    const struct rd_kafka_metadata_partition *partition,
    const struct rd_kafka_metadata_broker *brokers_sorted,
    const rd_kafka_metadata_broker_internal_t *brokers_internal,
    int broker_cnt) {
        size_t i;
        rd_kafka_TopicPartitionInfo_t *pinfo =
            rd_calloc(1, sizeof(rd_kafka_TopicPartitionInfo_t));

        pinfo->partition   = partition->id;
        pinfo->isr_cnt     = partition->isr_cnt;
        pinfo->replica_cnt = partition->replica_cnt;

        if (partition->leader >= 0) {
                pinfo->leader = rd_kafka_Node_new_from_brokers(
                    partition->leader, brokers_sorted, brokers_internal,
                    broker_cnt);
        }

        if (pinfo->isr_cnt > 0) {
                pinfo->isr =
                    rd_calloc(pinfo->isr_cnt, sizeof(rd_kafka_Node_t *));
                for (i = 0; i < pinfo->isr_cnt; i++)
                        pinfo->isr[i] = rd_kafka_Node_new_from_brokers(
                            partition->isrs[i], brokers_sorted,
                            brokers_internal, broker_cnt);
        }

        if (pinfo->replica_cnt > 0) {
                pinfo->replicas =
                    rd_calloc(pinfo->replica_cnt, sizeof(rd_kafka_Node_t *));
                for (i = 0; i < pinfo->replica_cnt; i++)
                        pinfo->replicas[i] = rd_kafka_Node_new_from_brokers(
                            partition->replicas[i], brokers_sorted,
                            brokers_internal, broker_cnt);
        }

        return pinfo;
}

/**
 * @brief Destroy and deallocate a TopicPartitionInfo.
 */
static void
rd_kafka_TopicPartitionInfo_destroy(rd_kafka_TopicPartitionInfo_t *pinfo) {
        size_t i;
        RD_IF_FREE(pinfo->leader, rd_kafka_Node_destroy);

        for (i = 0; i < pinfo->isr_cnt; i++)
                rd_kafka_Node_destroy(pinfo->isr[i]);
        RD_IF_FREE(pinfo->isr, rd_free);

        for (i = 0; i < pinfo->replica_cnt; i++)
                rd_kafka_Node_destroy(pinfo->replicas[i]);
        RD_IF_FREE(pinfo->replicas, rd_free);

        rd_free(pinfo);
}

/**
 * @brief Create a new TopicDescription object.
 *
 * @param topic topic name
 * @param topic_id topic id
 * @param partitions Array of partition metadata (rd_kafka_metadata_partition).
 * @param partition_cnt Number of partitions in partition metadata.
 * @param authorized_operations acl operations allowed for topic.
 * @param error Topic error reported by the broker.
 * @return A newly allocated TopicDescription object.
 * @remark Use rd_kafka_TopicDescription_destroy() to free when done.
 */
static rd_kafka_TopicDescription_t *rd_kafka_TopicDescription_new(
    const char *topic,
    rd_kafka_Uuid_t topic_id,
    const struct rd_kafka_metadata_partition *partitions,
    int partition_cnt,
    const struct rd_kafka_metadata_broker *brokers_sorted,
    const rd_kafka_metadata_broker_internal_t *brokers_internal,
    int broker_cnt,
    const rd_kafka_AclOperation_t *authorized_operations,
    int authorized_operations_cnt,
    rd_bool_t is_internal,
    rd_kafka_error_t *error) {
        rd_kafka_TopicDescription_t *topicdesc;
        int i;
        topicdesc                = rd_calloc(1, sizeof(*topicdesc));
        topicdesc->topic         = rd_strdup(topic);
        topicdesc->topic_id      = topic_id;
        topicdesc->partition_cnt = partition_cnt;
        topicdesc->is_internal   = is_internal;
        if (error)
                topicdesc->error = rd_kafka_error_copy(error);

        topicdesc->authorized_operations_cnt = authorized_operations_cnt;
        topicdesc->authorized_operations = rd_kafka_AuthorizedOperations_copy(
            authorized_operations, authorized_operations_cnt);

        if (partitions) {
                topicdesc->partitions =
                    rd_calloc(partition_cnt, sizeof(*partitions));
                for (i = 0; i < partition_cnt; i++)
                        topicdesc->partitions[i] =
                            rd_kafka_TopicPartitionInfo_new(
                                &partitions[i], brokers_sorted,
                                brokers_internal, broker_cnt);
        }
        return topicdesc;
}

/**
 * @brief Create a new TopicDescription object from an error.
 *
 * @param topic topic name
 * @param error Topic error reported by the broker.
 * @return A newly allocated TopicDescription with the passed error.
 * @remark Use rd_kafka_TopicDescription_destroy() to free when done.
 */
static rd_kafka_TopicDescription_t *
rd_kafka_TopicDescription_new_error(const char *topic,
                                    rd_kafka_Uuid_t topic_id,
                                    rd_kafka_error_t *error) {
        return rd_kafka_TopicDescription_new(topic, topic_id, NULL, 0, NULL,
                                             NULL, 0, NULL, 0, rd_false, error);
}

static void
rd_kafka_TopicDescription_destroy(rd_kafka_TopicDescription_t *topicdesc) {
        int i;

        RD_IF_FREE(topicdesc->topic, rd_free);
        RD_IF_FREE(topicdesc->error, rd_kafka_error_destroy);
        RD_IF_FREE(topicdesc->authorized_operations, rd_free);
        for (i = 0; i < topicdesc->partition_cnt; i++)
                rd_kafka_TopicPartitionInfo_destroy(topicdesc->partitions[i]);
        rd_free(topicdesc->partitions);

        rd_free(topicdesc);
}

static void rd_kafka_TopicDescription_free(void *ptr) {
        rd_kafka_TopicDescription_destroy(ptr);
}

const int rd_kafka_TopicPartitionInfo_partition(
    const rd_kafka_TopicPartitionInfo_t *partition) {
        return partition->partition;
}

const rd_kafka_Node_t *rd_kafka_TopicPartitionInfo_leader(
    const rd_kafka_TopicPartitionInfo_t *partition) {
        return partition->leader;
}


const rd_kafka_Node_t **
rd_kafka_TopicPartitionInfo_isr(const rd_kafka_TopicPartitionInfo_t *partition,
                                size_t *cntp) {
        *cntp = partition->isr_cnt;
        return (const rd_kafka_Node_t **)partition->isr;
}

const rd_kafka_Node_t **rd_kafka_TopicPartitionInfo_replicas(
    const rd_kafka_TopicPartitionInfo_t *partition,
    size_t *cntp) {
        *cntp = partition->replica_cnt;
        return (const rd_kafka_Node_t **)partition->replicas;
}

const rd_kafka_TopicPartitionInfo_t **rd_kafka_TopicDescription_partitions(
    const rd_kafka_TopicDescription_t *topicdesc,
    size_t *cntp) {
        *cntp = topicdesc->partition_cnt;
        return (const rd_kafka_TopicPartitionInfo_t **)topicdesc->partitions;
}

const rd_kafka_AclOperation_t *rd_kafka_TopicDescription_authorized_operations(
    const rd_kafka_TopicDescription_t *topicdesc,
    size_t *cntp) {
        *cntp = RD_MAX(topicdesc->authorized_operations_cnt, 0);
        return topicdesc->authorized_operations;
}


const char *
rd_kafka_TopicDescription_name(const rd_kafka_TopicDescription_t *topicdesc) {
        return topicdesc->topic;
}

int rd_kafka_TopicDescription_is_internal(
    const rd_kafka_TopicDescription_t *topicdesc) {
        return topicdesc->is_internal;
}

const rd_kafka_error_t *
rd_kafka_TopicDescription_error(const rd_kafka_TopicDescription_t *topicdesc) {
        return topicdesc->error;
}

const rd_kafka_Uuid_t *rd_kafka_TopicDescription_topic_id(
    const rd_kafka_TopicDescription_t *topicdesc) {
        return &topicdesc->topic_id;
}

const rd_kafka_TopicDescription_t **rd_kafka_DescribeTopics_result_topics(
    const rd_kafka_DescribeTopics_result_t *result,
    size_t *cntp) {
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DESCRIBETOPICS);

        *cntp = rd_list_cnt(&rko->rko_u.admin_result.results);
        return (const rd_kafka_TopicDescription_t **)
            rko->rko_u.admin_result.results.rl_elems;
}

/**
 * @brief Topics arguments comparator for DescribeTopics args
 */
static int rd_kafka_DescribeTopics_cmp(const void *a, const void *b) {
        return strcmp(a, b);
}

/**
 * @brief Construct and send DescribeTopicsRequest to \p rkb
 *        with the topics (char *) in \p topics, using
 *        \p options.
 *
 *        The response (unparsed) will be enqueued on \p replyq
 *        for handling by \p resp_cb (with \p opaque passed).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the request was enqueued for
 *          transmission, otherwise an error code and errstr will be
 *          updated with a human readable error string.
 */
static rd_kafka_resp_err_t
rd_kafka_admin_DescribeTopicsRequest(rd_kafka_broker_t *rkb,
                                     const rd_list_t *topics /*(char*)*/,
                                     rd_kafka_AdminOptions_t *options,
                                     char *errstr,
                                     size_t errstr_size,
                                     rd_kafka_replyq_t replyq,
                                     rd_kafka_resp_cb_t *resp_cb,
                                     void *opaque) {
        rd_kafka_resp_err_t err;
        int include_topic_authorized_operations =
            rd_kafka_confval_get_int(&options->include_authorized_operations);

        err = rd_kafka_admin_MetadataRequest(
            rkb, topics, "describe topics",
            rd_false /* don't include_topic_authorized_operations */,
            include_topic_authorized_operations,
            rd_false /* don't force_racks */, resp_cb, replyq, opaque);

        if (err) {
                rd_snprintf(errstr, errstr_size, "%s", rd_kafka_err2str(err));
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse DescribeTopicsResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeTopicsResponse_parse(rd_kafka_op_t *rko_req,
                                      rd_kafka_op_t **rko_resultp,
                                      rd_kafka_buf_t *reply,
                                      char *errstr,
                                      size_t errstr_size) {
        rd_kafka_metadata_internal_t *mdi = NULL;
        struct rd_kafka_metadata *md      = NULL;
        rd_kafka_resp_err_t err;
        rd_list_t topics       = rko_req->rko_u.admin_request.args;
        rd_kafka_broker_t *rkb = reply->rkbuf_rkb;
        int i;
        const int log_decode_errors = LOG_ERR;
        rd_kafka_op_t *rko_result   = NULL;

        err = rd_kafka_parse_Metadata_admin(rkb, reply, &topics, &mdi);
        if (err)
                goto err_parse;

        rko_result = rd_kafka_admin_result_new(rko_req);
        md         = &mdi->metadata;
        rd_list_init(&rko_result->rko_u.admin_result.results, md->topic_cnt,
                     rd_kafka_TopicDescription_free);

        for (i = 0; i < md->topic_cnt; i++) {
                rd_kafka_TopicDescription_t *topicdesc = NULL;
                int orig_pos;

                if (md->topics[i].err == RD_KAFKA_RESP_ERR_NO_ERROR) {
                        rd_kafka_AclOperation_t *authorized_operations;
                        int authorized_operation_cnt;
                        authorized_operations =
                            rd_kafka_AuthorizedOperations_parse(
                                mdi->topics[i].topic_authorized_operations,
                                &authorized_operation_cnt);
                        topicdesc = rd_kafka_TopicDescription_new(
                            md->topics[i].topic, mdi->topics[i].topic_id,
                            md->topics[i].partitions,
                            md->topics[i].partition_cnt, mdi->brokers_sorted,
                            mdi->brokers, md->broker_cnt, authorized_operations,
                            authorized_operation_cnt,
                            mdi->topics[i].is_internal, NULL);
                        RD_IF_FREE(authorized_operations, rd_free);
                } else {
                        rd_kafka_error_t *error = rd_kafka_error_new(
                            md->topics[i].err, "%s",
                            rd_kafka_err2str(md->topics[i].err));
                        topicdesc = rd_kafka_TopicDescription_new_error(
                            md->topics[i].topic, mdi->topics[i].topic_id,
                            error);
                        rd_kafka_error_destroy(error);
                }
                orig_pos = rd_list_index(&rko_result->rko_u.admin_result.args,
                                         topicdesc->topic,
                                         rd_kafka_DescribeTopics_cmp);
                if (orig_pos == -1) {
                        rd_kafka_TopicDescription_destroy(topicdesc);
                        rd_kafka_buf_parse_fail(
                            reply,
                            "Broker returned topic %s that was not "
                            "included in the original request",
                            topicdesc->topic);
                }

                if (rd_list_elem(&rko_result->rko_u.admin_result.results,
                                 orig_pos) != NULL) {
                        rd_kafka_TopicDescription_destroy(topicdesc);
                        rd_kafka_buf_parse_fail(
                            reply, "Broker returned topic %s multiple times",
                            topicdesc->topic);
                }

                rd_list_set(&rko_result->rko_u.admin_result.results, orig_pos,
                            topicdesc);
        }
        rd_free(mdi);

        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err_parse:
        RD_IF_FREE(rko_result, rd_kafka_op_destroy);
        rd_snprintf(errstr, errstr_size,
                    "DescribeTopics response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));
        return reply->rkbuf_err;
}

void rd_kafka_DescribeTopics(rd_kafka_t *rk,
                             const rd_kafka_TopicCollection_t *topics,
                             const rd_kafka_AdminOptions_t *options,
                             rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        rd_list_t dup_list;
        size_t i;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_admin_DescribeTopicsRequest,
            rd_kafka_DescribeTopicsResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DESCRIBETOPICS,
            RD_KAFKA_EVENT_DESCRIBETOPICS_RESULT, &cbs, options, rkqu->rkqu_q);

        rd_list_init(&rko->rko_u.admin_request.args, (int)topics->topics_cnt,
                     rd_free);
        for (i = 0; i < topics->topics_cnt; i++)
                rd_list_add(&rko->rko_u.admin_request.args,
                            rd_strdup(topics->topics[i]));

        if (rd_list_cnt(&rko->rko_u.admin_request.args)) {
                int j;
                char *topic_name;
                /* Check for duplicates.
                 * Make a temporary copy of the topic list and sort it to check
                 * for duplicates, we don't want the original list sorted since
                 * we want to maintain ordering. */
                rd_list_init(&dup_list,
                             rd_list_cnt(&rko->rko_u.admin_request.args), NULL);
                rd_list_copy_to(&dup_list, &rko->rko_u.admin_request.args, NULL,
                                NULL);
                rd_list_sort(&dup_list, rd_kafka_DescribeTopics_cmp);
                if (rd_list_find_duplicate(&dup_list,
                                           rd_kafka_DescribeTopics_cmp)) {
                        rd_list_destroy(&dup_list);
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate topics not allowed");
                        rd_kafka_admin_common_worker_destroy(
                            rk, rko, rd_true /*destroy*/);
                        return;
                }

                /* Check for empty topics. */
                RD_LIST_FOREACH(topic_name, &rko->rko_u.admin_request.args, j) {
                        if (!topic_name[0]) {
                                rd_list_destroy(&dup_list);
                                rd_kafka_admin_result_fail(
                                    rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                                    "Empty topic name at index %d isn't "
                                    "allowed",
                                    j);
                                rd_kafka_admin_common_worker_destroy(
                                    rk, rko, rd_true /*destroy*/);
                                return;
                        }
                }

                rd_list_destroy(&dup_list);
                rd_kafka_q_enq(rk->rk_ops, rko);
        } else {
                /* Empty list */
                rd_kafka_op_t *rko_result = rd_kafka_admin_result_new(rko);
                /* Enqueue empty result on application queue, we're done. */
                rd_kafka_admin_result_enq(rko, rko_result);
                rd_kafka_admin_common_worker_destroy(rk, rko,
                                                     rd_true /*destroy*/);
        }
}

/**@}*/

/**
 * @name Describe cluster
 * @{
 *
 *
 *
 *
 */

static const rd_kafka_ClusterDescription_t *
rd_kafka_DescribeCluster_result_description(
    const rd_kafka_DescribeCluster_result_t *result) {
        int cluster_result_cnt;
        const rd_kafka_ClusterDescription_t *clusterdesc;
        const rd_kafka_op_t *rko = (const rd_kafka_op_t *)result;
        rd_kafka_op_type_t reqtype =
            rko->rko_u.admin_result.reqtype & ~RD_KAFKA_OP_FLAGMASK;
        rd_assert(reqtype == RD_KAFKA_OP_DESCRIBECLUSTER);

        cluster_result_cnt = rd_list_cnt(&rko->rko_u.admin_result.results);
        rd_assert(cluster_result_cnt == 1);
        clusterdesc = rd_list_elem(&rko->rko_u.admin_result.results, 0);

        return clusterdesc;
}


const rd_kafka_Node_t **rd_kafka_DescribeCluster_result_nodes(
    const rd_kafka_DescribeCluster_result_t *result,
    size_t *cntp) {
        const rd_kafka_ClusterDescription_t *clusterdesc =
            rd_kafka_DescribeCluster_result_description(result);
        *cntp = clusterdesc->node_cnt;
        return (const rd_kafka_Node_t **)clusterdesc->nodes;
}

const rd_kafka_AclOperation_t *
rd_kafka_DescribeCluster_result_authorized_operations(
    const rd_kafka_DescribeCluster_result_t *result,
    size_t *cntp) {
        const rd_kafka_ClusterDescription_t *clusterdesc =
            rd_kafka_DescribeCluster_result_description(result);
        *cntp = RD_MAX(clusterdesc->authorized_operations_cnt, 0);
        return clusterdesc->authorized_operations;
}

const char *rd_kafka_DescribeCluster_result_cluster_id(
    const rd_kafka_DescribeCluster_result_t *result) {
        return rd_kafka_DescribeCluster_result_description(result)->cluster_id;
}

const rd_kafka_Node_t *rd_kafka_DescribeCluster_result_controller(
    const rd_kafka_DescribeCluster_result_t *result) {
        return rd_kafka_DescribeCluster_result_description(result)->controller;
}

/**
 * @brief Create a new ClusterDescription object.
 *
 * @param cluster_id current cluster_id
 * @param controller_id current controller_id.
 * @param md metadata struct returned by parse_metadata().
 *
 * @returns newly allocated ClusterDescription object.
 * @remark Use rd_kafka_ClusterDescription_destroy() to free when done.
 */
static rd_kafka_ClusterDescription_t *
rd_kafka_ClusterDescription_new(const rd_kafka_metadata_internal_t *mdi) {
        const rd_kafka_metadata_t *md = &mdi->metadata;
        rd_kafka_ClusterDescription_t *clusterdesc =
            rd_calloc(1, sizeof(*clusterdesc));
        int i;

        clusterdesc->cluster_id = rd_strdup(mdi->cluster_id);

        if (mdi->controller_id >= 0)
                clusterdesc->controller = rd_kafka_Node_new_from_brokers(
                    mdi->controller_id, mdi->brokers_sorted, mdi->brokers,
                    md->broker_cnt);

        clusterdesc->authorized_operations =
            rd_kafka_AuthorizedOperations_parse(
                mdi->cluster_authorized_operations,
                &clusterdesc->authorized_operations_cnt);

        clusterdesc->node_cnt = md->broker_cnt;
        clusterdesc->nodes =
            rd_calloc(clusterdesc->node_cnt, sizeof(rd_kafka_Node_t *));

        for (i = 0; i < md->broker_cnt; i++)
                clusterdesc->nodes[i] = rd_kafka_Node_new_from_brokers(
                    md->brokers[i].id, mdi->brokers_sorted, mdi->brokers,
                    md->broker_cnt);

        return clusterdesc;
}

static void rd_kafka_ClusterDescription_destroy(
    rd_kafka_ClusterDescription_t *clusterdesc) {
        RD_IF_FREE(clusterdesc->cluster_id, rd_free);
        RD_IF_FREE(clusterdesc->controller, rd_kafka_Node_free);
        RD_IF_FREE(clusterdesc->authorized_operations, rd_free);

        if (clusterdesc->node_cnt) {
                size_t i;
                for (i = 0; i < clusterdesc->node_cnt; i++)
                        rd_kafka_Node_free(clusterdesc->nodes[i]);
                rd_free(clusterdesc->nodes);
        }
        rd_free(clusterdesc);
}

static void rd_kafka_ClusterDescription_free(void *ptr) {
        rd_kafka_ClusterDescription_destroy(ptr);
}
/**
 * @brief Send DescribeClusterRequest. Admin worker compatible callback.
 */
static rd_kafka_resp_err_t rd_kafka_admin_DescribeClusterRequest(
    rd_kafka_broker_t *rkb,
    const rd_list_t *ignored /* We don't use any arguments set here. */,
    rd_kafka_AdminOptions_t *options,
    char *errstr,
    size_t errstr_size,
    rd_kafka_replyq_t replyq,
    rd_kafka_resp_cb_t *resp_cb,
    void *opaque) {
        rd_kafka_resp_err_t err;
        int include_cluster_authorized_operations =
            rd_kafka_confval_get_int(&options->include_authorized_operations);

        err = rd_kafka_admin_MetadataRequest(
            rkb, NULL /* topics */, "describe cluster",
            include_cluster_authorized_operations,
            rd_false /* don't include_topic_authorized_operations */,
            rd_false /* don't force racks */, resp_cb, replyq, opaque);

        if (err) {
                rd_snprintf(errstr, errstr_size, "%s", rd_kafka_err2str(err));
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Parse DescribeCluster and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_DescribeClusterResponse_parse(rd_kafka_op_t *rko_req,
                                       rd_kafka_op_t **rko_resultp,
                                       rd_kafka_buf_t *reply,
                                       char *errstr,
                                       size_t errstr_size) {
        rd_kafka_metadata_internal_t *mdi = NULL;
        rd_kafka_resp_err_t err;
        rd_kafka_ClusterDescription_t *clusterdesc = NULL;
        rd_list_t topics          = rko_req->rko_u.admin_request.args;
        rd_kafka_broker_t *rkb    = reply->rkbuf_rkb;
        rd_kafka_op_t *rko_result = NULL;

        err = rd_kafka_parse_Metadata_admin(rkb, reply, &topics, &mdi);
        if (err)
                goto err;

        rko_result = rd_kafka_admin_result_new(rko_req);
        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_ClusterDescription_free);

        clusterdesc = rd_kafka_ClusterDescription_new(mdi);

        rd_free(mdi);

        rd_list_add(&rko_result->rko_u.admin_result.results, clusterdesc);
        *rko_resultp = rko_result;
        return RD_KAFKA_RESP_ERR_NO_ERROR;

err:
        RD_IF_FREE(rko_result, rd_kafka_op_destroy);
        rd_snprintf(errstr, errstr_size,
                    "DescribeCluster response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));
        return reply->rkbuf_err;
}

void rd_kafka_DescribeCluster(rd_kafka_t *rk,
                              const rd_kafka_AdminOptions_t *options,
                              rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_admin_DescribeClusterRequest,
            rd_kafka_DescribeClusterResponse_parse};

        rko = rd_kafka_admin_request_op_new(
            rk, RD_KAFKA_OP_DESCRIBECLUSTER,
            RD_KAFKA_EVENT_DESCRIBECLUSTER_RESULT, &cbs, options, rkqu->rkqu_q);

        rd_kafka_q_enq(rk->rk_ops, rko);
}

/**@}*/

/**
 * @name ElectLeaders
 * @{
 *
 *
 *
 *
 */

/**
 * @brief Creates a new rd_kafka_ElectLeaders_t object with the given
 *        \p election_type and \p partitions.
 */
rd_kafka_ElectLeaders_t *
rd_kafka_ElectLeaders_new(rd_kafka_ElectionType_t election_type,
                          rd_kafka_topic_partition_list_t *partitions) {

        rd_kafka_ElectLeaders_t *elect_leaders;

        elect_leaders = rd_calloc(1, sizeof(*elect_leaders));
        if (partitions)
                elect_leaders->partitions =
                    rd_kafka_topic_partition_list_copy(partitions);
        elect_leaders->election_type = election_type;

        return elect_leaders;
}

rd_kafka_ElectLeaders_t *
rd_kafka_ElectLeaders_copy(const rd_kafka_ElectLeaders_t *elect_leaders) {
        return rd_kafka_ElectLeaders_new(elect_leaders->election_type,
                                         elect_leaders->partitions);
}

void rd_kafka_ElectLeaders_destroy(rd_kafka_ElectLeaders_t *elect_leaders) {
        if (elect_leaders->partitions)
                rd_kafka_topic_partition_list_destroy(
                    elect_leaders->partitions);
        rd_free(elect_leaders);
}

static void rd_kafka_ElectLeaders_free(void *ptr) {
        rd_kafka_ElectLeaders_destroy(ptr);
}

/**
 * @brief Creates a new rd_kafka_ElectLeadersResult_t object with the given
 *        \p error and \p partitions.
 */
static rd_kafka_ElectLeadersResult_t *
rd_kafka_ElectLeadersResult_new(rd_list_t *partitions) {

        rd_kafka_ElectLeadersResult_t *result;
        result = rd_calloc(1, sizeof(*result));
        rd_list_init_copy(&result->partitions, partitions);
        rd_list_copy_to(&result->partitions, partitions,
                        rd_kafka_topic_partition_result_copy_opaque, NULL);
        return result;
}

static const rd_kafka_topic_partition_result_t **
rd_kafka_ElectLeadersResult_partitions(
    const rd_kafka_ElectLeadersResult_t *result,
    size_t *cntp) {
        *cntp = rd_list_cnt(&result->partitions);
        return (const rd_kafka_topic_partition_result_t **)
            result->partitions.rl_elems;
}

static void
rd_kafka_ElectLeadersResult_destroy(rd_kafka_ElectLeadersResult_t *result) {
        rd_list_destroy(&result->partitions);
        rd_free(result);
}

static void rd_kafka_ElectLeadersResult_free(void *ptr) {
        rd_kafka_ElectLeadersResult_destroy(ptr);
}

static const rd_kafka_ElectLeadersResult_t *rd_kafka_ElectLeaders_result_result(
    const rd_kafka_ElectLeaders_result_t *result) {
        return (const rd_kafka_ElectLeadersResult_t *)rd_list_elem(
            &result->rko_u.admin_result.results, 0);
}

const rd_kafka_topic_partition_result_t **
rd_kafka_ElectLeaders_result_partitions(
    const rd_kafka_ElectLeaders_result_t *result,
    size_t *cntp) {
        return rd_kafka_ElectLeadersResult_partitions(
            rd_kafka_ElectLeaders_result_result(result), cntp);
}

/**
 * @brief Parse ElectLeadersResponse and create ADMIN_RESULT op.
 */
static rd_kafka_resp_err_t
rd_kafka_ElectLeadersResponse_parse(rd_kafka_op_t *rko_req,
                                    rd_kafka_op_t **rko_resultp,
                                    rd_kafka_buf_t *reply,
                                    char *errstr,
                                    size_t errstr_size) {
        const int log_decode_errors           = LOG_ERR;
        rd_kafka_op_t *rko_result             = NULL;
        rd_kafka_ElectLeadersResult_t *result = NULL;
        int16_t top_level_error_code          = 0;
        int32_t TopicArrayCnt;
        int partition_cnt;
        rd_list_t partitions_arr;
        rd_kafka_ElectLeaders_t *request =
            rko_req->rko_u.admin_request.args.rl_elems[0];
        int i;
        int j;

        rd_kafka_buf_read_throttle_time(reply);

        if (rd_kafka_buf_ApiVersion(reply) >= 1) {
                rd_kafka_buf_read_i16(reply, &top_level_error_code);
        }

        if (top_level_error_code) {
                rd_kafka_admin_result_fail(
                    rko_req, top_level_error_code,
                    "ElectLeaders request failed: %s",
                    rd_kafka_err2str(top_level_error_code));
                return top_level_error_code;
        }

        /* #partitions */
        rd_kafka_buf_read_arraycnt(reply, &TopicArrayCnt, RD_KAFKAP_TOPICS_MAX);

        if (request->partitions)
                partition_cnt = request->partitions->cnt;
        else
                partition_cnt = 1;
        rd_list_init(&partitions_arr, partition_cnt,
                     rd_kafka_topic_partition_result_free);
        memset(partitions_arr.rl_elems, 0,
               sizeof(*partitions_arr.rl_elems) * partition_cnt);

        for (i = 0; i < TopicArrayCnt; i++) {
                rd_kafka_topic_partition_result_t *partition_result;
                rd_kafkap_str_t ktopic;
                char *topic;
                int32_t PartArrayCnt;

                rd_kafka_buf_read_str(reply, &ktopic);
                RD_KAFKAP_STR_DUPA(&topic, &ktopic);

                rd_kafka_buf_read_arraycnt(reply, &PartArrayCnt,
                                           RD_KAFKAP_PARTITIONS_MAX);

                for (j = 0; j < PartArrayCnt; j++) {
                        int32_t partition;
                        int16_t partition_error_code;
                        rd_kafkap_str_t partition_error_msg;
                        char *partition_errstr;
                        int orig_pos;

                        rd_kafka_buf_read_i32(reply, &partition);
                        rd_kafka_buf_read_i16(reply, &partition_error_code);
                        rd_kafka_buf_read_str(reply, &partition_error_msg);

                        rd_kafka_buf_skip_tags(reply);

                        if (RD_KAFKAP_STR_IS_NULL(&partition_error_msg) ||
                            RD_KAFKAP_STR_LEN(&partition_error_msg) == 0)
                                partition_errstr = (char *)rd_kafka_err2str(
                                    partition_error_code);
                        else
                                RD_KAFKAP_STR_DUPA(&partition_errstr,
                                                   &partition_error_msg);

                        partition_result = rd_kafka_topic_partition_result_new(
                            topic, partition, partition_error_code,
                            partition_errstr);

                        if (request->partitions) {
                                orig_pos =
                                    rd_kafka_topic_partition_list_find_idx(
                                        request->partitions, topic, partition);

                                if (orig_pos == -1) {
                                        rd_kafka_buf_parse_fail(
                                            reply,
                                            "Broker returned partition %s "
                                            "[%" PRId32
                                            "] that was not "
                                            "included in the original request",
                                            topic, partition);
                                }

                                if (rd_list_elem(&partitions_arr, orig_pos) !=
                                    NULL) {
                                        rd_kafka_buf_parse_fail(
                                            reply,
                                            "Broker returned partition %s "
                                            "[%" PRId32 "] multiple times",
                                            topic, partition);
                                }

                                rd_list_set(&partitions_arr, orig_pos,
                                            partition_result);
                        } else {
                                rd_list_add(&partitions_arr, partition_result);
                        }
                }
                rd_kafka_buf_skip_tags(reply);
        }

        rd_kafka_buf_skip_tags(reply);

        result = rd_kafka_ElectLeadersResult_new(&partitions_arr);

        rko_result = rd_kafka_admin_result_new(rko_req);

        rd_list_init(&rko_result->rko_u.admin_result.results, 1,
                     rd_kafka_ElectLeadersResult_free);

        rd_list_add(&rko_result->rko_u.admin_result.results, result);

        *rko_resultp = rko_result;

        rd_list_destroy(&partitions_arr);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
err_parse:

        rd_list_destroy(&partitions_arr);

        if (rko_result)
                rd_kafka_op_destroy(rko_result);

        rd_snprintf(errstr, errstr_size,
                    "ElectLeaders response protocol parse failure: %s",
                    rd_kafka_err2str(reply->rkbuf_err));

        return reply->rkbuf_err;
}

void rd_kafka_ElectLeaders(rd_kafka_t *rk,
                           rd_kafka_ElectLeaders_t *elect_leaders,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu) {
        rd_kafka_op_t *rko;
        rd_kafka_topic_partition_list_t *copied_partitions = NULL;

        static const struct rd_kafka_admin_worker_cbs cbs = {
            rd_kafka_ElectLeadersRequest,
            rd_kafka_ElectLeadersResponse_parse,
        };

        rd_assert(rkqu);

        rko = rd_kafka_admin_request_op_new(rk, RD_KAFKA_OP_ELECTLEADERS,
                                            RD_KAFKA_EVENT_ELECTLEADERS_RESULT,
                                            &cbs, options, rkqu->rkqu_q);

        if (elect_leaders->partitions) {
                /* Duplicate topic partitions should not be present in the list
                 */
                copied_partitions = rd_kafka_topic_partition_list_copy(
                    elect_leaders->partitions);
                if (rd_kafka_topic_partition_list_has_duplicates(
                        copied_partitions, rd_false /* check partition*/)) {
                        rd_kafka_admin_result_fail(
                            rko, RD_KAFKA_RESP_ERR__INVALID_ARG,
                            "Duplicate partitions specified");
                        rd_kafka_admin_common_worker_destroy(
                            rk, rko, rd_true /*destroy*/);
                        rd_kafka_topic_partition_list_destroy(
                            copied_partitions);
                        return;
                }
        }

        rd_list_init(&rko->rko_u.admin_request.args, 1,
                     rd_kafka_ElectLeaders_free);

        rd_list_add(&rko->rko_u.admin_request.args,
                    rd_kafka_ElectLeaders_copy(elect_leaders));

        rd_kafka_q_enq(rk->rk_ops, rko);
        if (copied_partitions)
                rd_kafka_topic_partition_list_destroy(copied_partitions);
}

/**@}*/
