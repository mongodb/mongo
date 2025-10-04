/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017-2022, Magnus Edenhill
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
#include "rdkafka_interceptor.h"
#include "rdstring.h"

/**
 * @brief Interceptor methodtion/method reference
 */
typedef struct rd_kafka_interceptor_method_s {
        union {
                rd_kafka_interceptor_f_on_conf_set_t *on_conf_set;
                rd_kafka_interceptor_f_on_conf_dup_t *on_conf_dup;
                rd_kafka_interceptor_f_on_conf_destroy_t *on_conf_destroy;
                rd_kafka_interceptor_f_on_new_t *on_new;
                rd_kafka_interceptor_f_on_destroy_t *on_destroy;
                rd_kafka_interceptor_f_on_send_t *on_send;
                rd_kafka_interceptor_f_on_acknowledgement_t *on_acknowledgement;
                rd_kafka_interceptor_f_on_consume_t *on_consume;
                rd_kafka_interceptor_f_on_commit_t *on_commit;
                rd_kafka_interceptor_f_on_request_sent_t *on_request_sent;
                rd_kafka_interceptor_f_on_response_received_t
                    *on_response_received;
                rd_kafka_interceptor_f_on_thread_start_t *on_thread_start;
                rd_kafka_interceptor_f_on_thread_exit_t *on_thread_exit;
                rd_kafka_interceptor_f_on_broker_state_change_t
                    *on_broker_state_change;
                void *generic; /* For easy assignment */

        } u;
        char *ic_name;
        void *ic_opaque;
} rd_kafka_interceptor_method_t;

/**
 * @brief Destroy interceptor methodtion reference
 */
static void rd_kafka_interceptor_method_destroy(void *ptr) {
        rd_kafka_interceptor_method_t *method = ptr;
        rd_free(method->ic_name);
        rd_free(method);
}



/**
 * @brief Handle an interceptor on_... methodtion call failures.
 */
static RD_INLINE void
rd_kafka_interceptor_failed(rd_kafka_t *rk,
                            const rd_kafka_interceptor_method_t *method,
                            const char *method_name,
                            rd_kafka_resp_err_t err,
                            const rd_kafka_message_t *rkmessage,
                            const char *errstr) {

        /* FIXME: Suppress log messages, eventually */
        if (rkmessage)
                rd_kafka_log(
                    rk, LOG_WARNING, "ICFAIL",
                    "Interceptor %s failed %s for "
                    "message on %s [%" PRId32 "] @ %" PRId64 ": %s%s%s",
                    method->ic_name, method_name,
                    rd_kafka_topic_name(rkmessage->rkt), rkmessage->partition,
                    rkmessage->offset, rd_kafka_err2str(err),
                    errstr ? ": " : "", errstr ? errstr : "");
        else
                rd_kafka_log(rk, LOG_WARNING, "ICFAIL",
                             "Interceptor %s failed %s: %s%s%s",
                             method->ic_name, method_name,
                             rd_kafka_err2str(err), errstr ? ": " : "",
                             errstr ? errstr : "");
}



/**
 * @brief Create interceptor method reference.
 *        Duplicates are rejected
 */
static rd_kafka_interceptor_method_t *
rd_kafka_interceptor_method_new(const char *ic_name,
                                void *func,
                                void *ic_opaque) {
        rd_kafka_interceptor_method_t *method;

        method            = rd_calloc(1, sizeof(*method));
        method->ic_name   = rd_strdup(ic_name);
        method->ic_opaque = ic_opaque;
        method->u.generic = func;

        return method;
}


/**
 * @brief Method comparator to be used for finding, not sorting.
 */
static int rd_kafka_interceptor_method_cmp(const void *_a, const void *_b) {
        const rd_kafka_interceptor_method_t *a = _a, *b = _b;

        if (a->u.generic != b->u.generic)
                return -1;

        return strcmp(a->ic_name, b->ic_name);
}

/**
 * @brief Add interceptor method reference
 */
static rd_kafka_resp_err_t rd_kafka_interceptor_method_add(rd_list_t *list,
                                                           const char *ic_name,
                                                           void *func,
                                                           void *ic_opaque) {
        rd_kafka_interceptor_method_t *method;
        const rd_kafka_interceptor_method_t skel = {.ic_name = (char *)ic_name,
                                                    .u = {.generic = func}};

        /* Reject same method from same interceptor.
         * This is needed to avoid duplicate interceptors when configuration
         * objects are duplicated.
         * An exception is made for lists with _F_UNIQUE, which is currently
         * only on_conf_destroy() to allow interceptor cleanup. */
        if ((list->rl_flags & RD_LIST_F_UNIQUE) &&
            rd_list_find(list, &skel, rd_kafka_interceptor_method_cmp))
                return RD_KAFKA_RESP_ERR__CONFLICT;

        method = rd_kafka_interceptor_method_new(ic_name, func, ic_opaque);
        rd_list_add(list, method);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Destroy all interceptors
 * @locality application thread calling rd_kafka_conf_destroy() or
 *           rd_kafka_destroy()
 */
void rd_kafka_interceptors_destroy(rd_kafka_conf_t *conf) {
        rd_list_destroy(&conf->interceptors.on_conf_set);
        rd_list_destroy(&conf->interceptors.on_conf_dup);
        rd_list_destroy(&conf->interceptors.on_conf_destroy);
        rd_list_destroy(&conf->interceptors.on_new);
        rd_list_destroy(&conf->interceptors.on_destroy);
        rd_list_destroy(&conf->interceptors.on_send);
        rd_list_destroy(&conf->interceptors.on_acknowledgement);
        rd_list_destroy(&conf->interceptors.on_consume);
        rd_list_destroy(&conf->interceptors.on_commit);
        rd_list_destroy(&conf->interceptors.on_request_sent);
        rd_list_destroy(&conf->interceptors.on_response_received);
        rd_list_destroy(&conf->interceptors.on_thread_start);
        rd_list_destroy(&conf->interceptors.on_thread_exit);
        rd_list_destroy(&conf->interceptors.on_broker_state_change);

        /* Interceptor config */
        rd_list_destroy(&conf->interceptors.config);
}


/**
 * @brief Initialize interceptor sub-system for config object.
 * @locality application thread
 */
static void rd_kafka_interceptors_init(rd_kafka_conf_t *conf) {
        rd_list_init(&conf->interceptors.on_conf_set, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_conf_dup, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        /* conf_destroy() allows duplicates entries. */
        rd_list_init(&conf->interceptors.on_conf_destroy, 0,
                     rd_kafka_interceptor_method_destroy);
        rd_list_init(&conf->interceptors.on_new, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_destroy, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_send, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_acknowledgement, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_consume, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_commit, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_request_sent, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_response_received, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_thread_start, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_thread_exit, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;
        rd_list_init(&conf->interceptors.on_broker_state_change, 0,
                     rd_kafka_interceptor_method_destroy)
            ->rl_flags |= RD_LIST_F_UNIQUE;

        /* Interceptor config */
        rd_list_init(&conf->interceptors.config, 0,
                     (void (*)(void *))rd_strtup_destroy);
}



/**
 * @name Configuration backend
 */


/**
 * @brief Constructor called when configuration object is created.
 */
void rd_kafka_conf_interceptor_ctor(int scope, void *pconf) {
        rd_kafka_conf_t *conf = pconf;
        assert(scope == _RK_GLOBAL);
        rd_kafka_interceptors_init(conf);
}

/**
 * @brief Destructor called when configuration object is destroyed.
 */
void rd_kafka_conf_interceptor_dtor(int scope, void *pconf) {
        rd_kafka_conf_t *conf = pconf;
        assert(scope == _RK_GLOBAL);
        rd_kafka_interceptors_destroy(conf);
}

/**
 * @brief Copy-constructor called when configuration object \p psrcp is
 *        duplicated to \p dstp.
 * @remark Interceptors are NOT copied, but interceptor config is.
 *
 */
void rd_kafka_conf_interceptor_copy(int scope,
                                    void *pdst,
                                    const void *psrc,
                                    void *dstptr,
                                    const void *srcptr,
                                    size_t filter_cnt,
                                    const char **filter) {
        rd_kafka_conf_t *dconf       = pdst;
        const rd_kafka_conf_t *sconf = psrc;
        int i;
        const rd_strtup_t *confval;

        assert(scope == _RK_GLOBAL);

        /* Apply interceptor configuration values.
         * on_conf_dup() has already been called for dconf so
         * on_conf_set() interceptors are already in place and we can
         * apply the configuration through the standard conf_set() API. */
        RD_LIST_FOREACH(confval, &sconf->interceptors.config, i) {
                size_t fi;
                size_t nlen = strlen(confval->name);

                /* Apply filter */
                for (fi = 0; fi < filter_cnt; fi++) {
                        size_t flen = strlen(filter[fi]);
                        if (nlen >= flen &&
                            !strncmp(filter[fi], confval->name, flen))
                                break;
                }

                if (fi < filter_cnt)
                        continue; /* Filter matched: ignore property. */

                /* Ignore errors for now */
                rd_kafka_conf_set(dconf, confval->name, confval->value, NULL,
                                  0);
        }
}



/**
 * @brief Call interceptor on_conf_set methods.
 * @locality application thread calling rd_kafka_conf_set() and
 *           rd_kafka_conf_dup()
 */
rd_kafka_conf_res_t rd_kafka_interceptors_on_conf_set(rd_kafka_conf_t *conf,
                                                      const char *name,
                                                      const char *val,
                                                      char *errstr,
                                                      size_t errstr_size) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &conf->interceptors.on_conf_set, i) {
                rd_kafka_conf_res_t res;

                res = method->u.on_conf_set(conf, name, val, errstr,
                                            errstr_size, method->ic_opaque);
                if (res == RD_KAFKA_CONF_UNKNOWN)
                        continue;

                /* Add successfully handled properties to list of
                 * interceptor config properties so conf_t objects
                 * can be copied. */
                if (res == RD_KAFKA_CONF_OK)
                        rd_list_add(&conf->interceptors.config,
                                    rd_strtup_new(name, val));
                return res;
        }

        return RD_KAFKA_CONF_UNKNOWN;
}

/**
 * @brief Call interceptor on_conf_dup methods.
 * @locality application thread calling rd_kafka_conf_dup()
 */
void rd_kafka_interceptors_on_conf_dup(rd_kafka_conf_t *new_conf,
                                       const rd_kafka_conf_t *old_conf,
                                       size_t filter_cnt,
                                       const char **filter) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &old_conf->interceptors.on_conf_dup, i) {
                /* FIXME: Ignore error for now */
                method->u.on_conf_dup(new_conf, old_conf, filter_cnt, filter,
                                      method->ic_opaque);
        }
}


/**
 * @brief Call interceptor on_conf_destroy methods.
 * @locality application thread calling rd_kafka_conf_destroy(), rd_kafka_new(),
 *           rd_kafka_destroy()
 */
void rd_kafka_interceptors_on_conf_destroy(rd_kafka_conf_t *conf) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &conf->interceptors.on_conf_destroy, i) {
                /* FIXME: Ignore error for now */
                method->u.on_conf_destroy(method->ic_opaque);
        }
}


/**
 * @brief Call interceptor on_new methods.
 * @locality application thread calling rd_kafka_new()
 */
void rd_kafka_interceptors_on_new(rd_kafka_t *rk, const rd_kafka_conf_t *conf) {
        rd_kafka_interceptor_method_t *method;
        int i;
        char errstr[512];

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_new, i) {
                rd_kafka_resp_err_t err;

                err = method->u.on_new(rk, conf, method->ic_opaque, errstr,
                                       sizeof(errstr));
                if (unlikely(err))
                        rd_kafka_interceptor_failed(rk, method, "on_new", err,
                                                    NULL, errstr);
        }
}



/**
 * @brief Call interceptor on_destroy methods.
 * @locality application thread calling rd_kafka_new() or rd_kafka_destroy()
 */
void rd_kafka_interceptors_on_destroy(rd_kafka_t *rk) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_destroy, i) {
                rd_kafka_resp_err_t err;

                err = method->u.on_destroy(rk, method->ic_opaque);
                if (unlikely(err))
                        rd_kafka_interceptor_failed(rk, method, "on_destroy",
                                                    err, NULL, NULL);
        }
}



/**
 * @brief Call interceptor on_send methods.
 * @locality application thread calling produce()
 */
void rd_kafka_interceptors_on_send(rd_kafka_t *rk,
                                   rd_kafka_message_t *rkmessage) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_send, i) {
                rd_kafka_resp_err_t err;

                err = method->u.on_send(rk, rkmessage, method->ic_opaque);
                if (unlikely(err))
                        rd_kafka_interceptor_failed(rk, method, "on_send", err,
                                                    rkmessage, NULL);
        }
}



/**
 * @brief Call interceptor on_acknowledgement methods.
 * @locality application thread calling poll(), or the broker thread if
 *           if dr callback has been set.
 */
void rd_kafka_interceptors_on_acknowledgement(rd_kafka_t *rk,
                                              rd_kafka_message_t *rkmessage) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_acknowledgement,
                        i) {
                rd_kafka_resp_err_t err;

                err = method->u.on_acknowledgement(rk, rkmessage,
                                                   method->ic_opaque);
                if (unlikely(err))
                        rd_kafka_interceptor_failed(rk, method,
                                                    "on_acknowledgement", err,
                                                    rkmessage, NULL);
        }
}


/**
 * @brief Call on_acknowledgement methods for all messages in queue.
 *
 * @param force_err If non-zero, sets this error on each message.
 *
 * @locality broker thread
 */
void rd_kafka_interceptors_on_acknowledgement_queue(
    rd_kafka_t *rk,
    rd_kafka_msgq_t *rkmq,
    rd_kafka_resp_err_t force_err) {
        rd_kafka_msg_t *rkm;

        RD_KAFKA_MSGQ_FOREACH(rkm, rkmq) {
                if (force_err)
                        rkm->rkm_err = force_err;
                rd_kafka_interceptors_on_acknowledgement(rk,
                                                         &rkm->rkm_rkmessage);
        }
}


/**
 * @brief Call interceptor on_consume methods.
 * @locality application thread calling poll(), consume() or similar prior to
 *           passing the message to the application.
 */
void rd_kafka_interceptors_on_consume(rd_kafka_t *rk,
                                      rd_kafka_message_t *rkmessage) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_consume, i) {
                rd_kafka_resp_err_t err;

                err = method->u.on_consume(rk, rkmessage, method->ic_opaque);
                if (unlikely(err))
                        rd_kafka_interceptor_failed(rk, method, "on_consume",
                                                    err, rkmessage, NULL);
        }
}


/**
 * @brief Call interceptor on_commit methods.
 * @locality application thread calling poll(), consume() or similar,
 *           or rdkafka main thread if no commit_cb or handler registered.
 */
void rd_kafka_interceptors_on_commit(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *offsets,
    rd_kafka_resp_err_t err) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_commit, i) {
                rd_kafka_resp_err_t ic_err;

                ic_err =
                    method->u.on_commit(rk, offsets, err, method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(rk, method, "on_commit",
                                                    ic_err, NULL, NULL);
        }
}


/**
 * @brief Call interceptor on_request_sent methods
 * @locality internal broker thread
 */
void rd_kafka_interceptors_on_request_sent(rd_kafka_t *rk,
                                           int sockfd,
                                           const char *brokername,
                                           int32_t brokerid,
                                           int16_t ApiKey,
                                           int16_t ApiVersion,
                                           int32_t CorrId,
                                           size_t size) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_request_sent, i) {
                rd_kafka_resp_err_t ic_err;

                ic_err = method->u.on_request_sent(
                    rk, sockfd, brokername, brokerid, ApiKey, ApiVersion,
                    CorrId, size, method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(
                            rk, method, "on_request_sent", ic_err, NULL, NULL);
        }
}


/**
 * @brief Call interceptor on_response_received methods
 * @locality internal broker thread
 */
void rd_kafka_interceptors_on_response_received(rd_kafka_t *rk,
                                                int sockfd,
                                                const char *brokername,
                                                int32_t brokerid,
                                                int16_t ApiKey,
                                                int16_t ApiVersion,
                                                int32_t CorrId,
                                                size_t size,
                                                int64_t rtt,
                                                rd_kafka_resp_err_t err) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_response_received,
                        i) {
                rd_kafka_resp_err_t ic_err;

                ic_err = method->u.on_response_received(
                    rk, sockfd, brokername, brokerid, ApiKey, ApiVersion,
                    CorrId, size, rtt, err, method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(rk, method,
                                                    "on_response_received",
                                                    ic_err, NULL, NULL);
        }
}


void rd_kafka_interceptors_on_thread_start(rd_kafka_t *rk,
                                           rd_kafka_thread_type_t thread_type) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_thread_start, i) {
                rd_kafka_resp_err_t ic_err;

                ic_err = method->u.on_thread_start(
                    rk, thread_type, rd_kafka_thread_name, method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(
                            rk, method, "on_thread_start", ic_err, NULL, NULL);
        }
}


void rd_kafka_interceptors_on_thread_exit(rd_kafka_t *rk,
                                          rd_kafka_thread_type_t thread_type) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method, &rk->rk_conf.interceptors.on_thread_exit, i) {
                rd_kafka_resp_err_t ic_err;

                ic_err = method->u.on_thread_exit(
                    rk, thread_type, rd_kafka_thread_name, method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(
                            rk, method, "on_thread_exit", ic_err, NULL, NULL);
        }
}


/**
 * @brief Call interceptor on_broker_state_change methods.
 * @locality any.
 */
void rd_kafka_interceptors_on_broker_state_change(rd_kafka_t *rk,
                                                  int32_t broker_id,
                                                  const char *secproto,
                                                  const char *name,
                                                  int port,
                                                  const char *state) {
        rd_kafka_interceptor_method_t *method;
        int i;

        RD_LIST_FOREACH(method,
                        &rk->rk_conf.interceptors.on_broker_state_change, i) {
                rd_kafka_resp_err_t ic_err;

                ic_err = method->u.on_broker_state_change(
                    rk, broker_id, secproto, name, port, state,
                    method->ic_opaque);
                if (unlikely(ic_err))
                        rd_kafka_interceptor_failed(rk, method,
                                                    "on_broker_state_change",
                                                    ic_err, NULL, NULL);
        }
}



/**
 * @name Public API (backend)
 * @{
 */


rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_set(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_set_t *on_conf_set,
    void *ic_opaque) {
        return rd_kafka_interceptor_method_add(&conf->interceptors.on_conf_set,
                                               ic_name, (void *)on_conf_set,
                                               ic_opaque);
}

rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_dup(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_dup_t *on_conf_dup,
    void *ic_opaque) {
        return rd_kafka_interceptor_method_add(&conf->interceptors.on_conf_dup,
                                               ic_name, (void *)on_conf_dup,
                                               ic_opaque);
}

rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_destroy(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_destroy_t *on_conf_destroy,
    void *ic_opaque) {
        return rd_kafka_interceptor_method_add(
            &conf->interceptors.on_conf_destroy, ic_name,
            (void *)on_conf_destroy, ic_opaque);
}



rd_kafka_resp_err_t
rd_kafka_conf_interceptor_add_on_new(rd_kafka_conf_t *conf,
                                     const char *ic_name,
                                     rd_kafka_interceptor_f_on_new_t *on_new,
                                     void *ic_opaque) {
        return rd_kafka_interceptor_method_add(
            &conf->interceptors.on_new, ic_name, (void *)on_new, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_destroy(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_destroy_t *on_destroy,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_destroy, ic_name, (void *)on_destroy,
            ic_opaque);
}

rd_kafka_resp_err_t
rd_kafka_interceptor_add_on_send(rd_kafka_t *rk,
                                 const char *ic_name,
                                 rd_kafka_interceptor_f_on_send_t *on_send,
                                 void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_send, ic_name, (void *)on_send,
            ic_opaque);
}

rd_kafka_resp_err_t rd_kafka_interceptor_add_on_acknowledgement(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_acknowledgement_t *on_acknowledgement,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_acknowledgement, ic_name,
            (void *)on_acknowledgement, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_consume(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_consume_t *on_consume,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_consume, ic_name, (void *)on_consume,
            ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_commit(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_commit_t *on_commit,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_commit, ic_name, (void *)on_commit,
            ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_request_sent(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_request_sent_t *on_request_sent,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_request_sent, ic_name,
            (void *)on_request_sent, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_response_received(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_response_received_t *on_response_received,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_response_received, ic_name,
            (void *)on_response_received, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_thread_start(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_thread_start_t *on_thread_start,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_thread_start, ic_name,
            (void *)on_thread_start, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_thread_exit(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_thread_exit_t *on_thread_exit,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_thread_exit, ic_name,
            (void *)on_thread_exit, ic_opaque);
}


rd_kafka_resp_err_t rd_kafka_interceptor_add_on_broker_state_change(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_broker_state_change_t *on_broker_state_change,
    void *ic_opaque) {
        assert(!rk->rk_initialized);
        return rd_kafka_interceptor_method_add(
            &rk->rk_conf.interceptors.on_broker_state_change, ic_name,
            (void *)on_broker_state_change, ic_opaque);
}
