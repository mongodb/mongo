/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2018 Magnus Edenhill
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
 * Background queue thread and event handling.
 *
 * See rdkafka.h's rd_kafka_conf_set_background_event_cb() for details.
 */

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_event.h"
#include "rdkafka_interceptor.h"

#include <signal.h>

/**
 * @brief Call the registered background_event_cb.
 * @locality rdkafka background queue thread
 */
static RD_INLINE void rd_kafka_call_background_event_cb(rd_kafka_t *rk,
                                                        rd_kafka_op_t *rko) {
        rd_assert(!rk->rk_background.calling);
        rk->rk_background.calling = 1;

        rk->rk_conf.background_event_cb(rk, rko, rk->rk_conf.opaque);

        rk->rk_background.calling = 0;
}


/**
 * @brief Background queue handler.
 *
 * Triggers the background_event_cb for all event:able ops,
 * for non-event:able ops:
 *  - call op callback if set, else
 *  - log and discard the op. This is a user error, forwarding non-event
 *    APIs to the background queue.
 */
static rd_kafka_op_res_t
rd_kafka_background_queue_serve(rd_kafka_t *rk,
                                rd_kafka_q_t *rkq,
                                rd_kafka_op_t *rko,
                                rd_kafka_q_cb_type_t cb_type,
                                void *opaque) {
        rd_kafka_op_res_t res;

        /*
         * Dispatch Event:able ops to background_event_cb()
         */
        if (likely(rk->rk_conf.background_event_cb &&
                   rd_kafka_event_setup(rk, rko))) {
                rd_kafka_call_background_event_cb(rk, rko);
                /* Event must be destroyed by application. */
                return RD_KAFKA_OP_RES_HANDLED;
        }

        /*
         * Handle non-event:able ops through the standard poll_cb that
         * will trigger type-specific callbacks (and return OP_RES_HANDLED)
         * or do no handling and return OP_RES_PASS.
         * Also signal yield to q_serve() (which implies that op was handled).
         */
        res = rd_kafka_poll_cb(rk, rkq, rko, RD_KAFKA_Q_CB_CALLBACK, opaque);
        if (res == RD_KAFKA_OP_RES_HANDLED || res == RD_KAFKA_OP_RES_YIELD)
                return res;

        /* Op was not handled, log and destroy it. */
        rd_kafka_log(rk, LOG_NOTICE, "BGQUEUE",
                     "No support for handling "
                     "non-event op %s in background queue: discarding",
                     rd_kafka_op2str(rko->rko_type));
        rd_kafka_op_destroy(rko);

        /* Indicate that the op was handled. */
        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Main loop for background queue thread.
 */
int rd_kafka_background_thread_main(void *arg) {
        rd_kafka_t *rk = arg;

        rd_kafka_set_thread_name("background");
        rd_kafka_set_thread_sysname("rdk:bg");

        rd_kafka_interceptors_on_thread_start(rk, RD_KAFKA_THREAD_BACKGROUND);

        (void)rd_atomic32_add(&rd_kafka_thread_cnt_curr, 1);

        /* Acquire lock (which was held by thread creator during creation)
         * to synchronise state. */
        rd_kafka_wrlock(rk);
        rd_kafka_wrunlock(rk);

        mtx_lock(&rk->rk_init_lock);
        rk->rk_init_wait_cnt--;
        cnd_broadcast(&rk->rk_init_cnd);
        mtx_unlock(&rk->rk_init_lock);

        while (likely(!rd_kafka_terminating(rk))) {
                rd_kafka_q_serve(rk->rk_background.q, 10 * 1000, 0,
                                 RD_KAFKA_Q_CB_RETURN,
                                 rd_kafka_background_queue_serve, NULL);
        }

        /* Inform the user that they terminated the client before
         * all outstanding events were handled. */
        if (rd_kafka_q_len(rk->rk_background.q) > 0)
                rd_kafka_log(rk, LOG_INFO, "BGQUEUE",
                             "Purging %d unserved events from background queue",
                             rd_kafka_q_len(rk->rk_background.q));
        rd_kafka_q_disable(rk->rk_background.q);
        rd_kafka_q_purge(rk->rk_background.q);

        rd_kafka_dbg(rk, GENERIC, "BGQUEUE", "Background queue thread exiting");

        rd_kafka_interceptors_on_thread_exit(rk, RD_KAFKA_THREAD_BACKGROUND);

        rd_atomic32_sub(&rd_kafka_thread_cnt_curr, 1);

        return 0;
}


/**
 * @brief Create the background thread.
 *
 * @locks_acquired rk_init_lock
 * @locks_required rd_kafka_wrlock()
 */
rd_kafka_resp_err_t rd_kafka_background_thread_create(rd_kafka_t *rk,
                                                      char *errstr,
                                                      size_t errstr_size) {
#ifndef _WIN32
        sigset_t newset, oldset;
#endif

        if (rk->rk_background.q) {
                rd_snprintf(errstr, errstr_size,
                            "Background thread already created");
                return RD_KAFKA_RESP_ERR__CONFLICT;
        }

        rk->rk_background.q = rd_kafka_q_new(rk);

        mtx_lock(&rk->rk_init_lock);
        rk->rk_init_wait_cnt++;

#ifndef _WIN32
        /* Block all signals in newly created threads.
         * To avoid race condition we block all signals in the calling
         * thread, which the new thread will inherit its sigmask from,
         * and then restore the original sigmask of the calling thread when
         * we're done creating the thread. */
        sigemptyset(&oldset);
        sigfillset(&newset);
        if (rk->rk_conf.term_sig) {
                struct sigaction sa_term = {.sa_handler =
                                                rd_kafka_term_sig_handler};
                sigaction(rk->rk_conf.term_sig, &sa_term, NULL);
        }
        pthread_sigmask(SIG_SETMASK, &newset, &oldset);
#endif


        if ((thrd_create(&rk->rk_background.thread,
                         rd_kafka_background_thread_main, rk)) !=
            thrd_success) {
                rd_snprintf(errstr, errstr_size,
                            "Failed to create background thread: %s",
                            rd_strerror(errno));
                rd_kafka_q_destroy_owner(rk->rk_background.q);
                rk->rk_background.q = NULL;
                rk->rk_init_wait_cnt--;
                mtx_unlock(&rk->rk_init_lock);

#ifndef _WIN32
                /* Restore sigmask of caller */
                pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif
                return RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
        }

        mtx_unlock(&rk->rk_init_lock);

#ifndef _WIN32
        /* Restore sigmask of caller */
        pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}
