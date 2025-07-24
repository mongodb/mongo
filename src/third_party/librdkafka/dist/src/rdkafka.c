/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
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


#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#if !_WIN32
#include <sys/types.h>
#include <dirent.h>
#endif

#include "rdkafka_int.h"
#include "rdkafka_msg.h"
#include "rdkafka_broker.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_offset.h"
#include "rdkafka_telemetry.h"
#include "rdkafka_transport.h"
#include "rdkafka_cgrp.h"
#include "rdkafka_assignor.h"
#include "rdkafka_request.h"
#include "rdkafka_event.h"
#include "rdkafka_error.h"
#include "rdkafka_sasl.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_sasl_oauthbearer.h"
#if WITH_OAUTHBEARER_OIDC
#include "rdkafka_sasl_oauthbearer_oidc.h"
#endif
#if WITH_SSL
#include "rdkafka_ssl.h"
#endif

#include "rdtime.h"
#include "rdmap.h"
#include "crc32c.h"
#include "rdunittest.h"

#ifdef _WIN32
#include <sys/types.h>
#include <sys/timeb.h>
#endif

#define CJSON_HIDE_SYMBOLS
#include "cJSON.h"

#if WITH_CURL
#include "rdhttp.h"
#endif


static once_flag rd_kafka_global_init_once  = ONCE_FLAG_INIT;
static once_flag rd_kafka_global_srand_once = ONCE_FLAG_INIT;

/**
 * @brief Global counter+lock for all active librdkafka instances
 */
mtx_t rd_kafka_global_lock;
int rd_kafka_global_cnt;


/**
 * Last API error code, per thread.
 * Shared among all rd_kafka_t instances.
 */
rd_kafka_resp_err_t RD_TLS rd_kafka_last_error_code;


/**
 * Current number of threads created by rdkafka.
 * This is used in regression tests.
 */
rd_atomic32_t rd_kafka_thread_cnt_curr;
int rd_kafka_thread_cnt(void) {
        return rd_atomic32_get(&rd_kafka_thread_cnt_curr);
}

/**
 * Current thread's log name (TLS)
 */
char RD_TLS rd_kafka_thread_name[64] = "app";

void rd_kafka_set_thread_name(const char *fmt, ...) {
        va_list ap;

        va_start(ap, fmt);
        rd_vsnprintf(rd_kafka_thread_name, sizeof(rd_kafka_thread_name), fmt,
                     ap);
        va_end(ap);
}

/**
 * @brief Current thread's system name (TLS)
 *
 * Note the name must be 15 characters or less, because it is passed to
 * pthread_setname_np on Linux which imposes this limit.
 */
static char RD_TLS rd_kafka_thread_sysname[16] = "app";

void rd_kafka_set_thread_sysname(const char *fmt, ...) {
        va_list ap;

        va_start(ap, fmt);
        rd_vsnprintf(rd_kafka_thread_sysname, sizeof(rd_kafka_thread_sysname),
                     fmt, ap);
        va_end(ap);

        thrd_setname(rd_kafka_thread_sysname);
}

static void rd_kafka_global_init0(void) {
        cJSON_Hooks json_hooks = {.malloc_fn = rd_malloc, .free_fn = rd_free};

        mtx_init(&rd_kafka_global_lock, mtx_plain);
#if ENABLE_DEVEL
        rd_atomic32_init(&rd_kafka_op_cnt, 0);
#endif
        rd_crc32c_global_init();
#if WITH_SSL
        /* The configuration interface might need to use
         * OpenSSL to parse keys, prior to any rd_kafka_t
         * object has been created. */
        rd_kafka_ssl_init();
#endif

        cJSON_InitHooks(&json_hooks);

#if WITH_CURL
        rd_http_global_init();
#endif
}

/**
 * @brief Initialize once per process
 */
void rd_kafka_global_init(void) {
        call_once(&rd_kafka_global_init_once, rd_kafka_global_init0);
}


/**
 * @brief Seed the PRNG with current_time.milliseconds
 */
static void rd_kafka_global_srand(void) {
        struct timeval tv;

        rd_gettimeofday(&tv, NULL);

        srand((unsigned int)(tv.tv_usec / 1000));
}


/**
 * @returns the current number of active librdkafka instances
 */
static int rd_kafka_global_cnt_get(void) {
        int r;
        mtx_lock(&rd_kafka_global_lock);
        r = rd_kafka_global_cnt;
        mtx_unlock(&rd_kafka_global_lock);
        return r;
}


/**
 * @brief Increase counter for active librdkafka instances.
 * If this is the first instance the global constructors will be called, if any.
 */
static void rd_kafka_global_cnt_incr(void) {
        mtx_lock(&rd_kafka_global_lock);
        rd_kafka_global_cnt++;
        if (rd_kafka_global_cnt == 1) {
                rd_kafka_transport_init();
#if WITH_SSL
                rd_kafka_ssl_init();
#endif
                rd_kafka_sasl_global_init();
        }
        mtx_unlock(&rd_kafka_global_lock);
}

/**
 * @brief Decrease counter for active librdkafka instances.
 * If this counter reaches 0 the global destructors will be called, if any.
 */
static void rd_kafka_global_cnt_decr(void) {
        mtx_lock(&rd_kafka_global_lock);
        rd_kafka_assert(NULL, rd_kafka_global_cnt > 0);
        rd_kafka_global_cnt--;
        if (rd_kafka_global_cnt == 0) {
                rd_kafka_sasl_global_term();
#if WITH_SSL
                rd_kafka_ssl_term();
#endif
        }
        mtx_unlock(&rd_kafka_global_lock);
}


/**
 * Wait for all rd_kafka_t objects to be destroyed.
 * Returns 0 if all kafka objects are now destroyed, or -1 if the
 * timeout was reached.
 */
int rd_kafka_wait_destroyed(int timeout_ms) {
        rd_ts_t timeout = rd_clock() + (timeout_ms * 1000);

        while (rd_kafka_thread_cnt() > 0 || rd_kafka_global_cnt_get() > 0) {
                if (rd_clock() >= timeout) {
                        rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__TIMED_OUT,
                                                ETIMEDOUT);
                        return -1;
                }
                rd_usleep(25000, NULL); /* 25ms */
        }

        return 0;
}

static void rd_kafka_log_buf(const rd_kafka_conf_t *conf,
                             const rd_kafka_t *rk,
                             int level,
                             int ctx,
                             const char *fac,
                             const char *buf) {
        if (level > conf->log_level)
                return;
        else if (rk && conf->log_queue) {
                rd_kafka_op_t *rko;

                if (!rk->rk_logq)
                        return; /* Terminating */

                rko = rd_kafka_op_new(RD_KAFKA_OP_LOG);
                rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_MEDIUM);
                rko->rko_u.log.level = level;
                rd_strlcpy(rko->rko_u.log.fac, fac, sizeof(rko->rko_u.log.fac));
                rko->rko_u.log.str = rd_strdup(buf);
                rko->rko_u.log.ctx = ctx;
                rd_kafka_q_enq(rk->rk_logq, rko);

        } else if (conf->log_cb) {
                conf->log_cb(rk, level, fac, buf);
        }
}

/**
 * @brief Logger
 *
 * @remark conf must be set, but rk may be NULL
 */
void rd_kafka_log0(const rd_kafka_conf_t *conf,
                   const rd_kafka_t *rk,
                   const char *extra,
                   int level,
                   int ctx,
                   const char *fac,
                   const char *fmt,
                   ...) {
        char buf[2048];
        va_list ap;
        unsigned int elen = 0;
        unsigned int of   = 0;

        if (level > conf->log_level)
                return;

        if (conf->log_thread_name) {
                elen = rd_snprintf(buf, sizeof(buf),
                                   "[thrd:%s]: ", rd_kafka_thread_name);
                if (unlikely(elen >= sizeof(buf)))
                        elen = sizeof(buf);
                of = elen;
        }

        if (extra) {
                elen = rd_snprintf(buf + of, sizeof(buf) - of, "%s: ", extra);
                if (unlikely(elen >= sizeof(buf) - of))
                        elen = sizeof(buf) - of;
                of += elen;
        }

        va_start(ap, fmt);
        rd_vsnprintf(buf + of, sizeof(buf) - of, fmt, ap);
        va_end(ap);

        rd_kafka_log_buf(conf, rk, level, ctx, fac, buf);
}

rd_kafka_resp_err_t
rd_kafka_oauthbearer_set_token(rd_kafka_t *rk,
                               const char *token_value,
                               int64_t md_lifetime_ms,
                               const char *md_principal_name,
                               const char **extensions,
                               size_t extension_size,
                               char *errstr,
                               size_t errstr_size) {
#if WITH_SASL_OAUTHBEARER
        return rd_kafka_oauthbearer_set_token0(
            rk, token_value, md_lifetime_ms, md_principal_name, extensions,
            extension_size, errstr, errstr_size);
#else
        rd_snprintf(errstr, errstr_size,
                    "librdkafka not built with SASL OAUTHBEARER support");
        return RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
#endif
}

rd_kafka_resp_err_t rd_kafka_oauthbearer_set_token_failure(rd_kafka_t *rk,
                                                           const char *errstr) {
#if WITH_SASL_OAUTHBEARER
        return rd_kafka_oauthbearer_set_token_failure0(rk, errstr);
#else
        return RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;
#endif
}

void rd_kafka_log_print(const rd_kafka_t *rk,
                        int level,
                        const char *fac,
                        const char *buf) {
        int secs, msecs;
        struct timeval tv;
        rd_gettimeofday(&tv, NULL);
        secs  = (int)tv.tv_sec;
        msecs = (int)(tv.tv_usec / 1000);
        fprintf(stderr, "%%%i|%u.%03u|%s|%s| %s\n", level, secs, msecs, fac,
                rk ? rk->rk_name : "", buf);
}

void rd_kafka_log_syslog(const rd_kafka_t *rk,
                         int level,
                         const char *fac,
                         const char *buf) {
#if WITH_SYSLOG
        static int initialized = 0;

        if (!initialized)
                openlog("rdkafka", LOG_PID | LOG_CONS, LOG_USER);

        syslog(level, "%s: %s: %s", fac, rk ? rk->rk_name : "", buf);
#else
        rd_assert(!*"syslog support not enabled in this build");
#endif
}

void rd_kafka_set_logger(rd_kafka_t *rk,
                         void (*func)(const rd_kafka_t *rk,
                                      int level,
                                      const char *fac,
                                      const char *buf)) {
#if !WITH_SYSLOG
        if (func == rd_kafka_log_syslog)
                rd_assert(!*"syslog support not enabled in this build");
#endif
        rk->rk_conf.log_cb = func;
}

void rd_kafka_set_log_level(rd_kafka_t *rk, int level) {
        rk->rk_conf.log_level = level;
}



#define _ERR_DESC(ENUM, DESC)                                                  \
        [ENUM - RD_KAFKA_RESP_ERR__BEGIN] = {ENUM, &(#ENUM)[18] /*pfx*/, DESC}

static const struct rd_kafka_err_desc rd_kafka_err_descs[] = {
    _ERR_DESC(RD_KAFKA_RESP_ERR__BEGIN, NULL),
    _ERR_DESC(RD_KAFKA_RESP_ERR__BAD_MSG, "Local: Bad message format"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__BAD_COMPRESSION,
              "Local: Invalid compressed data"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__DESTROY, "Local: Broker handle destroyed"),
    _ERR_DESC(
        RD_KAFKA_RESP_ERR__FAIL,
        "Local: Communication failure with broker"),  // FIXME: too specific
    _ERR_DESC(RD_KAFKA_RESP_ERR__TRANSPORT, "Local: Broker transport failure"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE,
              "Local: Critical system resource failure"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__RESOLVE, "Local: Host resolution failure"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__MSG_TIMED_OUT, "Local: Message timed out"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__PARTITION_EOF, "Broker: No more messages"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION, "Local: Unknown partition"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__FS, "Local: File or filesystem error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC, "Local: Unknown topic"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN,
              "Local: All broker connections are down"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__INVALID_ARG,
              "Local: Invalid argument or configuration"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__TIMED_OUT, "Local: Timed out"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__QUEUE_FULL, "Local: Queue full"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__ISR_INSUFF, "Local: ISR count insufficient"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NODE_UPDATE, "Local: Broker node update"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__SSL, "Local: SSL error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__WAIT_COORD, "Local: Waiting for coordinator"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNKNOWN_GROUP, "Local: Unknown group"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__IN_PROGRESS, "Local: Operation in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS,
              "Local: Previous operation in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__EXISTING_SUBSCRIPTION,
              "Local: Existing subscription"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS, "Local: Assign partitions"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS, "Local: Revoke partitions"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__CONFLICT, "Local: Conflicting use"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__STATE, "Local: Erroneous state"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNKNOWN_PROTOCOL, "Local: Unknown protocol"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED, "Local: Not implemented"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__AUTHENTICATION,
              "Local: Authentication failure"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NO_OFFSET, "Local: No offset stored"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__OUTDATED, "Local: Outdated"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE, "Local: Timed out in queue"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE,
              "Local: Required feature not supported by broker"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__WAIT_CACHE, "Local: Awaiting cache update"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__INTR, "Local: Operation interrupted"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__KEY_SERIALIZATION,
              "Local: Key serialization error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__VALUE_SERIALIZATION,
              "Local: Value serialization error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__KEY_DESERIALIZATION,
              "Local: Key deserialization error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__VALUE_DESERIALIZATION,
              "Local: Value deserialization error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__PARTIAL, "Local: Partial response"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__READ_ONLY, "Local: Read-only object"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NOENT, "Local: No such entry"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNDERFLOW, "Local: Read underflow"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__INVALID_TYPE, "Local: Invalid type"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__RETRY, "Local: Retry operation"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__PURGE_QUEUE, "Local: Purged in queue"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__PURGE_INFLIGHT, "Local: Purged in flight"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__FATAL, "Local: Fatal error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__INCONSISTENT, "Local: Inconsistent state"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__GAPLESS_GUARANTEE,
              "Local: Gap-less ordering would not be guaranteed "
              "if proceeding"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__MAX_POLL_EXCEEDED,
              "Local: Maximum application poll interval "
              "(max.poll.interval.ms) exceeded"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__UNKNOWN_BROKER, "Local: Unknown broker"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NOT_CONFIGURED,
              "Local: Functionality not configured"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__FENCED,
              "Local: This instance has been fenced by a newer instance"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__APPLICATION,
              "Local: Application generated error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__ASSIGNMENT_LOST,
              "Local: Group partition assignment lost"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__NOOP, "Local: No operation performed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__AUTO_OFFSET_RESET,
              "Local: No offset to automatically reset to"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__LOG_TRUNCATION,
              "Local: Partition log truncation detected"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__INVALID_DIFFERENT_RECORD,
              "Local: an invalid record in the same batch caused "
              "the failure of this message too."),

    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN, "Unknown broker error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NO_ERROR, "Success"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_OFFSET_OUT_OF_RANGE,
              "Broker: Offset out of range"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_MSG, "Broker: Invalid message"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART,
              "Broker: Unknown topic or partition"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_MSG_SIZE,
              "Broker: Invalid message size"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE,
              "Broker: Leader not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION,
              "Broker: Not leader for partition"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT, "Broker: Request timed out"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE,
              "Broker: Broker not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_REPLICA_NOT_AVAILABLE,
              "Broker: Replica not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE,
              "Broker: Message size too large"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_STALE_CTRL_EPOCH,
              "Broker: StaleControllerEpochCode"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_OFFSET_METADATA_TOO_LARGE,
              "Broker: Offset metadata string too large"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION,
              "Broker: Broker disconnected before response received"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS,
              "Broker: Coordinator load in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE,
              "Broker: Coordinator not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NOT_COORDINATOR, "Broker: Not coordinator"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION, "Broker: Invalid topic"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_RECORD_LIST_TOO_LARGE,
              "Broker: Message batch larger than configured server "
              "segment size"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS,
              "Broker: Not enough in-sync replicas"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND,
              "Broker: Message(s) written to insufficient number of "
              "in-sync replicas"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_REQUIRED_ACKS,
              "Broker: Invalid required acks value"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION,
              "Broker: Specified group generation id is not valid"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INCONSISTENT_GROUP_PROTOCOL,
              "Broker: Inconsistent group protocol"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_GROUP_ID, "Broker: Invalid group.id"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID, "Broker: Unknown member"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_SESSION_TIMEOUT,
              "Broker: Invalid session timeout"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS,
              "Broker: Group rebalance in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_COMMIT_OFFSET_SIZE,
              "Broker: Commit offset data size is not valid"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED,
              "Broker: Topic authorization failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED,
              "Broker: Group authorization failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED,
              "Broker: Cluster authorization failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_TIMESTAMP, "Broker: Invalid timestamp"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM,
              "Broker: Unsupported SASL mechanism"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE,
              "Broker: Request not valid in current SASL state"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION,
              "Broker: API version not supported"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS,
              "Broker: Topic already exists"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_PARTITIONS,
              "Broker: Invalid number of partitions"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_REPLICATION_FACTOR,
              "Broker: Invalid replication factor"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_REPLICA_ASSIGNMENT,
              "Broker: Invalid replica assignment"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_CONFIG,
              "Broker: Configuration is invalid"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NOT_CONTROLLER,
              "Broker: Not controller for cluster"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_REQUEST, "Broker: Invalid request"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSUPPORTED_FOR_MESSAGE_FORMAT,
              "Broker: Message format on broker does not support request"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_POLICY_VIOLATION, "Broker: Policy violation"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER,
              "Broker: Broker received an out of order sequence number"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DUPLICATE_SEQUENCE_NUMBER,
              "Broker: Broker received a duplicate sequence number"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH,
              "Broker: Producer attempted an operation with an old epoch"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_TXN_STATE,
              "Broker: Producer attempted a transactional operation in "
              "an invalid state"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_PRODUCER_ID_MAPPING,
              "Broker: Producer attempted to use a producer id which is "
              "not currently assigned to its transactional id"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_TRANSACTION_TIMEOUT,
              "Broker: Transaction timeout is larger than the maximum "
              "value allowed by the broker's max.transaction.timeout.ms"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS,
              "Broker: Producer attempted to update a transaction while "
              "another concurrent operation on the same transaction was "
              "ongoing"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TRANSACTION_COORDINATOR_FENCED,
              "Broker: Indicates that the transaction coordinator sending "
              "a WriteTxnMarker is no longer the current coordinator for "
              "a given producer"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED,
              "Broker: Transactional Id authorization failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_SECURITY_DISABLED,
              "Broker: Security features are disabled"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_OPERATION_NOT_ATTEMPTED,
              "Broker: Operation not attempted"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR,
              "Broker: Disk error when trying to access log file on disk"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_LOG_DIR_NOT_FOUND,
              "Broker: The user-specified log directory is not found "
              "in the broker config"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED,
              "Broker: SASL Authentication failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID,
              "Broker: Unknown Producer Id"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_REASSIGNMENT_IN_PROGRESS,
              "Broker: Partition reassignment is in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTH_DISABLED,
              "Broker: Delegation Token feature is not enabled"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_NOT_FOUND,
              "Broker: Delegation Token is not found on server"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_OWNER_MISMATCH,
              "Broker: Specified Principal is not valid Owner/Renewer"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_REQUEST_NOT_ALLOWED,
              "Broker: Delegation Token requests are not allowed on "
              "this connection"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED,
              "Broker: Delegation Token authorization failed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_EXPIRED,
              "Broker: Delegation Token is expired"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_PRINCIPAL_TYPE,
              "Broker: Supplied principalType is not supported"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NON_EMPTY_GROUP,
              "Broker: The group is not empty"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_GROUP_ID_NOT_FOUND,
              "Broker: The group id does not exist"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_FETCH_SESSION_ID_NOT_FOUND,
              "Broker: The fetch session ID was not found"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_FETCH_SESSION_EPOCH,
              "Broker: The fetch session epoch is invalid"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_LISTENER_NOT_FOUND,
              "Broker: No matching listener"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TOPIC_DELETION_DISABLED,
              "Broker: Topic deletion is disabled"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_FENCED_LEADER_EPOCH,
              "Broker: Leader epoch is older than broker epoch"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_LEADER_EPOCH,
              "Broker: Leader epoch is newer than broker epoch"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSUPPORTED_COMPRESSION_TYPE,
              "Broker: Unsupported compression type"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_STALE_BROKER_EPOCH,
              "Broker: Broker epoch has changed"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_OFFSET_NOT_AVAILABLE,
              "Broker: Leader high watermark is not caught up"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_MEMBER_ID_REQUIRED,
              "Broker: Group member needs a valid member ID"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_PREFERRED_LEADER_NOT_AVAILABLE,
              "Broker: Preferred leader was not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_GROUP_MAX_SIZE_REACHED,
              "Broker: Consumer group has reached maximum size"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID,
              "Broker: Static consumer fenced by other consumer with same "
              "group.instance.id"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_ELIGIBLE_LEADERS_NOT_AVAILABLE,
              "Broker: Eligible partition leaders are not available"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_ELECTION_NOT_NEEDED,
              "Broker: Leader election not needed for topic partition"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_NO_REASSIGNMENT_IN_PROGRESS,
              "Broker: No partition reassignment is in progress"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_GROUP_SUBSCRIBED_TO_TOPIC,
              "Broker: Deleting offsets of a topic while the consumer "
              "group is subscribed to it"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_RECORD,
              "Broker: Broker failed to validate record"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSTABLE_OFFSET_COMMIT,
              "Broker: There are unstable offsets that need to be cleared"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_THROTTLING_QUOTA_EXCEEDED,
              "Broker: Throttling quota has been exceeded"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_PRODUCER_FENCED,
              "Broker: There is a newer producer with the same "
              "transactionalId which fences the current one"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_RESOURCE_NOT_FOUND,
              "Broker: Request illegally referred to resource that "
              "does not exist"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_DUPLICATE_RESOURCE,
              "Broker: Request illegally referred to the same resource "
              "twice"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNACCEPTABLE_CREDENTIAL,
              "Broker: Requested credential would not meet criteria for "
              "acceptability"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INCONSISTENT_VOTER_SET,
              "Broker: Indicates that the either the sender or recipient "
              "of a voter-only request is not one of the expected voters"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_INVALID_UPDATE_VERSION,
              "Broker: Invalid update version"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_FEATURE_UPDATE_FAILED,
              "Broker: Unable to update finalized features due to "
              "server error"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_PRINCIPAL_DESERIALIZATION_FAILURE,
              "Broker: Request principal deserialization failed during "
              "forwarding"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_ID, "Broker: Unknown topic id"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_FENCED_MEMBER_EPOCH,
              "Broker: The member epoch is fenced by the group coordinator"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNRELEASED_INSTANCE_ID,
              "Broker: The instance ID is still used by another member in the "
              "consumer group"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNSUPPORTED_ASSIGNOR,
              "Broker: The assignor or its version range is not supported by "
              "the consumer group"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_STALE_MEMBER_EPOCH,
              "Broker: The member epoch is stale"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_UNKNOWN_SUBSCRIPTION_ID,
              "Broker: Client sent a push telemetry request with an invalid or "
              "outdated subscription ID"),
    _ERR_DESC(RD_KAFKA_RESP_ERR_TELEMETRY_TOO_LARGE,
              "Broker: Client sent a push telemetry request larger than the "
              "maximum size the broker will accept"),
    _ERR_DESC(RD_KAFKA_RESP_ERR__END, NULL)};


void rd_kafka_get_err_descs(const struct rd_kafka_err_desc **errdescs,
                            size_t *cntp) {
        *errdescs = rd_kafka_err_descs;
        *cntp     = RD_ARRAYSIZE(rd_kafka_err_descs);
}


const char *rd_kafka_err2str(rd_kafka_resp_err_t err) {
        static RD_TLS char ret[32];
        int idx = err - RD_KAFKA_RESP_ERR__BEGIN;

        if (unlikely(err <= RD_KAFKA_RESP_ERR__BEGIN ||
                     err >= RD_KAFKA_RESP_ERR_END_ALL ||
                     !rd_kafka_err_descs[idx].desc)) {
                rd_snprintf(ret, sizeof(ret), "Err-%i?", err);
                return ret;
        }

        return rd_kafka_err_descs[idx].desc;
}


const char *rd_kafka_err2name(rd_kafka_resp_err_t err) {
        static RD_TLS char ret[32];
        int idx = err - RD_KAFKA_RESP_ERR__BEGIN;

        if (unlikely(err <= RD_KAFKA_RESP_ERR__BEGIN ||
                     err >= RD_KAFKA_RESP_ERR_END_ALL ||
                     !rd_kafka_err_descs[idx].desc)) {
                rd_snprintf(ret, sizeof(ret), "ERR_%i?", err);
                return ret;
        }

        return rd_kafka_err_descs[idx].name;
}


rd_kafka_resp_err_t rd_kafka_last_error(void) {
        return rd_kafka_last_error_code;
}


rd_kafka_resp_err_t rd_kafka_errno2err(int errnox) {
        switch (errnox) {
        case EINVAL:
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        case EBUSY:
                return RD_KAFKA_RESP_ERR__CONFLICT;

        case ENOENT:
                return RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC;

        case ESRCH:
                return RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;

        case ETIMEDOUT:
                return RD_KAFKA_RESP_ERR__TIMED_OUT;

        case EMSGSIZE:
                return RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE;

        case ENOBUFS:
                return RD_KAFKA_RESP_ERR__QUEUE_FULL;

        case ECANCELED:
                return RD_KAFKA_RESP_ERR__FATAL;

        default:
                return RD_KAFKA_RESP_ERR__FAIL;
        }
}


rd_kafka_resp_err_t
rd_kafka_fatal_error(rd_kafka_t *rk, char *errstr, size_t errstr_size) {
        rd_kafka_resp_err_t err;

        if (unlikely((err = rd_atomic32_get(&rk->rk_fatal.err)))) {
                rd_kafka_rdlock(rk);
                rd_snprintf(errstr, errstr_size, "%s", rk->rk_fatal.errstr);
                rd_kafka_rdunlock(rk);
        }

        return err;
}


/**
 * @brief Set's the fatal error for this instance.
 *
 * @param do_lock RD_DO_LOCK: rd_kafka_wrlock() will be acquired and released,
 *                RD_DONT_LOCK: caller must hold rd_kafka_wrlock().
 *
 * @returns 1 if the error was set, or 0 if a previous fatal error
 *          has already been set on this instance.
 *
 * @locality any
 * @locks none
 */
int rd_kafka_set_fatal_error0(rd_kafka_t *rk,
                              rd_dolock_t do_lock,
                              rd_kafka_resp_err_t err,
                              const char *fmt,
                              ...) {
        va_list ap;
        char buf[512];

        if (do_lock)
                rd_kafka_wrlock(rk);
        rk->rk_fatal.cnt++;
        if (rd_atomic32_get(&rk->rk_fatal.err)) {
                if (do_lock)
                        rd_kafka_wrunlock(rk);
                rd_kafka_dbg(rk, GENERIC, "FATAL",
                             "Suppressing subsequent fatal error: %s",
                             rd_kafka_err2name(err));
                return 0;
        }

        rd_atomic32_set(&rk->rk_fatal.err, err);

        va_start(ap, fmt);
        rd_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        rk->rk_fatal.errstr = rd_strdup(buf);

        if (do_lock)
                rd_kafka_wrunlock(rk);

        /* If there is an error callback or event handler we
         * also log the fatal error as it happens.
         * If there is no error callback the error event
         * will be automatically logged, and this check here
         * prevents us from duplicate logs. */
        if (rk->rk_conf.enabled_events & RD_KAFKA_EVENT_ERROR)
                rd_kafka_log(rk, LOG_EMERG, "FATAL", "Fatal error: %s: %s",
                             rd_kafka_err2str(err), rk->rk_fatal.errstr);
        else
                rd_kafka_dbg(rk, ALL, "FATAL", "Fatal error: %s: %s",
                             rd_kafka_err2str(err), rk->rk_fatal.errstr);

        /* Indicate to the application that a fatal error was raised,
         * the app should use rd_kafka_fatal_error() to extract the
         * fatal error code itself.
         * For the high-level consumer we propagate the error as a
         * consumer error so it is returned from consumer_poll(),
         * while for all other client types (the producer) we propagate to
         * the standard error handler (typically error_cb). */
        if (rk->rk_type == RD_KAFKA_CONSUMER && rk->rk_cgrp)
                rd_kafka_consumer_err(
                    rk->rk_cgrp->rkcg_q, RD_KAFKA_NODEID_UA,
                    RD_KAFKA_RESP_ERR__FATAL, 0, NULL, NULL,
                    RD_KAFKA_OFFSET_INVALID, "Fatal error: %s: %s",
                    rd_kafka_err2str(err), rk->rk_fatal.errstr);
        else
                rd_kafka_op_err(rk, RD_KAFKA_RESP_ERR__FATAL,
                                "Fatal error: %s: %s", rd_kafka_err2str(err),
                                rk->rk_fatal.errstr);


        /* Tell rdkafka main thread to purge producer queues, but not
         * in-flight since we'll want proper delivery status for transmitted
         * requests.
         * Need NON_BLOCKING to avoid dead-lock if user is
         * calling purge() at the same time, which could be
         * waiting for this broker thread to handle its
         * OP_PURGE request. */
        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                rd_kafka_op_t *rko = rd_kafka_op_new(RD_KAFKA_OP_PURGE);
                rko->rko_u.purge.flags =
                    RD_KAFKA_PURGE_F_QUEUE | RD_KAFKA_PURGE_F_NON_BLOCKING;
                rd_kafka_q_enq(rk->rk_ops, rko);
        }

        return 1;
}


/**
 * @returns a copy of the current fatal error, if any, else NULL.
 *
 * @locks_acquired rd_kafka_rdlock(rk)
 */
rd_kafka_error_t *rd_kafka_get_fatal_error(rd_kafka_t *rk) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err;

        if (!(err = rd_atomic32_get(&rk->rk_fatal.err)))
                return NULL; /* No fatal error raised */

        rd_kafka_rdlock(rk);
        error = rd_kafka_error_new_fatal(err, "%s", rk->rk_fatal.errstr);
        rd_kafka_rdunlock(rk);

        return error;
}


rd_kafka_resp_err_t rd_kafka_test_fatal_error(rd_kafka_t *rk,
                                              rd_kafka_resp_err_t err,
                                              const char *reason) {
        if (!rd_kafka_set_fatal_error(rk, err, "test_fatal_error: %s", reason))
                return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS;
        else
                return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @brief Final destructor for rd_kafka_t, must only be called with refcnt 0.
 *
 * @locality application thread
 */
void rd_kafka_destroy_final(rd_kafka_t *rk) {

        rd_kafka_assert(rk, rd_kafka_terminating(rk));

        /* Synchronize state */
        rd_kafka_wrlock(rk);
        rd_kafka_wrunlock(rk);

        rd_kafka_telemetry_clear(rk, rd_true /*clear_control_flow_fields*/);

        /* Terminate SASL provider */
        if (rk->rk_conf.sasl.provider)
                rd_kafka_sasl_term(rk);

        rd_kafka_timers_destroy(&rk->rk_timers);

        rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Destroying op queues");

        /* Destroy cgrp */
        if (rk->rk_cgrp) {
                rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Destroying cgrp");
                /* Reset queue forwarding (rep -> cgrp) */
                rd_kafka_q_fwd_set(rk->rk_rep, NULL);
                rd_kafka_cgrp_destroy_final(rk->rk_cgrp);
        }

        rd_kafka_assignors_term(rk);

        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                rd_kafka_assignment_destroy(rk);
                if (rk->rk_consumer.q)
                        rd_kafka_q_destroy(rk->rk_consumer.q);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency);
                rd_avg_destroy(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency);
        }

        /* Purge op-queues */
        rd_kafka_q_destroy_owner(rk->rk_rep);
        rd_kafka_q_destroy_owner(rk->rk_ops);

#if WITH_SSL
        if (rk->rk_conf.ssl.ctx) {
                rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Destroying SSL CTX");
                rd_kafka_ssl_ctx_term(rk);
        }
        rd_list_destroy(&rk->rk_conf.ssl.loaded_providers);
#endif

        /* It is not safe to log after this point. */
        rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                     "Termination done: freeing resources");

        if (rk->rk_logq) {
                rd_kafka_q_destroy_owner(rk->rk_logq);
                rk->rk_logq = NULL;
        }

        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                cnd_destroy(&rk->rk_curr_msgs.cnd);
                mtx_destroy(&rk->rk_curr_msgs.lock);
        }

        if (rk->rk_fatal.errstr) {
                rd_free(rk->rk_fatal.errstr);
                rk->rk_fatal.errstr = NULL;
        }

        cnd_destroy(&rk->rk_broker_state_change_cnd);
        mtx_destroy(&rk->rk_broker_state_change_lock);

        mtx_destroy(&rk->rk_suppress.sparse_connect_lock);

        cnd_destroy(&rk->rk_init_cnd);
        mtx_destroy(&rk->rk_init_lock);

        if (rk->rk_full_metadata)
                rd_kafka_metadata_destroy(&rk->rk_full_metadata->metadata);
        rd_kafkap_str_destroy(rk->rk_client_id);
        rd_kafkap_str_destroy(rk->rk_group_id);
        rd_kafkap_str_destroy(rk->rk_eos.transactional_id);
        rd_kafka_anyconf_destroy(_RK_GLOBAL, &rk->rk_conf);
        rd_list_destroy(&rk->rk_broker_by_id);

        mtx_destroy(&rk->rk_conf.sasl.lock);
        rwlock_destroy(&rk->rk_lock);

        rd_free(rk);
        rd_kafka_global_cnt_decr();
}


static void rd_kafka_destroy_app(rd_kafka_t *rk, int flags) {
        thrd_t thrd;
#ifndef _WIN32
        int term_sig = rk->rk_conf.term_sig;
#endif
        int res;
        char flags_str[256];
        static const char *rd_kafka_destroy_flags_names[] = {
            "Terminate", "DestroyCalled", "Immediate", "NoConsumerClose", NULL};

        /* Fatal errors and _F_IMMEDIATE also sets .._NO_CONSUMER_CLOSE */
        if (flags & RD_KAFKA_DESTROY_F_IMMEDIATE ||
            rd_kafka_fatal_error_code(rk))
                flags |= RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE;

        rd_flags2str(flags_str, sizeof(flags_str), rd_kafka_destroy_flags_names,
                     flags);
        rd_kafka_dbg(rk, ALL, "DESTROY",
                     "Terminating instance "
                     "(destroy flags %s (0x%x))",
                     flags ? flags_str : "none", flags);

        /* If producer still has messages in queue the application
         * is terminating the producer without first calling flush() or purge()
         * which is a common new user mistake, so hint the user of proper
         * shutdown semantics. */
        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                unsigned int tot_cnt;
                size_t tot_size;

                rd_kafka_curr_msgs_get(rk, &tot_cnt, &tot_size);

                if (tot_cnt > 0)
                        rd_kafka_log(rk, LOG_WARNING, "TERMINATE",
                                     "Producer terminating with %u message%s "
                                     "(%" PRIusz
                                     " byte%s) still in "
                                     "queue or transit: "
                                     "use flush() to wait for "
                                     "outstanding message delivery",
                                     tot_cnt, tot_cnt > 1 ? "s" : "", tot_size,
                                     tot_size > 1 ? "s" : "");
        }

        /* Make sure destroy is not called from a librdkafka thread
         * since this will most likely cause a deadlock.
         * FIXME: include broker threads (for log_cb) */
        if (thrd_is_current(rk->rk_thread) ||
            thrd_is_current(rk->rk_background.thread)) {
                rd_kafka_log(rk, LOG_EMERG, "BGQUEUE",
                             "Application bug: "
                             "rd_kafka_destroy() called from "
                             "librdkafka owned thread");
                rd_kafka_assert(NULL,
                                !*"Application bug: "
                                "calling rd_kafka_destroy() from "
                                "librdkafka owned thread is prohibited");
        }

        /* Before signaling for general termination, set the destroy
         * flags to hint cgrp how to shut down. */
        rd_atomic32_set(&rk->rk_terminate,
                        flags | RD_KAFKA_DESTROY_F_DESTROY_CALLED);

        /* The legacy/simple consumer lacks an API to close down the consumer*/
        if (rk->rk_cgrp) {
                rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                             "Terminating consumer group handler");
                rd_kafka_consumer_close(rk);
        }

        /* Await telemetry termination. This method blocks until the last
         * PushTelemetry request is sent (if possible). */
        if (!(flags & RD_KAFKA_DESTROY_F_IMMEDIATE))
                rd_kafka_telemetry_await_termination(rk);

        /* With the consumer and telemetry closed, terminate the rest of
         * librdkafka. */
        rd_atomic32_set(&rk->rk_terminate,
                        flags | RD_KAFKA_DESTROY_F_TERMINATE);

        rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Interrupting timers");
        rd_kafka_wrlock(rk);
        thrd = rk->rk_thread;
        rd_kafka_timers_interrupt(&rk->rk_timers);
        rd_kafka_wrunlock(rk);

        rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                     "Sending TERMINATE to internal main thread");
        /* Send op to trigger queue/io wake-up.
         * The op itself is (likely) ignored by the receiver. */
        rd_kafka_q_enq(rk->rk_ops, rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

#ifndef _WIN32
        /* Interrupt main kafka thread to speed up termination. */
        if (term_sig) {
                rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                             "Sending thread kill signal %d", term_sig);
                pthread_kill(thrd, term_sig);
        }
#endif

        if (rd_kafka_destroy_flags_check(rk, RD_KAFKA_DESTROY_F_IMMEDIATE))
                return; /* FIXME: thread resource leak */

        rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Joining internal main thread");

        if (thrd_join(thrd, &res) != thrd_success)
                rd_kafka_log(rk, LOG_ERR, "DESTROY",
                             "Failed to join internal main thread: %s "
                             "(was process forked?)",
                             rd_strerror(errno));

        rd_kafka_destroy_final(rk);
}


/* NOTE: Must only be called by application.
 *       librdkafka itself must use rd_kafka_destroy0(). */
void rd_kafka_destroy(rd_kafka_t *rk) {
        rd_kafka_destroy_app(rk, 0);
}

void rd_kafka_destroy_flags(rd_kafka_t *rk, int flags) {
        rd_kafka_destroy_app(rk, flags);
}


/**
 * Main destructor for rd_kafka_t
 *
 * Locality: rdkafka main thread or application thread during rd_kafka_new()
 */
static void rd_kafka_destroy_internal(rd_kafka_t *rk) {
        rd_kafka_topic_t *rkt, *rkt_tmp;
        rd_kafka_broker_t *rkb, *rkb_tmp;
        rd_list_t wait_thrds;
        thrd_t *thrd;
        int i;

        rd_kafka_dbg(rk, ALL, "DESTROY", "Destroy internal");

        /* Trigger any state-change waiters (which should check the
         * terminate flag whenever they wake up). */
        rd_kafka_brokers_broadcast_state_change(rk);

        if (rk->rk_background.thread) {
                int res;
                /* Send op to trigger queue/io wake-up.
                 * The op itself is (likely) ignored by the receiver. */
                rd_kafka_q_enq(rk->rk_background.q,
                               rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

                rd_kafka_dbg(rk, ALL, "DESTROY",
                             "Waiting for background queue thread "
                             "to terminate");
                thrd_join(rk->rk_background.thread, &res);
                rd_kafka_q_destroy_owner(rk->rk_background.q);
        }

        /* Call on_destroy() interceptors */
        rd_kafka_interceptors_on_destroy(rk);

        /* Brokers pick up on rk_terminate automatically. */

        /* List of (broker) threads to join to synchronize termination */
        rd_list_init(&wait_thrds, rd_atomic32_get(&rk->rk_broker_cnt), NULL);

        rd_kafka_wrlock(rk);

        rd_kafka_dbg(rk, ALL, "DESTROY", "Removing all topics");
        /* Decommission all topics */
        TAILQ_FOREACH_SAFE(rkt, &rk->rk_topics, rkt_link, rkt_tmp) {
                rd_kafka_wrunlock(rk);
                rd_kafka_topic_partitions_remove(rkt);
                rd_kafka_wrlock(rk);
        }

        /* Decommission brokers.
         * Broker thread holds a refcount and detects when broker refcounts
         * reaches 1 and then decommissions itself. */
        TAILQ_FOREACH_SAFE(rkb, &rk->rk_brokers, rkb_link, rkb_tmp) {
                /* Add broker's thread to wait_thrds list for later joining */
                thrd  = rd_malloc(sizeof(*thrd));
                *thrd = rkb->rkb_thread;
                rd_list_add(&wait_thrds, thrd);
                rd_kafka_wrunlock(rk);

                rd_kafka_dbg(rk, BROKER, "DESTROY", "Sending TERMINATE to %s",
                             rd_kafka_broker_name(rkb));
                /* Send op to trigger queue/io wake-up.
                 * The op itself is (likely) ignored by the broker thread. */
                rd_kafka_q_enq(rkb->rkb_ops,
                               rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

#ifndef _WIN32
                /* Interrupt IO threads to speed up termination. */
                if (rk->rk_conf.term_sig)
                        pthread_kill(rkb->rkb_thread, rk->rk_conf.term_sig);
#endif

                rd_kafka_broker_destroy(rkb);

                rd_kafka_wrlock(rk);
        }

        if (rk->rk_clusterid) {
                rd_free(rk->rk_clusterid);
                rk->rk_clusterid = NULL;
        }

        /* Destroy coord requests */
        rd_kafka_coord_reqs_term(rk);

        /* Destroy the coordinator cache */
        rd_kafka_coord_cache_destroy(&rk->rk_coord_cache);

        /* Purge metadata cache.
         * #3279:
         * We mustn't call cache_destroy() here since there might be outstanding
         * broker rkos that hold references to the metadata cache lock,
         * and these brokers are destroyed below. So to avoid a circular
         * dependency refcnt deadlock we first purge the cache here
         * and destroy it after the brokers are destroyed. */
        rd_kafka_metadata_cache_purge(rk, rd_true /*observers too*/);

        rd_kafka_wrunlock(rk);

        mtx_lock(&rk->rk_broker_state_change_lock);
        /* Purge broker state change waiters */
        rd_list_destroy(&rk->rk_broker_state_change_waiters);
        mtx_unlock(&rk->rk_broker_state_change_lock);

        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                if (rk->rk_consumer.q)
                        rd_kafka_q_disable(rk->rk_consumer.q);
        }

        rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Purging reply queue");

        /* Purge op-queue */
        rd_kafka_q_disable(rk->rk_rep);
        rd_kafka_q_purge(rk->rk_rep);

        /* Loose our special reference to the internal broker. */
        mtx_lock(&rk->rk_internal_rkb_lock);
        if ((rkb = rk->rk_internal_rkb)) {
                rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                             "Decommissioning internal broker");

                /* Send op to trigger queue wake-up. */
                rd_kafka_q_enq(rkb->rkb_ops,
                               rd_kafka_op_new(RD_KAFKA_OP_TERMINATE));

                rk->rk_internal_rkb = NULL;
                thrd                = rd_malloc(sizeof(*thrd));
                *thrd               = rkb->rkb_thread;
                rd_list_add(&wait_thrds, thrd);
        }
        mtx_unlock(&rk->rk_internal_rkb_lock);
        if (rkb)
                rd_kafka_broker_destroy(rkb);


        rd_kafka_dbg(rk, GENERIC, "TERMINATE", "Join %d broker thread(s)",
                     rd_list_cnt(&wait_thrds));

        /* Join broker threads */
        RD_LIST_FOREACH(thrd, &wait_thrds, i) {
                int res;
                if (thrd_join(*thrd, &res) != thrd_success)
                        ;
                rd_free(thrd);
        }

        rd_list_destroy(&wait_thrds);

        /* Destroy mock cluster */
        if (rk->rk_mock.cluster)
                rd_kafka_mock_cluster_destroy(rk->rk_mock.cluster);

        if (rd_atomic32_get(&rk->rk_mock.cluster_cnt) > 0) {
                rd_kafka_log(rk, LOG_EMERG, "MOCK",
                             "%d mock cluster(s) still active: "
                             "must be explicitly destroyed with "
                             "rd_kafka_mock_cluster_destroy() prior to "
                             "terminating the rd_kafka_t instance",
                             (int)rd_atomic32_get(&rk->rk_mock.cluster_cnt));
                rd_assert(!*"All mock clusters must be destroyed prior to "
                          "rd_kafka_t destroy");
        }

        /* Destroy metadata cache */
        rd_kafka_wrlock(rk);
        rd_kafka_metadata_cache_destroy(rk);
        rd_kafka_wrunlock(rk);
}

/**
 * @brief Buffer state for stats emitter
 */
struct _stats_emit {
        char *buf;   /* Pointer to allocated buffer */
        size_t size; /* Current allocated size of buf */
        size_t of;   /* Current write-offset in buf */
};


/* Stats buffer printf. Requires a (struct _stats_emit *)st variable in the
 * current scope. */
#define _st_printf(...)                                                          \
        do {                                                                     \
                ssize_t _r;                                                      \
                ssize_t _rem = st->size - st->of;                                \
                _r           = rd_snprintf(st->buf + st->of, _rem, __VA_ARGS__); \
                if (_r >= _rem) {                                                \
                        st->size *= 2;                                           \
                        _rem    = st->size - st->of;                             \
                        st->buf = rd_realloc(st->buf, st->size);                 \
                        _r = rd_snprintf(st->buf + st->of, _rem, __VA_ARGS__);   \
                }                                                                \
                st->of += _r;                                                    \
        } while (0)

struct _stats_total {
        int64_t tx;          /**< broker.tx */
        int64_t tx_bytes;    /**< broker.tx_bytes */
        int64_t rx;          /**< broker.rx */
        int64_t rx_bytes;    /**< broker.rx_bytes */
        int64_t txmsgs;      /**< partition.txmsgs */
        int64_t txmsg_bytes; /**< partition.txbytes */
        int64_t rxmsgs;      /**< partition.rxmsgs */
        int64_t rxmsg_bytes; /**< partition.rxbytes */
};



/**
 * @brief Rollover and emit an average window.
 */
static RD_INLINE void rd_kafka_stats_emit_avg(struct _stats_emit *st,
                                              const char *name,
                                              rd_avg_t *src_avg) {
        rd_avg_t avg;

        rd_avg_rollover(&avg, src_avg);
        _st_printf(
            "\"%s\": {"
            " \"min\":%" PRId64
            ","
            " \"max\":%" PRId64
            ","
            " \"avg\":%" PRId64
            ","
            " \"sum\":%" PRId64
            ","
            " \"stddev\": %" PRId64
            ","
            " \"p50\": %" PRId64
            ","
            " \"p75\": %" PRId64
            ","
            " \"p90\": %" PRId64
            ","
            " \"p95\": %" PRId64
            ","
            " \"p99\": %" PRId64
            ","
            " \"p99_99\": %" PRId64
            ","
            " \"outofrange\": %" PRId64
            ","
            " \"hdrsize\": %" PRId32
            ","
            " \"cnt\":%i "
            "}, ",
            name, avg.ra_v.minv, avg.ra_v.maxv, avg.ra_v.avg, avg.ra_v.sum,
            (int64_t)avg.ra_hist.stddev, avg.ra_hist.p50, avg.ra_hist.p75,
            avg.ra_hist.p90, avg.ra_hist.p95, avg.ra_hist.p99,
            avg.ra_hist.p99_99, avg.ra_hist.oor, avg.ra_hist.hdrsize,
            avg.ra_v.cnt);
        rd_avg_destroy(&avg);
}

/**
 * Emit stats for toppar
 */
static RD_INLINE void rd_kafka_stats_emit_toppar(struct _stats_emit *st,
                                                 struct _stats_total *total,
                                                 rd_kafka_toppar_t *rktp,
                                                 int first) {
        rd_kafka_t *rk = rktp->rktp_rkt->rkt_rk;
        int64_t end_offset;
        int64_t consumer_lag        = -1;
        int64_t consumer_lag_stored = -1;
        struct offset_stats offs;
        int32_t broker_id = -1;

        rd_kafka_toppar_lock(rktp);

        if (rktp->rktp_broker) {
                rd_kafka_broker_lock(rktp->rktp_broker);
                broker_id = rktp->rktp_broker->rkb_nodeid;
                rd_kafka_broker_unlock(rktp->rktp_broker);
        }

        /* Grab a copy of the latest finalized offset stats */
        offs = rktp->rktp_offsets_fin;

        end_offset = (rk->rk_conf.isolation_level == RD_KAFKA_READ_COMMITTED)
                         ? rktp->rktp_ls_offset
                         : rktp->rktp_hi_offset;

        /* Calculate consumer_lag by using the highest offset
         * of stored_offset (the last message passed to application + 1, or
         * if enable.auto.offset.store=false the last message manually stored),
         * or the committed_offset (the last message committed by this or
         * another consumer).
         * Using stored_offset allows consumer_lag to be up to date even if
         * offsets are not (yet) committed.
         */
        if (end_offset != RD_KAFKA_OFFSET_INVALID) {
                if (rktp->rktp_stored_pos.offset >= 0 &&
                    rktp->rktp_stored_pos.offset <= end_offset)
                        consumer_lag_stored =
                            end_offset - rktp->rktp_stored_pos.offset;
                if (rktp->rktp_committed_pos.offset >= 0 &&
                    rktp->rktp_committed_pos.offset <= end_offset)
                        consumer_lag =
                            end_offset - rktp->rktp_committed_pos.offset;
        }

        _st_printf(
            "%s\"%" PRId32
            "\": { "
            "\"partition\":%" PRId32
            ", "
            "\"broker\":%" PRId32
            ", "
            "\"leader\":%" PRId32
            ", "
            "\"desired\":%s, "
            "\"unknown\":%s, "
            "\"msgq_cnt\":%i, "
            "\"msgq_bytes\":%" PRIusz
            ", "
            "\"xmit_msgq_cnt\":%i, "
            "\"xmit_msgq_bytes\":%" PRIusz
            ", "
            "\"fetchq_cnt\":%i, "
            "\"fetchq_size\":%" PRIu64
            ", "
            "\"fetch_state\":\"%s\", "
            "\"query_offset\":%" PRId64
            ", "
            "\"next_offset\":%" PRId64
            ", "
            "\"app_offset\":%" PRId64
            ", "
            "\"stored_offset\":%" PRId64
            ", "
            "\"stored_leader_epoch\":%" PRId32
            ", "
            "\"commited_offset\":%" PRId64
            ", " /*FIXME: issue #80 */
            "\"committed_offset\":%" PRId64
            ", "
            "\"committed_leader_epoch\":%" PRId32
            ", "
            "\"eof_offset\":%" PRId64
            ", "
            "\"lo_offset\":%" PRId64
            ", "
            "\"hi_offset\":%" PRId64
            ", "
            "\"ls_offset\":%" PRId64
            ", "
            "\"consumer_lag\":%" PRId64
            ", "
            "\"consumer_lag_stored\":%" PRId64
            ", "
            "\"leader_epoch\":%" PRId32
            ", "
            "\"txmsgs\":%" PRIu64
            ", "
            "\"txbytes\":%" PRIu64
            ", "
            "\"rxmsgs\":%" PRIu64
            ", "
            "\"rxbytes\":%" PRIu64
            ", "
            "\"msgs\": %" PRIu64
            ", "
            "\"rx_ver_drops\": %" PRIu64
            ", "
            "\"msgs_inflight\": %" PRId32
            ", "
            "\"next_ack_seq\": %" PRId32
            ", "
            "\"next_err_seq\": %" PRId32
            ", "
            "\"acked_msgid\": %" PRIu64 "} ",
            first ? "" : ", ", rktp->rktp_partition, rktp->rktp_partition,
            broker_id, rktp->rktp_leader_id,
            (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_DESIRED) ? "true" : "false",
            (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_UNKNOWN) ? "true" : "false",
            rd_kafka_msgq_len(&rktp->rktp_msgq),
            rd_kafka_msgq_size(&rktp->rktp_msgq),
            /* FIXME: xmit_msgq is local to the broker thread. */
            0, (size_t)0, rd_kafka_q_len(rktp->rktp_fetchq),
            rd_kafka_q_size(rktp->rktp_fetchq),
            rd_kafka_fetch_states[rktp->rktp_fetch_state],
            rktp->rktp_query_pos.offset, offs.fetch_pos.offset,
            rktp->rktp_app_pos.offset, rktp->rktp_stored_pos.offset,
            rktp->rktp_stored_pos.leader_epoch,
            rktp->rktp_committed_pos.offset, /* FIXME: issue #80 */
            rktp->rktp_committed_pos.offset,
            rktp->rktp_committed_pos.leader_epoch, offs.eof_offset,
            rktp->rktp_lo_offset, rktp->rktp_hi_offset, rktp->rktp_ls_offset,
            consumer_lag, consumer_lag_stored, rktp->rktp_leader_epoch,
            rd_atomic64_get(&rktp->rktp_c.tx_msgs),
            rd_atomic64_get(&rktp->rktp_c.tx_msg_bytes),
            rd_atomic64_get(&rktp->rktp_c.rx_msgs),
            rd_atomic64_get(&rktp->rktp_c.rx_msg_bytes),
            rk->rk_type == RD_KAFKA_PRODUCER
                ? rd_atomic64_get(&rktp->rktp_c.producer_enq_msgs)
                : rd_atomic64_get(
                      &rktp->rktp_c.rx_msgs), /* legacy, same as rx_msgs */
            rd_atomic64_get(&rktp->rktp_c.rx_ver_drops),
            rd_atomic32_get(&rktp->rktp_msgs_inflight),
            rktp->rktp_eos.next_ack_seq, rktp->rktp_eos.next_err_seq,
            rktp->rktp_eos.acked_msgid);

        if (total) {
                total->txmsgs += rd_atomic64_get(&rktp->rktp_c.tx_msgs);
                total->txmsg_bytes +=
                    rd_atomic64_get(&rktp->rktp_c.tx_msg_bytes);
                total->rxmsgs += rd_atomic64_get(&rktp->rktp_c.rx_msgs);
                total->rxmsg_bytes +=
                    rd_atomic64_get(&rktp->rktp_c.rx_msg_bytes);
        }

        rd_kafka_toppar_unlock(rktp);
}

/**
 * @brief Emit broker request type stats
 */
static void rd_kafka_stats_emit_broker_reqs(struct _stats_emit *st,
                                            rd_kafka_broker_t *rkb) {
        /* Filter out request types that will never be sent by the client. */
        static const rd_bool_t filter[4][RD_KAFKAP__NUM] = {
            [RD_KAFKA_PRODUCER] = {[RD_KAFKAP_Fetch]        = rd_true,
                                   [RD_KAFKAP_OffsetCommit] = rd_true,
                                   [RD_KAFKAP_OffsetFetch]  = rd_true,
                                   [RD_KAFKAP_JoinGroup]    = rd_true,
                                   [RD_KAFKAP_Heartbeat]    = rd_true,
                                   [RD_KAFKAP_LeaveGroup]   = rd_true,
                                   [RD_KAFKAP_SyncGroup]    = rd_true},
            [RD_KAFKA_CONSUMER] =
                {
                    [RD_KAFKAP_Produce]        = rd_true,
                    [RD_KAFKAP_InitProducerId] = rd_true,
                    /* Transactional producer */
                    [RD_KAFKAP_AddPartitionsToTxn] = rd_true,
                    [RD_KAFKAP_AddOffsetsToTxn]    = rd_true,
                    [RD_KAFKAP_EndTxn]             = rd_true,
                    [RD_KAFKAP_TxnOffsetCommit]    = rd_true,
                },
            [2 /*any client type*/] =
                {
                    [RD_KAFKAP_UpdateMetadata]       = rd_true,
                    [RD_KAFKAP_ControlledShutdown]   = rd_true,
                    [RD_KAFKAP_LeaderAndIsr]         = rd_true,
                    [RD_KAFKAP_StopReplica]          = rd_true,
                    [RD_KAFKAP_OffsetForLeaderEpoch] = rd_true,

                    [RD_KAFKAP_WriteTxnMarkers] = rd_true,

                    [RD_KAFKAP_AlterReplicaLogDirs] = rd_true,
                    [RD_KAFKAP_DescribeLogDirs]     = rd_true,

                    [RD_KAFKAP_CreateDelegationToken]       = rd_true,
                    [RD_KAFKAP_RenewDelegationToken]        = rd_true,
                    [RD_KAFKAP_ExpireDelegationToken]       = rd_true,
                    [RD_KAFKAP_DescribeDelegationToken]     = rd_true,
                    [RD_KAFKAP_IncrementalAlterConfigs]     = rd_true,
                    [RD_KAFKAP_ElectLeaders]                = rd_true,
                    [RD_KAFKAP_AlterPartitionReassignments] = rd_true,
                    [RD_KAFKAP_ListPartitionReassignments]  = rd_true,
                    [RD_KAFKAP_AlterUserScramCredentials]   = rd_true,
                    [RD_KAFKAP_Vote]                        = rd_true,
                    [RD_KAFKAP_BeginQuorumEpoch]            = rd_true,
                    [RD_KAFKAP_EndQuorumEpoch]              = rd_true,
                    [RD_KAFKAP_DescribeQuorum]              = rd_true,
                    [RD_KAFKAP_AlterIsr]                    = rd_true,
                    [RD_KAFKAP_UpdateFeatures]              = rd_true,
                    [RD_KAFKAP_Envelope]                    = rd_true,
                    [RD_KAFKAP_FetchSnapshot]               = rd_true,
                    [RD_KAFKAP_BrokerHeartbeat]             = rd_true,
                    [RD_KAFKAP_UnregisterBroker]            = rd_true,
                    [RD_KAFKAP_AllocateProducerIds]         = rd_true,
                    [RD_KAFKAP_ConsumerGroupHeartbeat]      = rd_true,
                },
            [3 /*hide-unless-non-zero*/] = {
                /* Hide Admin requests unless they've been used */
                [RD_KAFKAP_CreateTopics]                 = rd_true,
                [RD_KAFKAP_DeleteTopics]                 = rd_true,
                [RD_KAFKAP_DeleteRecords]                = rd_true,
                [RD_KAFKAP_CreatePartitions]             = rd_true,
                [RD_KAFKAP_DescribeAcls]                 = rd_true,
                [RD_KAFKAP_CreateAcls]                   = rd_true,
                [RD_KAFKAP_DeleteAcls]                   = rd_true,
                [RD_KAFKAP_DescribeConfigs]              = rd_true,
                [RD_KAFKAP_AlterConfigs]                 = rd_true,
                [RD_KAFKAP_DeleteGroups]                 = rd_true,
                [RD_KAFKAP_ListGroups]                   = rd_true,
                [RD_KAFKAP_DescribeGroups]               = rd_true,
                [RD_KAFKAP_DescribeLogDirs]              = rd_true,
                [RD_KAFKAP_IncrementalAlterConfigs]      = rd_true,
                [RD_KAFKAP_AlterPartitionReassignments]  = rd_true,
                [RD_KAFKAP_ListPartitionReassignments]   = rd_true,
                [RD_KAFKAP_OffsetDelete]                 = rd_true,
                [RD_KAFKAP_DescribeClientQuotas]         = rd_true,
                [RD_KAFKAP_AlterClientQuotas]            = rd_true,
                [RD_KAFKAP_DescribeUserScramCredentials] = rd_true,
                [RD_KAFKAP_AlterUserScramCredentials]    = rd_true,
            }};
        int i;
        int cnt = 0;

        _st_printf("\"req\": { ");
        for (i = 0; i < RD_KAFKAP__NUM; i++) {
                int64_t v;

                if (filter[rkb->rkb_rk->rk_type][i] || filter[2][i])
                        continue;

                v = rd_atomic64_get(&rkb->rkb_c.reqtype[i]);
                if (!v && filter[3][i])
                        continue; /* Filter out zero values */

                _st_printf("%s\"%s\": %" PRId64, cnt > 0 ? ", " : "",
                           rd_kafka_ApiKey2str(i), v);

                cnt++;
        }
        _st_printf(" }, ");
}


/**
 * Emit all statistics
 */
static void rd_kafka_stats_emit_all(rd_kafka_t *rk) {
        rd_kafka_broker_t *rkb;
        rd_kafka_topic_t *rkt;
        rd_ts_t now;
        rd_kafka_op_t *rko;
        unsigned int tot_cnt;
        size_t tot_size;
        rd_kafka_resp_err_t err;
        struct _stats_emit stx    = {.size = 1024 * 10};
        struct _stats_emit *st    = &stx;
        struct _stats_total total = {0};

        st->buf = rd_malloc(st->size);


        rd_kafka_curr_msgs_get(rk, &tot_cnt, &tot_size);
        rd_kafka_rdlock(rk);

        now = rd_clock();
        _st_printf(
            "{ "
            "\"name\": \"%s\", "
            "\"client_id\": \"%s\", "
            "\"type\": \"%s\", "
            "\"ts\":%" PRId64
            ", "
            "\"time\":%lli, "
            "\"age\":%" PRId64
            ", "
            "\"replyq\":%i, "
            "\"msg_cnt\":%u, "
            "\"msg_size\":%" PRIusz
            ", "
            "\"msg_max\":%u, "
            "\"msg_size_max\":%" PRIusz
            ", "
            "\"simple_cnt\":%i, "
            "\"metadata_cache_cnt\":%i, "
            "\"brokers\":{ " /*open brokers*/,
            rk->rk_name, rk->rk_conf.client_id_str,
            rd_kafka_type2str(rk->rk_type), now, (signed long long)time(NULL),
            now - rk->rk_ts_created, rd_kafka_q_len(rk->rk_rep), tot_cnt,
            tot_size, rk->rk_curr_msgs.max_cnt, rk->rk_curr_msgs.max_size,
            rd_atomic32_get(&rk->rk_simple_cnt),
            rk->rk_metadata_cache.rkmc_cnt);


        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_kafka_toppar_t *rktp;
                rd_ts_t txidle = -1, rxidle = -1;

                rd_kafka_broker_lock(rkb);

                if (rkb->rkb_state >= RD_KAFKA_BROKER_STATE_UP) {
                        /* Calculate tx and rx idle time in usecs */
                        txidle = rd_atomic64_get(&rkb->rkb_c.ts_send);
                        rxidle = rd_atomic64_get(&rkb->rkb_c.ts_recv);

                        if (txidle)
                                txidle = RD_MAX(now - txidle, 0);
                        else
                                txidle = -1;

                        if (rxidle)
                                rxidle = RD_MAX(now - rxidle, 0);
                        else
                                rxidle = -1;
                }

                _st_printf(
                    "%s\"%s\": { " /*open broker*/
                    "\"name\":\"%s\", "
                    "\"nodeid\":%" PRId32
                    ", "
                    "\"nodename\":\"%s\", "
                    "\"source\":\"%s\", "
                    "\"state\":\"%s\", "
                    "\"stateage\":%" PRId64
                    ", "
                    "\"outbuf_cnt\":%i, "
                    "\"outbuf_msg_cnt\":%i, "
                    "\"waitresp_cnt\":%i, "
                    "\"waitresp_msg_cnt\":%i, "
                    "\"tx\":%" PRIu64
                    ", "
                    "\"txbytes\":%" PRIu64
                    ", "
                    "\"txerrs\":%" PRIu64
                    ", "
                    "\"txretries\":%" PRIu64
                    ", "
                    "\"txidle\":%" PRId64
                    ", "
                    "\"req_timeouts\":%" PRIu64
                    ", "
                    "\"rx\":%" PRIu64
                    ", "
                    "\"rxbytes\":%" PRIu64
                    ", "
                    "\"rxerrs\":%" PRIu64
                    ", "
                    "\"rxcorriderrs\":%" PRIu64
                    ", "
                    "\"rxpartial\":%" PRIu64
                    ", "
                    "\"rxidle\":%" PRId64
                    ", "
                    "\"zbuf_grow\":%" PRIu64
                    ", "
                    "\"buf_grow\":%" PRIu64
                    ", "
                    "\"wakeups\":%" PRIu64
                    ", "
                    "\"connects\":%" PRId32
                    ", "
                    "\"disconnects\":%" PRId32 ", ",
                    rkb == TAILQ_FIRST(&rk->rk_brokers) ? "" : ", ",
                    rkb->rkb_name, rkb->rkb_name, rkb->rkb_nodeid,
                    rkb->rkb_nodename, rd_kafka_confsource2str(rkb->rkb_source),
                    rd_kafka_broker_state_names[rkb->rkb_state],
                    rkb->rkb_ts_state ? now - rkb->rkb_ts_state : 0,
                    rd_atomic32_get(&rkb->rkb_outbufs.rkbq_cnt),
                    rd_atomic32_get(&rkb->rkb_outbufs.rkbq_msg_cnt),
                    rd_atomic32_get(&rkb->rkb_waitresps.rkbq_cnt),
                    rd_atomic32_get(&rkb->rkb_waitresps.rkbq_msg_cnt),
                    rd_atomic64_get(&rkb->rkb_c.tx),
                    rd_atomic64_get(&rkb->rkb_c.tx_bytes),
                    rd_atomic64_get(&rkb->rkb_c.tx_err),
                    rd_atomic64_get(&rkb->rkb_c.tx_retries), txidle,
                    rd_atomic64_get(&rkb->rkb_c.req_timeouts),
                    rd_atomic64_get(&rkb->rkb_c.rx),
                    rd_atomic64_get(&rkb->rkb_c.rx_bytes),
                    rd_atomic64_get(&rkb->rkb_c.rx_err),
                    rd_atomic64_get(&rkb->rkb_c.rx_corrid_err),
                    rd_atomic64_get(&rkb->rkb_c.rx_partial), rxidle,
                    rd_atomic64_get(&rkb->rkb_c.zbuf_grow),
                    rd_atomic64_get(&rkb->rkb_c.buf_grow),
                    rd_atomic64_get(&rkb->rkb_c.wakeups),
                    rd_atomic32_get(&rkb->rkb_c.connects),
                    rd_atomic32_get(&rkb->rkb_c.disconnects));

                total.tx += rd_atomic64_get(&rkb->rkb_c.tx);
                total.tx_bytes += rd_atomic64_get(&rkb->rkb_c.tx_bytes);
                total.rx += rd_atomic64_get(&rkb->rkb_c.rx);
                total.rx_bytes += rd_atomic64_get(&rkb->rkb_c.rx_bytes);

                rd_kafka_stats_emit_avg(st, "int_latency",
                                        &rkb->rkb_avg_int_latency);
                rd_kafka_stats_emit_avg(st, "outbuf_latency",
                                        &rkb->rkb_avg_outbuf_latency);
                rd_kafka_stats_emit_avg(st, "rtt", &rkb->rkb_avg_rtt);
                rd_kafka_stats_emit_avg(st, "throttle", &rkb->rkb_avg_throttle);

                rd_kafka_stats_emit_broker_reqs(st, rkb);

                _st_printf("\"toppars\":{ " /*open toppars*/);

                TAILQ_FOREACH(rktp, &rkb->rkb_toppars, rktp_rkblink) {
                        _st_printf(
                            "%s\"%.*s-%" PRId32
                            "\": { "
                            "\"topic\":\"%.*s\", "
                            "\"partition\":%" PRId32 "} ",
                            rktp == TAILQ_FIRST(&rkb->rkb_toppars) ? "" : ", ",
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition,
                            RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                            rktp->rktp_partition);
                }

                rd_kafka_broker_unlock(rkb);

                _st_printf(
                    "} " /*close toppars*/
                    "} " /*close broker*/);
        }


        _st_printf(
            "}, " /* close "brokers" array */
            "\"topics\":{ ");

        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                rd_kafka_toppar_t *rktp;
                int i, j;

                rd_kafka_topic_rdlock(rkt);
                _st_printf(
                    "%s\"%.*s\": { "
                    "\"topic\":\"%.*s\", "
                    "\"age\":%" PRId64
                    ", "
                    "\"metadata_age\":%" PRId64 ", ",
                    rkt == TAILQ_FIRST(&rk->rk_topics) ? "" : ", ",
                    RD_KAFKAP_STR_PR(rkt->rkt_topic),
                    RD_KAFKAP_STR_PR(rkt->rkt_topic),
                    (now - rkt->rkt_ts_create) / 1000,
                    rkt->rkt_ts_metadata ? (now - rkt->rkt_ts_metadata) / 1000
                                         : 0);

                rd_kafka_stats_emit_avg(st, "batchsize",
                                        &rkt->rkt_avg_batchsize);
                rd_kafka_stats_emit_avg(st, "batchcnt", &rkt->rkt_avg_batchcnt);

                _st_printf("\"partitions\":{ " /*open partitions*/);

                for (i = 0; i < rkt->rkt_partition_cnt; i++)
                        rd_kafka_stats_emit_toppar(st, &total, rkt->rkt_p[i],
                                                   i == 0);

                RD_LIST_FOREACH(rktp, &rkt->rkt_desp, j)
                rd_kafka_stats_emit_toppar(st, &total, rktp, i + j == 0);

                i += j;

                if (rkt->rkt_ua)
                        rd_kafka_stats_emit_toppar(st, NULL, rkt->rkt_ua,
                                                   i++ == 0);

                rd_kafka_topic_rdunlock(rkt);

                _st_printf(
                    "} " /*close partitions*/
                    "} " /*close topic*/);
        }
        _st_printf("} " /*close topics*/);

        if (rk->rk_cgrp) {
                rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;
                _st_printf(
                    ", \"cgrp\": { "
                    "\"state\": \"%s\", "
                    "\"stateage\": %" PRId64
                    ", "
                    "\"join_state\": \"%s\", "
                    "\"rebalance_age\": %" PRId64
                    ", "
                    "\"rebalance_cnt\": %d, "
                    "\"rebalance_reason\": \"%s\", "
                    "\"assignment_size\": %d }",
                    rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                    rkcg->rkcg_ts_statechange
                        ? (now - rkcg->rkcg_ts_statechange) / 1000
                        : 0,
                    rd_kafka_cgrp_join_state_names[rkcg->rkcg_join_state],
                    rkcg->rkcg_c.ts_rebalance
                        ? (now - rkcg->rkcg_c.ts_rebalance) / 1000
                        : 0,
                    rkcg->rkcg_c.rebalance_cnt, rkcg->rkcg_c.rebalance_reason,
                    rkcg->rkcg_c.assignment_size);
        }

        if (rd_kafka_is_idempotent(rk)) {
                _st_printf(
                    ", \"eos\": { "
                    "\"idemp_state\": \"%s\", "
                    "\"idemp_stateage\": %" PRId64
                    ", "
                    "\"txn_state\": \"%s\", "
                    "\"txn_stateage\": %" PRId64
                    ", "
                    "\"txn_may_enq\": %s, "
                    "\"producer_id\": %" PRId64
                    ", "
                    "\"producer_epoch\": %hd, "
                    "\"epoch_cnt\": %d "
                    "}",
                    rd_kafka_idemp_state2str(rk->rk_eos.idemp_state),
                    (now - rk->rk_eos.ts_idemp_state) / 1000,
                    rd_kafka_txn_state2str(rk->rk_eos.txn_state),
                    (now - rk->rk_eos.ts_txn_state) / 1000,
                    rd_atomic32_get(&rk->rk_eos.txn_may_enq) ? "true" : "false",
                    rk->rk_eos.pid.id, rk->rk_eos.pid.epoch,
                    rk->rk_eos.epoch_cnt);
        }

        if ((err = rd_atomic32_get(&rk->rk_fatal.err)))
                _st_printf(
                    ", \"fatal\": { "
                    "\"error\": \"%s\", "
                    "\"reason\": \"%s\", "
                    "\"cnt\": %d "
                    "}",
                    rd_kafka_err2str(err), rk->rk_fatal.errstr,
                    rk->rk_fatal.cnt);

        rd_kafka_rdunlock(rk);

        /* Total counters */
        _st_printf(
            ", "
            "\"tx\":%" PRId64
            ", "
            "\"tx_bytes\":%" PRId64
            ", "
            "\"rx\":%" PRId64
            ", "
            "\"rx_bytes\":%" PRId64
            ", "
            "\"txmsgs\":%" PRId64
            ", "
            "\"txmsg_bytes\":%" PRId64
            ", "
            "\"rxmsgs\":%" PRId64
            ", "
            "\"rxmsg_bytes\":%" PRId64,
            total.tx, total.tx_bytes, total.rx, total.rx_bytes, total.txmsgs,
            total.txmsg_bytes, total.rxmsgs, total.rxmsg_bytes);

        _st_printf("}" /*close object*/);


        /* Enqueue op for application */
        rko = rd_kafka_op_new(RD_KAFKA_OP_STATS);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_HIGH);
        rko->rko_u.stats.json     = st->buf;
        rko->rko_u.stats.json_len = st->of;
        rd_kafka_q_enq(rk->rk_rep, rko);
}


/**
 * @brief 1 second generic timer.
 *
 * @locality rdkafka main thread
 * @locks none
 */
static void rd_kafka_1s_tmr_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_t *rk = rkts->rkts_rk;

        /* Scan topic state, message timeouts, etc. */
        rd_kafka_topic_scan_all(rk, rd_clock());

        /* Sparse connections:
         * try to maintain at least one connection to the cluster. */
        if (rk->rk_conf.sparse_connections &&
            rd_atomic32_get(&rk->rk_broker_up_cnt) == 0)
                rd_kafka_connect_any(rk, "no cluster connection");

        rd_kafka_coord_cache_expire(&rk->rk_coord_cache);
}

static void rd_kafka_stats_emit_tmr_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_t *rk = rkts->rkts_rk;
        rd_kafka_stats_emit_all(rk);
}


/**
 * @brief Periodic metadata refresh callback
 *
 * @locality rdkafka main thread
 */
static void rd_kafka_metadata_refresh_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_t *rk = rkts->rkts_rk;
        rd_kafka_resp_err_t err;

        /* High-level consumer:
         * We need to query both locally known topics and subscribed topics
         * so that we can detect locally known topics changing partition
         * count or disappearing, as well as detect previously non-existent
         * subscribed topics now being available in the cluster. */
        if (rk->rk_type == RD_KAFKA_CONSUMER && rk->rk_cgrp)
                err = rd_kafka_metadata_refresh_consumer_topics(
                    rk, NULL, "periodic topic and broker list refresh");
        else
                err = rd_kafka_metadata_refresh_known_topics(
                    rk, NULL, rd_true /*force*/,
                    "periodic topic and broker list refresh");


        if (err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC &&
            rd_interval(&rk->rk_suppress.broker_metadata_refresh,
                        10 * 1000 * 1000 /*10s*/, 0) > 0) {
                /* If there are no (locally referenced) topics
                 * to query, refresh the broker list.
                 * This avoids getting idle-disconnected for clients
                 * that have not yet referenced a topic and makes
                 * sure such a client has an up to date broker list. */
                rd_kafka_metadata_refresh_brokers(
                    rk, NULL, "periodic broker list refresh");
        }
}



/**
 * @brief Wait for background threads to initialize.
 *
 * @returns the number of background threads still not initialized.
 *
 * @locality app thread calling rd_kafka_new()
 * @locks none
 */
static int rd_kafka_init_wait(rd_kafka_t *rk, int timeout_ms) {
        struct timespec tspec;
        int ret;

        rd_timeout_init_timespec(&tspec, timeout_ms);

        mtx_lock(&rk->rk_init_lock);
        while (rk->rk_init_wait_cnt > 0 &&
               cnd_timedwait_abs(&rk->rk_init_cnd, &rk->rk_init_lock, &tspec) ==
                   thrd_success)
                ;
        ret = rk->rk_init_wait_cnt;
        mtx_unlock(&rk->rk_init_lock);

        return ret;
}


/**
 * Main loop for Kafka handler thread.
 */
static int rd_kafka_thread_main(void *arg) {
        rd_kafka_t *rk                        = arg;
        rd_kafka_timer_t tmr_1s               = RD_ZERO_INIT;
        rd_kafka_timer_t tmr_stats_emit       = RD_ZERO_INIT;
        rd_kafka_timer_t tmr_metadata_refresh = RD_ZERO_INIT;

        rd_kafka_set_thread_name("main");
        rd_kafka_set_thread_sysname("rdk:main");

        rd_kafka_interceptors_on_thread_start(rk, RD_KAFKA_THREAD_MAIN);

        (void)rd_atomic32_add(&rd_kafka_thread_cnt_curr, 1);

        /* Acquire lock (which was held by thread creator during creation)
         * to synchronise state. */
        rd_kafka_wrlock(rk);
        rd_kafka_wrunlock(rk);

        /* 1 second timer for topic scan and connection checking. */
        rd_kafka_timer_start(&rk->rk_timers, &tmr_1s, 1000000,
                             rd_kafka_1s_tmr_cb, NULL);
        if (rk->rk_conf.stats_interval_ms)
                rd_kafka_timer_start(&rk->rk_timers, &tmr_stats_emit,
                                     rk->rk_conf.stats_interval_ms * 1000ll,
                                     rd_kafka_stats_emit_tmr_cb, NULL);
        if (rk->rk_conf.metadata_refresh_interval_ms > 0)
                rd_kafka_timer_start(&rk->rk_timers, &tmr_metadata_refresh,
                                     rk->rk_conf.metadata_refresh_interval_ms *
                                         1000ll,
                                     rd_kafka_metadata_refresh_cb, NULL);

        if (rk->rk_cgrp)
                rd_kafka_q_fwd_set(rk->rk_cgrp->rkcg_ops, rk->rk_ops);

        if (rd_kafka_is_idempotent(rk))
                rd_kafka_idemp_init(rk);

        mtx_lock(&rk->rk_init_lock);
        rk->rk_init_wait_cnt--;
        cnd_broadcast(&rk->rk_init_cnd);
        mtx_unlock(&rk->rk_init_lock);

        while (likely(!rd_kafka_terminating(rk) || rd_kafka_q_len(rk->rk_ops) ||
                      (rk->rk_cgrp && (rk->rk_cgrp->rkcg_state !=
                                       RD_KAFKA_CGRP_STATE_TERM)))) {
                rd_ts_t sleeptime = rd_kafka_timers_next(
                    &rk->rk_timers, 1000 * 1000 /*1s*/, 1 /*lock*/);
                /* Use ceiling division to avoid calling serve with a 0 ms
                 * timeout in a tight loop until 1 ms has passed. */
                int timeout_ms = (sleeptime + 999) / 1000;
                rd_kafka_q_serve(rk->rk_ops, timeout_ms, 0,
                                 RD_KAFKA_Q_CB_CALLBACK, NULL, NULL);
                if (rk->rk_cgrp) /* FIXME: move to timer-triggered */
                        rd_kafka_cgrp_serve(rk->rk_cgrp);
                rd_kafka_timers_run(&rk->rk_timers, RD_POLL_NOWAIT);
        }

        rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                     "Internal main thread terminating");

        if (rd_kafka_is_idempotent(rk))
                rd_kafka_idemp_term(rk);

        rd_kafka_q_disable(rk->rk_ops);
        rd_kafka_q_purge(rk->rk_ops);

        rd_kafka_timer_stop(&rk->rk_timers, &tmr_1s, 1);
        if (rk->rk_conf.stats_interval_ms)
                rd_kafka_timer_stop(&rk->rk_timers, &tmr_stats_emit, 1);
        rd_kafka_timer_stop(&rk->rk_timers, &tmr_metadata_refresh, 1);

        /* Synchronise state */
        rd_kafka_wrlock(rk);
        rd_kafka_wrunlock(rk);

        rd_kafka_interceptors_on_thread_exit(rk, RD_KAFKA_THREAD_MAIN);

        rd_kafka_destroy_internal(rk);

        rd_kafka_dbg(rk, GENERIC, "TERMINATE",
                     "Internal main thread termination done");

        rd_atomic32_sub(&rd_kafka_thread_cnt_curr, 1);

        return 0;
}


void rd_kafka_term_sig_handler(int sig) {
        /* nop */
}


rd_kafka_t *rd_kafka_new(rd_kafka_type_t type,
                         rd_kafka_conf_t *app_conf,
                         char *errstr,
                         size_t errstr_size) {
        rd_kafka_t *rk;
        static rd_atomic32_t rkid;
        rd_kafka_conf_t *conf;
        rd_kafka_resp_err_t ret_err = RD_KAFKA_RESP_ERR_NO_ERROR;
        int ret_errno               = 0;
        const char *conf_err;
        char *group_remote_assignor_override = NULL;
#ifndef _WIN32
        sigset_t newset, oldset;
#endif
        char builtin_features[128];
        size_t bflen;

        rd_kafka_global_init();

        /* rd_kafka_new() takes ownership of the provided \p app_conf
         * object if rd_kafka_new() succeeds.
         * Since \p app_conf is optional we allocate a default configuration
         * object here if \p app_conf is NULL.
         * The configuration object itself is struct-copied later
         * leaving the default *conf pointer to be ready for freeing.
         * In case new() fails and app_conf was specified we will clear out
         * rk_conf to avoid double-freeing from destroy_internal() and the
         * user's eventual call to rd_kafka_conf_destroy().
         * This is all a bit tricky but that's the nature of
         * legacy interfaces. */
        if (!app_conf)
                conf = rd_kafka_conf_new();
        else
                conf = app_conf;

        /* Verify and finalize configuration */
        if ((conf_err = rd_kafka_conf_finalize(type, conf))) {
                /* Incompatible configuration settings */
                rd_snprintf(errstr, errstr_size, "%s", conf_err);
                if (!app_conf)
                        rd_kafka_conf_destroy(conf);
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return NULL;
        }


        rd_kafka_global_cnt_incr();

        /*
         * Set up the handle.
         */
        rk = rd_calloc(1, sizeof(*rk));

        rk->rk_type       = type;
        rk->rk_ts_created = rd_clock();

        /* Struct-copy the config object. */
        rk->rk_conf = *conf;
        if (!app_conf)
                rd_free(conf); /* Free the base config struct only,
                                * not its fields since they were copied to
                                * rk_conf just above. Those fields are
                                * freed from rd_kafka_destroy_internal()
                                * as the rk itself is destroyed. */

        /* Seed PRNG, don't bother about HAVE_RAND_R, since it is pretty cheap.
         */
        if (rk->rk_conf.enable_random_seed)
                call_once(&rd_kafka_global_srand_once, rd_kafka_global_srand);

        /* Call on_new() interceptors */
        rd_kafka_interceptors_on_new(rk, &rk->rk_conf);

        rwlock_init(&rk->rk_lock);
        mtx_init(&rk->rk_conf.sasl.lock, mtx_plain);
        mtx_init(&rk->rk_internal_rkb_lock, mtx_plain);

        cnd_init(&rk->rk_broker_state_change_cnd);
        mtx_init(&rk->rk_broker_state_change_lock, mtx_plain);
        rd_list_init(&rk->rk_broker_state_change_waiters, 8,
                     rd_kafka_enq_once_trigger_destroy);

        cnd_init(&rk->rk_init_cnd);
        mtx_init(&rk->rk_init_lock, mtx_plain);

        rd_interval_init(&rk->rk_suppress.no_idemp_brokers);
        rd_interval_init(&rk->rk_suppress.broker_metadata_refresh);
        rd_interval_init(&rk->rk_suppress.sparse_connect_random);
        mtx_init(&rk->rk_suppress.sparse_connect_lock, mtx_plain);

        mtx_init(&rk->rk_telemetry.lock, mtx_plain);
        cnd_init(&rk->rk_telemetry.termination_cnd);

        rd_atomic64_init(&rk->rk_ts_last_poll, rk->rk_ts_created);
        rd_atomic32_init(&rk->rk_flushing, 0);

        rk->rk_rep             = rd_kafka_q_new(rk);
        rk->rk_ops             = rd_kafka_q_new(rk);
        rk->rk_ops->rkq_serve  = rd_kafka_poll_cb;
        rk->rk_ops->rkq_opaque = rk;

        if (rk->rk_conf.log_queue) {
                rk->rk_logq             = rd_kafka_q_new(rk);
                rk->rk_logq->rkq_serve  = rd_kafka_poll_cb;
                rk->rk_logq->rkq_opaque = rk;
        }

        TAILQ_INIT(&rk->rk_brokers);
        TAILQ_INIT(&rk->rk_topics);
        rd_kafka_timers_init(&rk->rk_timers, rk, rk->rk_ops);
        rd_kafka_metadata_cache_init(rk);
        rd_kafka_coord_cache_init(&rk->rk_coord_cache,
                                  rk->rk_conf.metadata_max_age_ms);
        rd_kafka_coord_reqs_init(rk);

        if (rk->rk_conf.dr_cb || rk->rk_conf.dr_msg_cb)
                rk->rk_drmode = RD_KAFKA_DR_MODE_CB;
        else if (rk->rk_conf.enabled_events & RD_KAFKA_EVENT_DR)
                rk->rk_drmode = RD_KAFKA_DR_MODE_EVENT;
        else
                rk->rk_drmode = RD_KAFKA_DR_MODE_NONE;
        if (rk->rk_drmode != RD_KAFKA_DR_MODE_NONE)
                rk->rk_conf.enabled_events |= RD_KAFKA_EVENT_DR;

        if (rk->rk_conf.rebalance_cb)
                rk->rk_conf.enabled_events |= RD_KAFKA_EVENT_REBALANCE;
        if (rk->rk_conf.offset_commit_cb)
                rk->rk_conf.enabled_events |= RD_KAFKA_EVENT_OFFSET_COMMIT;
        if (rk->rk_conf.error_cb)
                rk->rk_conf.enabled_events |= RD_KAFKA_EVENT_ERROR;
#if WITH_SASL_OAUTHBEARER
        if (rk->rk_conf.sasl.enable_oauthbearer_unsecure_jwt &&
            !rk->rk_conf.sasl.oauthbearer.token_refresh_cb)
                rd_kafka_conf_set_oauthbearer_token_refresh_cb(
                    &rk->rk_conf, rd_kafka_oauthbearer_unsecured_token);

        if (rk->rk_conf.sasl.oauthbearer.token_refresh_cb &&
            rk->rk_conf.sasl.oauthbearer.method !=
                RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC)
                rk->rk_conf.enabled_events |=
                    RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH;
#endif

#if WITH_OAUTHBEARER_OIDC
        if (rk->rk_conf.sasl.oauthbearer.method ==
                RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC &&
            !rk->rk_conf.sasl.oauthbearer.token_refresh_cb)
                rd_kafka_conf_set_oauthbearer_token_refresh_cb(
                    &rk->rk_conf, rd_kafka_oidc_token_refresh_cb);
#endif

        rk->rk_controllerid = -1;

        /* Admin client defaults */
        rk->rk_conf.admin.request_timeout_ms = rk->rk_conf.socket_timeout_ms;

        if (rk->rk_conf.debug)
                rk->rk_conf.log_level = LOG_DEBUG;

        rd_snprintf(rk->rk_name, sizeof(rk->rk_name), "%s#%s-%i",
                    rk->rk_conf.client_id_str, rd_kafka_type2str(rk->rk_type),
                    rd_atomic32_add(&rkid, 1));

        /* Construct clientid kafka string */
        rk->rk_client_id = rd_kafkap_str_new(rk->rk_conf.client_id_str, -1);

        /* Convert group.id to kafka string (may be NULL) */
        rk->rk_group_id = rd_kafkap_str_new(rk->rk_conf.group_id_str, -1);

        /* Config fixups */
        rk->rk_conf.queued_max_msg_bytes =
            (int64_t)rk->rk_conf.queued_max_msg_kbytes * 1000ll;

        /* Enable api.version.request=true if fallback.broker.version
         * indicates a supporting broker. */
        if (rd_kafka_ApiVersion_is_queryable(
                rk->rk_conf.broker_version_fallback))
                rk->rk_conf.api_version_request = 1;

        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                mtx_init(&rk->rk_curr_msgs.lock, mtx_plain);
                cnd_init(&rk->rk_curr_msgs.cnd);
                rk->rk_curr_msgs.max_cnt = rk->rk_conf.queue_buffering_max_msgs;
                if ((unsigned long long)rk->rk_conf.queue_buffering_max_kbytes *
                        1024 >
                    (unsigned long long)SIZE_MAX) {
                        rk->rk_curr_msgs.max_size = SIZE_MAX;
                        rd_kafka_log(rk, LOG_WARNING, "QUEUESIZE",
                                     "queue.buffering.max.kbytes adjusted "
                                     "to system SIZE_MAX limit %" PRIusz
                                     " bytes",
                                     rk->rk_curr_msgs.max_size);
                } else {
                        rk->rk_curr_msgs.max_size =
                            (size_t)rk->rk_conf.queue_buffering_max_kbytes *
                            1024;
                }
        }

        if (rd_kafka_assignors_init(rk, errstr, errstr_size) == -1) {
                ret_err   = RD_KAFKA_RESP_ERR__INVALID_ARG;
                ret_errno = EINVAL;
                goto fail;
        }

        if (!rk->rk_conf.group_remote_assignor) {
                rd_kafka_assignor_t *cooperative_assignor;

                /* Detect if chosen assignor is cooperative
                 * FIXME: remove this compatibility altogether
                 * and apply the breaking changes that will be required
                 * in next major version. */

                cooperative_assignor =
                    rd_kafka_assignor_find(rk, "cooperative-sticky");
                rk->rk_conf.partition_assignors_cooperative =
                    !rk->rk_conf.partition_assignors.rl_cnt ||
                    (cooperative_assignor &&
                     cooperative_assignor->rkas_enabled);

                if (rk->rk_conf.group_protocol ==
                    RD_KAFKA_GROUP_PROTOCOL_CONSUMER) {
                        /* Default remote assignor to the chosen local one. */
                        if (rk->rk_conf.partition_assignors_cooperative) {
                                group_remote_assignor_override =
                                    rd_strdup("uniform");
                                rk->rk_conf.group_remote_assignor =
                                    group_remote_assignor_override;
                        } else {
                                rd_kafka_assignor_t *range_assignor =
                                    rd_kafka_assignor_find(rk, "range");
                                if (range_assignor &&
                                    range_assignor->rkas_enabled) {
                                        rd_kafka_log(
                                            rk, LOG_WARNING, "ASSIGNOR",
                                            "\"range\" assignor is sticky "
                                            "with group protocol CONSUMER");
                                        group_remote_assignor_override =
                                            rd_strdup("range");
                                        rk->rk_conf.group_remote_assignor =
                                            group_remote_assignor_override;
                                } else {
                                        rd_kafka_log(
                                            rk, LOG_WARNING, "ASSIGNOR",
                                            "roundrobin assignor isn't "
                                            "available "
                                            "with group protocol CONSUMER, "
                                            "using the \"uniform\" one. "
                                            "It's similar, "
                                            "but it's also sticky");
                                        group_remote_assignor_override =
                                            rd_strdup("uniform");
                                        rk->rk_conf.group_remote_assignor =
                                            group_remote_assignor_override;
                                }
                        }
                }
        } else {
                /* When users starts setting properties of the new protocol,
                 * they can only use incremental_assign/unassign. */
                rk->rk_conf.partition_assignors_cooperative = rd_true;
        }

        /* Create Mock cluster */
        rd_atomic32_init(&rk->rk_mock.cluster_cnt, 0);
        if (rk->rk_conf.mock.broker_cnt > 0) {
                const char *mock_bootstraps;
                rk->rk_mock.cluster =
                    rd_kafka_mock_cluster_new(rk, rk->rk_conf.mock.broker_cnt);

                if (!rk->rk_mock.cluster) {
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to create mock cluster, see logs");
                        ret_err   = RD_KAFKA_RESP_ERR__FAIL;
                        ret_errno = EINVAL;
                        goto fail;
                }

                mock_bootstraps =
                    rd_kafka_mock_cluster_bootstraps(rk->rk_mock.cluster),
                rd_kafka_log(rk, LOG_NOTICE, "MOCK",
                             "Mock cluster enabled: "
                             "original bootstrap.servers and security.protocol "
                             "ignored and replaced with %s",
                             mock_bootstraps);

                /* Overwrite bootstrap.servers and connection settings */
                if (rd_kafka_conf_set(&rk->rk_conf, "bootstrap.servers",
                                      mock_bootstraps, NULL,
                                      0) != RD_KAFKA_CONF_OK)
                        rd_assert(!"failed to replace mock bootstrap.servers");

                if (rd_kafka_conf_set(&rk->rk_conf, "security.protocol",
                                      "plaintext", NULL, 0) != RD_KAFKA_CONF_OK)
                        rd_assert(!"failed to reset mock security.protocol");

                rk->rk_conf.security_protocol = RD_KAFKA_PROTO_PLAINTEXT;

                /* Apply default RTT to brokers */
                if (rk->rk_conf.mock.broker_rtt)
                        rd_kafka_mock_broker_set_rtt(
                            rk->rk_mock.cluster, -1 /*all brokers*/,
                            rk->rk_conf.mock.broker_rtt);
        }

        if (rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_SSL ||
            rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_PLAINTEXT) {
                /* Select SASL provider */
                if (rd_kafka_sasl_select_provider(rk, errstr, errstr_size) ==
                    -1) {
                        ret_err   = RD_KAFKA_RESP_ERR__INVALID_ARG;
                        ret_errno = EINVAL;
                        goto fail;
                }

                /* Initialize SASL provider */
                if (rd_kafka_sasl_init(rk, errstr, errstr_size) == -1) {
                        rk->rk_conf.sasl.provider = NULL;
                        ret_err   = RD_KAFKA_RESP_ERR__INVALID_ARG;
                        ret_errno = EINVAL;
                        goto fail;
                }
        }

#if WITH_SSL
        if (rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SSL ||
            rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_SSL) {
                /* Create SSL context */
                if (rd_kafka_ssl_ctx_init(rk, errstr, errstr_size) == -1) {
                        ret_err   = RD_KAFKA_RESP_ERR__INVALID_ARG;
                        ret_errno = EINVAL;
                        goto fail;
                }
        }
#endif

        if (type == RD_KAFKA_CONSUMER) {
                rd_kafka_assignment_init(rk);

                if (RD_KAFKAP_STR_LEN(rk->rk_group_id) > 0) {
                        /* Create consumer group handle */
                        rk->rk_cgrp = rd_kafka_cgrp_new(
                            rk, rk->rk_conf.group_protocol, rk->rk_group_id,
                            rk->rk_client_id);
                        rk->rk_consumer.q =
                            rd_kafka_q_keep(rk->rk_cgrp->rkcg_q);
                } else {
                        /* Legacy consumer */
                        rk->rk_consumer.q = rd_kafka_q_keep(rk->rk_rep);
                }

                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_poll_idle_ratio,
                    RD_AVG_GAUGE, 0, 1, 2, rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_poll_idle_ratio,
                    RD_AVG_GAUGE, 0, 1, 2, rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_rebalance_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_rebalance_latency,
                    RD_AVG_GAUGE, 0, 900000 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_rollover.rk_avg_commit_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);
                rd_avg_init(
                    &rk->rk_telemetry.rd_avg_current.rk_avg_commit_latency,
                    RD_AVG_GAUGE, 0, 500 * 1000, 2,
                    rk->rk_conf.enable_metrics_push);

        } else if (type == RD_KAFKA_PRODUCER) {
                rk->rk_eos.transactional_id =
                    rd_kafkap_str_new(rk->rk_conf.eos.transactional_id, -1);
        }

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

        /* Create background thread and queue if background_event_cb()
         * RD_KAFKA_EVENT_BACKGROUND has been enabled.
         * Do this before creating the main thread since after
         * the main thread is created it is no longer trivial to error
         * out from rd_kafka_new(). */
        if (rk->rk_conf.background_event_cb ||
            (rk->rk_conf.enabled_events & RD_KAFKA_EVENT_BACKGROUND)) {
                rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
                rd_kafka_wrlock(rk);
                if (!rk->rk_background.q)
                        err = rd_kafka_background_thread_create(rk, errstr,
                                                                errstr_size);
                rd_kafka_wrunlock(rk);
                if (err)
                        goto fail;
        }

        /* Lock handle here to synchronise state, i.e., hold off
         * the thread until we've finalized the handle. */
        rd_kafka_wrlock(rk);

        /* Create handler thread */
        mtx_lock(&rk->rk_init_lock);
        rk->rk_init_wait_cnt++;
        if ((thrd_create(&rk->rk_thread, rd_kafka_thread_main, rk)) !=
            thrd_success) {
                rk->rk_init_wait_cnt--;
                ret_err   = RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE;
                ret_errno = errno;
                if (errstr)
                        rd_snprintf(errstr, errstr_size,
                                    "Failed to create thread: %s (%i)",
                                    rd_strerror(errno), errno);
                mtx_unlock(&rk->rk_init_lock);
                rd_kafka_wrunlock(rk);
#ifndef _WIN32
                /* Restore sigmask of caller */
                pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif
                goto fail;
        }

        mtx_unlock(&rk->rk_init_lock);
        rd_kafka_wrunlock(rk);

        /*
         * @warning `goto fail` is prohibited past this point
         */

        mtx_lock(&rk->rk_internal_rkb_lock);
        rk->rk_internal_rkb =
            rd_kafka_broker_add(rk, RD_KAFKA_INTERNAL, RD_KAFKA_PROTO_PLAINTEXT,
                                "", 0, RD_KAFKA_NODEID_UA);
        mtx_unlock(&rk->rk_internal_rkb_lock);

        /* Add initial list of brokers from configuration */
        if (rk->rk_conf.brokerlist) {
                if (rd_kafka_brokers_add0(rk, rk->rk_conf.brokerlist,
                                          rd_true) == 0)
                        rd_kafka_op_err(rk, RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN,
                                        "No brokers configured");
        }

#ifndef _WIN32
        /* Restore sigmask of caller */
        pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif

        /* Wait for background threads to fully initialize so that
         * the client instance is fully functional at the time it is
         * returned from the constructor. */
        if (rd_kafka_init_wait(rk, 60 * 1000) != 0) {
                /* This should never happen unless there is a bug
                 * or the OS is not scheduling the background threads.
                 * Either case there is no point in handling this gracefully
                 * in the current state since the thread joins are likely
                 * to hang as well. */
                mtx_lock(&rk->rk_init_lock);
                rd_kafka_log(rk, LOG_CRIT, "INIT",
                             "Failed to initialize %s: "
                             "%d background thread(s) did not initialize "
                             "within 60 seconds",
                             rk->rk_name, rk->rk_init_wait_cnt);
                if (errstr)
                        rd_snprintf(errstr, errstr_size,
                                    "Timed out waiting for "
                                    "%d background thread(s) to initialize",
                                    rk->rk_init_wait_cnt);
                mtx_unlock(&rk->rk_init_lock);

                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE,
                                        EDEADLK);
                return NULL;
        }

        rk->rk_initialized = 1;

        bflen = sizeof(builtin_features);
        if (rd_kafka_conf_get(&rk->rk_conf, "builtin.features",
                              builtin_features, &bflen) != RD_KAFKA_CONF_OK)
                rd_snprintf(builtin_features, sizeof(builtin_features), "?");
        rd_kafka_dbg(rk, ALL, "INIT",
                     "librdkafka v%s (0x%x) %s initialized "
                     "(builtin.features %s, %s, debug 0x%x)",
                     rd_kafka_version_str(), rd_kafka_version(), rk->rk_name,
                     builtin_features, BUILT_WITH, rk->rk_conf.debug);

        /* Log warnings for deprecated configuration */
        rd_kafka_conf_warn(rk);

        /* Debug dump configuration */
        if (rk->rk_conf.debug & RD_KAFKA_DBG_CONF) {
                rd_kafka_anyconf_dump_dbg(rk, _RK_GLOBAL, &rk->rk_conf,
                                          "Client configuration");
                if (rk->rk_conf.topic_conf)
                        rd_kafka_anyconf_dump_dbg(
                            rk, _RK_TOPIC, rk->rk_conf.topic_conf,
                            "Default topic configuration");
        }

        /* Free user supplied conf's base pointer on success,
         * but not the actual allocated fields since the struct
         * will have been copied in its entirety above. */
        if (app_conf)
                rd_free(app_conf);
        rd_kafka_set_last_error(0, 0);

        return rk;

fail:
        /*
         * Error out and clean up
         */

        /*
         * Tell background thread to terminate and wait for it to return.
         */
        rd_atomic32_set(&rk->rk_terminate, RD_KAFKA_DESTROY_F_TERMINATE);

        /* Terminate SASL provider */
        if (rk->rk_conf.sasl.provider)
                rd_kafka_sasl_term(rk);

        if (rk->rk_background.thread) {
                int res;
                thrd_join(rk->rk_background.thread, &res);
                rd_kafka_q_destroy_owner(rk->rk_background.q);
        }

        /* If on_new() interceptors have been called we also need
         * to allow interceptor clean-up by calling on_destroy() */
        rd_kafka_interceptors_on_destroy(rk);

        /* If rk_conf is a struct-copy of the application configuration
         * we need to avoid rk_conf fields from being freed from
         * rd_kafka_destroy_internal() since they belong to app_conf.
         * However, there are some internal fields, such as interceptors,
         * that belong to rk_conf and thus needs to be cleaned up.
         * Legacy APIs, sigh.. */
        if (app_conf) {
                if (group_remote_assignor_override)
                        rd_free(group_remote_assignor_override);
                rd_kafka_assignors_term(rk);
                rd_kafka_interceptors_destroy(&rk->rk_conf);
                memset(&rk->rk_conf, 0, sizeof(rk->rk_conf));
        }

        rd_kafka_destroy_internal(rk);
        rd_kafka_destroy_final(rk);

        rd_kafka_set_last_error(ret_err, ret_errno);

        return NULL;
}



/**
 * Counts usage of the legacy/simple consumer (rd_kafka_consume_start() with
 * friends) since it does not have an API for stopping the cgrp we will need to
 * sort that out automatically in the background when all consumption
 * has stopped.
 *
 * Returns 0 if a  High level consumer is already instantiated
 * which means a Simple consumer cannot co-operate with it, else 1.
 *
 * A rd_kafka_t handle can never migrate from simple to high-level, or
 * vice versa, so we dont need a ..consumer_del().
 */
int rd_kafka_simple_consumer_add(rd_kafka_t *rk) {
        if (rd_atomic32_get(&rk->rk_simple_cnt) < 0)
                return 0;

        return (int)rd_atomic32_add(&rk->rk_simple_cnt, 1);
}



/**
 * rktp fetch is split up in these parts:
 *   * application side:
 *   * broker side (handled by current leader broker thread for rktp):
 *          - the fetch state, initial offset, etc.
 *          - fetching messages, updating fetched offset, etc.
 *          - offset commits
 *
 * Communication between the two are:
 *    app side -> rdkafka main side: rktp_ops
 *    broker thread -> app side: rktp_fetchq
 *
 * There is no shared state between these threads, instead
 * state is communicated through the two op queues, and state synchronization
 * is performed by version barriers.
 *
 */

static RD_UNUSED int rd_kafka_consume_start0(rd_kafka_topic_t *rkt,
                                             int32_t partition,
                                             int64_t offset,
                                             rd_kafka_q_t *rkq) {
        rd_kafka_toppar_t *rktp;

        if (partition < 0) {
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                        ESRCH);
                return -1;
        }

        if (!rd_kafka_simple_consumer_add(rkt->rkt_rk)) {
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return -1;
        }

        rd_kafka_topic_wrlock(rkt);
        rktp = rd_kafka_toppar_desired_add(rkt, partition);
        rd_kafka_topic_wrunlock(rkt);

        /* Verify offset */
        if (offset == RD_KAFKA_OFFSET_BEGINNING ||
            offset == RD_KAFKA_OFFSET_END ||
            offset <= RD_KAFKA_OFFSET_TAIL_BASE) {
                /* logical offsets */

        } else if (offset == RD_KAFKA_OFFSET_STORED) {
                /* offset manager */

                if (rkt->rkt_conf.offset_store_method ==
                        RD_KAFKA_OFFSET_METHOD_BROKER &&
                    RD_KAFKAP_STR_IS_NULL(rkt->rkt_rk->rk_group_id)) {
                        /* Broker based offsets require a group id. */
                        rd_kafka_toppar_destroy(rktp);
                        rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                                EINVAL);
                        return -1;
                }

        } else if (offset < 0) {
                rd_kafka_toppar_destroy(rktp);
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return -1;
        }

        rd_kafka_toppar_op_fetch_start(rktp, RD_KAFKA_FETCH_POS(offset, -1),
                                       rkq, RD_KAFKA_NO_REPLYQ);

        rd_kafka_toppar_destroy(rktp);

        rd_kafka_set_last_error(0, 0);
        return 0;
}



int rd_kafka_consume_start(rd_kafka_topic_t *app_rkt,
                           int32_t partition,
                           int64_t offset) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_dbg(rkt->rkt_rk, TOPIC, "START",
                     "Start consuming partition %" PRId32, partition);
        return rd_kafka_consume_start0(rkt, partition, offset, NULL);
}

int rd_kafka_consume_start_queue(rd_kafka_topic_t *app_rkt,
                                 int32_t partition,
                                 int64_t offset,
                                 rd_kafka_queue_t *rkqu) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);

        return rd_kafka_consume_start0(rkt, partition, offset, rkqu->rkqu_q);
}



static RD_UNUSED int rd_kafka_consume_stop0(rd_kafka_toppar_t *rktp) {
        rd_kafka_q_t *tmpq = NULL;
        rd_kafka_resp_err_t err;

        rd_kafka_topic_wrlock(rktp->rktp_rkt);
        rd_kafka_toppar_lock(rktp);
        rd_kafka_toppar_desired_del(rktp);
        rd_kafka_toppar_unlock(rktp);
        rd_kafka_topic_wrunlock(rktp->rktp_rkt);

        tmpq = rd_kafka_q_new(rktp->rktp_rkt->rkt_rk);

        rd_kafka_toppar_op_fetch_stop(rktp, RD_KAFKA_REPLYQ(tmpq, 0));

        /* Synchronisation: Wait for stop reply from broker thread */
        err = rd_kafka_q_wait_result(tmpq, RD_POLL_INFINITE);
        rd_kafka_q_destroy_owner(tmpq);

        rd_kafka_set_last_error(err, err ? EINVAL : 0);

        return err ? -1 : 0;
}


int rd_kafka_consume_stop(rd_kafka_topic_t *app_rkt, int32_t partition) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        int r;

        if (partition == RD_KAFKA_PARTITION_UA) {
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INVALID_ARG, EINVAL);
                return -1;
        }

        rd_kafka_topic_wrlock(rkt);
        if (!(rktp = rd_kafka_toppar_get(rkt, partition, 0)) &&
            !(rktp = rd_kafka_toppar_desired_get(rkt, partition))) {
                rd_kafka_topic_wrunlock(rkt);
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                        ESRCH);
                return -1;
        }
        rd_kafka_topic_wrunlock(rkt);

        r = rd_kafka_consume_stop0(rktp);
        /* set_last_error() called by stop0() */

        rd_kafka_toppar_destroy(rktp);

        return r;
}



rd_kafka_resp_err_t rd_kafka_seek(rd_kafka_topic_t *app_rkt,
                                  int32_t partition,
                                  int64_t offset,
                                  int timeout_ms) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        rd_kafka_q_t *tmpq = NULL;
        rd_kafka_resp_err_t err;
        rd_kafka_replyq_t replyq = RD_KAFKA_NO_REPLYQ;

        /* FIXME: simple consumer check */

        if (partition == RD_KAFKA_PARTITION_UA)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        rd_kafka_topic_rdlock(rkt);
        if (!(rktp = rd_kafka_toppar_get(rkt, partition, 0)) &&
            !(rktp = rd_kafka_toppar_desired_get(rkt, partition))) {
                rd_kafka_topic_rdunlock(rkt);
                return RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
        }
        rd_kafka_topic_rdunlock(rkt);

        if (timeout_ms) {
                tmpq   = rd_kafka_q_new(rkt->rkt_rk);
                replyq = RD_KAFKA_REPLYQ(tmpq, 0);
        }

        if ((err = rd_kafka_toppar_op_seek(rktp, RD_KAFKA_FETCH_POS(offset, -1),
                                           replyq))) {
                if (tmpq)
                        rd_kafka_q_destroy_owner(tmpq);
                rd_kafka_toppar_destroy(rktp);
                return err;
        }

        rd_kafka_toppar_destroy(rktp);

        if (tmpq) {
                err = rd_kafka_q_wait_result(tmpq, timeout_ms);
                rd_kafka_q_destroy_owner(tmpq);
                return err;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


rd_kafka_error_t *
rd_kafka_seek_partitions(rd_kafka_t *rk,
                         rd_kafka_topic_partition_list_t *partitions,
                         int timeout_ms) {
        rd_kafka_q_t *tmpq = NULL;
        rd_kafka_topic_partition_t *rktpar;
        rd_ts_t abs_timeout = rd_timeout_init(timeout_ms);
        int cnt             = 0;

        if (rk->rk_type != RD_KAFKA_CONSUMER)
                return rd_kafka_error_new(
                    RD_KAFKA_RESP_ERR__INVALID_ARG,
                    "Must only be used on consumer instance");

        if (!partitions || partitions->cnt == 0)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "partitions must be specified");

        if (timeout_ms)
                tmpq = rd_kafka_q_new(rk);

        RD_KAFKA_TPLIST_FOREACH(rktpar, partitions) {
                rd_kafka_toppar_t *rktp;
                rd_kafka_resp_err_t err;

                rktp = rd_kafka_toppar_get2(
                    rk, rktpar->topic, rktpar->partition,
                    rd_false /*no-ua-on-miss*/, rd_false /*no-create-on-miss*/);
                if (!rktp) {
                        rktpar->err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
                        continue;
                }

                err = rd_kafka_toppar_op_seek(
                    rktp, rd_kafka_topic_partition_get_fetch_pos(rktpar),
                    RD_KAFKA_REPLYQ(tmpq, 0));
                if (err) {
                        rktpar->err = err;
                } else {
                        rktpar->err = RD_KAFKA_RESP_ERR__IN_PROGRESS;
                        cnt++;
                }

                rd_kafka_toppar_destroy(rktp); /* refcnt from toppar_get2() */
        }

        if (!timeout_ms)
                return NULL;


        while (cnt > 0) {
                rd_kafka_op_t *rko;

                rko =
                    rd_kafka_q_pop(tmpq, rd_timeout_remains_us(abs_timeout), 0);
                if (!rko) {
                        rd_kafka_q_destroy_owner(tmpq);

                        return rd_kafka_error_new(
                            RD_KAFKA_RESP_ERR__TIMED_OUT,
                            "Timed out waiting for %d remaining partition "
                            "seek(s) to finish",
                            cnt);
                }

                if (rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY) {
                        rd_kafka_q_destroy_owner(tmpq);
                        rd_kafka_op_destroy(rko);

                        return rd_kafka_error_new(RD_KAFKA_RESP_ERR__DESTROY,
                                                  "Instance is terminating");
                }

                rd_assert(rko->rko_rktp);

                rktpar = rd_kafka_topic_partition_list_find(
                    partitions, rko->rko_rktp->rktp_rkt->rkt_topic->str,
                    rko->rko_rktp->rktp_partition);
                rd_assert(rktpar);

                rktpar->err = rko->rko_err;

                rd_kafka_op_destroy(rko);

                cnt--;
        }

        rd_kafka_q_destroy_owner(tmpq);

        return NULL;
}



static ssize_t rd_kafka_consume_batch0(rd_kafka_q_t *rkq,
                                       int timeout_ms,
                                       rd_kafka_message_t **rkmessages,
                                       size_t rkmessages_size) {
        /* Populate application's rkmessages array. */
        return rd_kafka_q_serve_rkmessages(rkq, timeout_ms, rkmessages,
                                           rkmessages_size);
}


ssize_t rd_kafka_consume_batch(rd_kafka_topic_t *app_rkt,
                               int32_t partition,
                               int timeout_ms,
                               rd_kafka_message_t **rkmessages,
                               size_t rkmessages_size) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        ssize_t cnt;

        /* Get toppar */
        rd_kafka_topic_rdlock(rkt);
        rktp = rd_kafka_toppar_get(rkt, partition, 0 /*no ua on miss*/);
        if (unlikely(!rktp))
                rktp = rd_kafka_toppar_desired_get(rkt, partition);
        rd_kafka_topic_rdunlock(rkt);

        if (unlikely(!rktp)) {
                /* No such toppar known */
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                        ESRCH);
                return -1;
        }

        /* Populate application's rkmessages array. */
        cnt = rd_kafka_q_serve_rkmessages(rktp->rktp_fetchq, timeout_ms,
                                          rkmessages, rkmessages_size);

        rd_kafka_toppar_destroy(rktp); /* refcnt from .._get() */

        rd_kafka_set_last_error(0, 0);

        return cnt;
}

ssize_t rd_kafka_consume_batch_queue(rd_kafka_queue_t *rkqu,
                                     int timeout_ms,
                                     rd_kafka_message_t **rkmessages,
                                     size_t rkmessages_size) {
        /* Populate application's rkmessages array. */
        return rd_kafka_consume_batch0(rkqu->rkqu_q, timeout_ms, rkmessages,
                                       rkmessages_size);
}


struct consume_ctx {
        void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque);
        void *opaque;
};


/**
 * Trampoline for application's consume_cb()
 */
static rd_kafka_op_res_t rd_kafka_consume_cb(rd_kafka_t *rk,
                                             rd_kafka_q_t *rkq,
                                             rd_kafka_op_t *rko,
                                             rd_kafka_q_cb_type_t cb_type,
                                             void *opaque) {
        struct consume_ctx *ctx = opaque;
        rd_kafka_message_t *rkmessage;

        if (unlikely(rd_kafka_op_version_outdated(rko, 0)) ||
            rko->rko_type == RD_KAFKA_OP_BARRIER) {
                rd_kafka_op_destroy(rko);
                return RD_KAFKA_OP_RES_HANDLED;
        }

        rkmessage = rd_kafka_message_get(rko);

        rd_kafka_fetch_op_app_prepare(rk, rko);

        ctx->consume_cb(rkmessage, ctx->opaque);

        rd_kafka_op_destroy(rko);

        return RD_KAFKA_OP_RES_HANDLED;
}



static rd_kafka_op_res_t rd_kafka_consume_callback0(
    rd_kafka_q_t *rkq,
    int timeout_ms,
    int max_cnt,
    void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque),
    void *opaque) {
        struct consume_ctx ctx = {.consume_cb = consume_cb, .opaque = opaque};
        rd_kafka_op_res_t res;

        rd_kafka_app_poll_start(rkq->rkq_rk, 0, timeout_ms);

        res = rd_kafka_q_serve(rkq, timeout_ms, max_cnt, RD_KAFKA_Q_CB_RETURN,
                               rd_kafka_consume_cb, &ctx);

        rd_kafka_app_polled(rkq->rkq_rk);

        return res;
}


int rd_kafka_consume_callback(rd_kafka_topic_t *app_rkt,
                              int32_t partition,
                              int timeout_ms,
                              void (*consume_cb)(rd_kafka_message_t *rkmessage,
                                                 void *opaque),
                              void *opaque) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        int r;

        /* Get toppar */
        rd_kafka_topic_rdlock(rkt);
        rktp = rd_kafka_toppar_get(rkt, partition, 0 /*no ua on miss*/);
        if (unlikely(!rktp))
                rktp = rd_kafka_toppar_desired_get(rkt, partition);
        rd_kafka_topic_rdunlock(rkt);

        if (unlikely(!rktp)) {
                /* No such toppar known */
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                        ESRCH);
                return -1;
        }

        r = rd_kafka_consume_callback0(rktp->rktp_fetchq, timeout_ms,
                                       rkt->rkt_conf.consume_callback_max_msgs,
                                       consume_cb, opaque);

        rd_kafka_toppar_destroy(rktp);

        rd_kafka_set_last_error(0, 0);

        return r;
}



int rd_kafka_consume_callback_queue(
    rd_kafka_queue_t *rkqu,
    int timeout_ms,
    void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque),
    void *opaque) {
        return rd_kafka_consume_callback0(rkqu->rkqu_q, timeout_ms, 0,
                                          consume_cb, opaque);
}


/**
 * Serve queue 'rkq' and return one message.
 * By serving the queue it will also call any registered callbacks
 * registered for matching events, this includes consumer_cb()
 * in which case no message will be returned.
 */
static rd_kafka_message_t *
rd_kafka_consume0(rd_kafka_t *rk, rd_kafka_q_t *rkq, int timeout_ms) {
        rd_kafka_op_t *rko;
        rd_kafka_message_t *rkmessage = NULL;
        rd_ts_t now                   = rd_clock();
        rd_ts_t abs_timeout           = rd_timeout_init0(now, timeout_ms);

        rd_kafka_app_poll_start(rk, now, timeout_ms);

        rd_kafka_yield_thread = 0;
        while ((
            rko = rd_kafka_q_pop(rkq, rd_timeout_remains_us(abs_timeout), 0))) {
                rd_kafka_op_res_t res;
                res =
                    rd_kafka_poll_cb(rk, rkq, rko, RD_KAFKA_Q_CB_RETURN, NULL);

                if (res == RD_KAFKA_OP_RES_PASS)
                        break;

                if (unlikely(res == RD_KAFKA_OP_RES_YIELD ||
                             rd_kafka_yield_thread)) {
                        /* Callback called rd_kafka_yield(), we must
                         * stop dispatching the queue and return. */
                        rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__INTR, EINTR);
                        rd_kafka_app_polled(rk);
                        return NULL;
                }

                /* Message was handled by callback. */
                continue;
        }

        if (!rko) {
                /* Timeout reached with no op returned. */
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__TIMED_OUT,
                                        ETIMEDOUT);
                rd_kafka_app_polled(rk);
                return NULL;
        }

        rd_kafka_assert(rk, rko->rko_type == RD_KAFKA_OP_FETCH ||
                                rko->rko_type == RD_KAFKA_OP_CONSUMER_ERR);

        /* Get rkmessage from rko */
        rkmessage = rd_kafka_message_get(rko);

        /* Store offset, etc */
        rd_kafka_fetch_op_app_prepare(rk, rko);

        rd_kafka_set_last_error(0, 0);

        rd_kafka_app_polled(rk);

        return rkmessage;
}

rd_kafka_message_t *
rd_kafka_consume(rd_kafka_topic_t *app_rkt, int32_t partition, int timeout_ms) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        rd_kafka_message_t *rkmessage;

        rd_kafka_topic_rdlock(rkt);
        rktp = rd_kafka_toppar_get(rkt, partition, 0 /*no ua on miss*/);
        if (unlikely(!rktp))
                rktp = rd_kafka_toppar_desired_get(rkt, partition);
        rd_kafka_topic_rdunlock(rkt);

        if (unlikely(!rktp)) {
                /* No such toppar known */
                rd_kafka_set_last_error(RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION,
                                        ESRCH);
                return NULL;
        }

        rkmessage =
            rd_kafka_consume0(rkt->rkt_rk, rktp->rktp_fetchq, timeout_ms);

        rd_kafka_toppar_destroy(rktp); /* refcnt from .._get() */

        return rkmessage;
}


rd_kafka_message_t *rd_kafka_consume_queue(rd_kafka_queue_t *rkqu,
                                           int timeout_ms) {
        return rd_kafka_consume0(rkqu->rkqu_rk, rkqu->rkqu_q, timeout_ms);
}



rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t *rk) {
        rd_kafka_cgrp_t *rkcg;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        rd_kafka_q_fwd_set(rk->rk_rep, rkcg->rkcg_q);
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



rd_kafka_message_t *rd_kafka_consumer_poll(rd_kafka_t *rk, int timeout_ms) {
        rd_kafka_cgrp_t *rkcg;

        if (unlikely(!(rkcg = rd_kafka_cgrp_get(rk)))) {
                rd_kafka_message_t *rkmessage = rd_kafka_message_new();
                rkmessage->err = RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;
                return rkmessage;
        }

        return rd_kafka_consume0(rk, rkcg->rkcg_q, timeout_ms);
}


/**
 * @brief Consumer close.
 *
 * @param rkq The consumer group queue will be forwarded to this queue, which
 *            which must be served (rebalance events) by the application/caller
 *            until rd_kafka_consumer_closed() returns true.
 *            If the consumer is not in a joined state, no rebalance events
 *            will be emitted.
 */
static rd_kafka_error_t *rd_kafka_consumer_close_q(rd_kafka_t *rk,
                                                   rd_kafka_q_t *rkq) {
        rd_kafka_cgrp_t *rkcg;
        rd_kafka_error_t *error = NULL;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__UNKNOWN_GROUP,
                                          "Consume close called on non-group "
                                          "consumer");

        if (rd_atomic32_get(&rkcg->rkcg_terminated))
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__DESTROY,
                                          "Consumer already closed");

        /* If a fatal error has been raised and this is an
         * explicit consumer_close() from the application we return
         * a fatal error. Otherwise let the "silent" no_consumer_close
         * logic be performed to clean up properly. */
        if (!rd_kafka_destroy_flags_no_consumer_close(rk) &&
            (error = rd_kafka_get_fatal_error(rk)))
                return error;

        rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "CLOSE",
                     "Closing consumer");

        /* Redirect cgrp queue to the rebalance queue to make sure all posted
         * ops (e.g., rebalance callbacks) are served by
         * the application/caller. */
        rd_kafka_q_fwd_set(rkcg->rkcg_q, rkq);

        /* Tell cgrp subsystem to terminate. A TERMINATE op will be posted
         * on the rkq when done. */
        rd_kafka_cgrp_terminate(rkcg, RD_KAFKA_REPLYQ(rkq, 0)); /* async */

        return error;
}

rd_kafka_error_t *rd_kafka_consumer_close_queue(rd_kafka_t *rk,
                                                rd_kafka_queue_t *rkqu) {
        if (!rkqu)
                return rd_kafka_error_new(RD_KAFKA_RESP_ERR__INVALID_ARG,
                                          "Queue must be specified");
        return rd_kafka_consumer_close_q(rk, rkqu->rkqu_q);
}

rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk) {
        rd_kafka_error_t *error;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR__TIMED_OUT;
        rd_kafka_q_t *rkq;

        /* Create a temporary reply queue to handle the TERMINATE reply op. */
        rkq = rd_kafka_q_new(rk);

        /* Initiate the close (async) */
        error = rd_kafka_consumer_close_q(rk, rkq);
        if (error) {
                err = rd_kafka_error_is_fatal(error)
                          ? RD_KAFKA_RESP_ERR__FATAL
                          : rd_kafka_error_code(error);
                rd_kafka_error_destroy(error);
                rd_kafka_q_destroy_owner(rkq);
                return err;
        }

        /* Disable the queue if termination is immediate or the user
         * does not want the blocking consumer_close() behaviour, this will
         * cause any ops posted for this queue (such as rebalance) to
         * be destroyed.
         */
        if (rd_kafka_destroy_flags_no_consumer_close(rk)) {
                rd_kafka_dbg(rk, CONSUMER, "CLOSE",
                             "Disabling and purging temporary queue to quench "
                             "close events");
                err = RD_KAFKA_RESP_ERR_NO_ERROR;
                rd_kafka_q_disable(rkq);
                /* Purge ops already enqueued */
                rd_kafka_q_purge(rkq);
        } else {
                rd_kafka_op_t *rko;
                rd_kafka_dbg(rk, CONSUMER, "CLOSE", "Waiting for close events");
                while ((rko = rd_kafka_q_pop(rkq, RD_POLL_INFINITE, 0))) {
                        rd_kafka_op_res_t res;
                        if ((rko->rko_type & ~RD_KAFKA_OP_FLAGMASK) ==
                            RD_KAFKA_OP_TERMINATE) {
                                err = rko->rko_err;
                                rd_kafka_op_destroy(rko);
                                break;
                        }
                        /* Handle callbacks */
                        res = rd_kafka_poll_cb(rk, rkq, rko,
                                               RD_KAFKA_Q_CB_RETURN, NULL);
                        if (res == RD_KAFKA_OP_RES_PASS)
                                rd_kafka_op_destroy(rko);
                        /* Ignore YIELD, we need to finish */
                }
        }

        rd_kafka_q_destroy_owner(rkq);

        if (err)
                rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "CLOSE",
                             "Consumer closed with error: %s",
                             rd_kafka_err2str(err));
        else
                rd_kafka_dbg(rk, CONSUMER | RD_KAFKA_DBG_CGRP, "CLOSE",
                             "Consumer closed");

        return err;
}


int rd_kafka_consumer_closed(rd_kafka_t *rk) {
        if (unlikely(!rk->rk_cgrp))
                return 0;

        return rd_atomic32_get(&rk->rk_cgrp->rkcg_terminated);
}


rd_kafka_resp_err_t
rd_kafka_committed(rd_kafka_t *rk,
                   rd_kafka_topic_partition_list_t *partitions,
                   int timeout_ms) {
        rd_kafka_q_t *rkq;
        rd_kafka_resp_err_t err;
        rd_kafka_cgrp_t *rkcg;
        rd_ts_t abs_timeout = rd_timeout_init(timeout_ms);

        if (!partitions)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        /* Set default offsets. */
        rd_kafka_topic_partition_list_reset_offsets(partitions,
                                                    RD_KAFKA_OFFSET_INVALID);

        rkq = rd_kafka_q_new(rk);

        do {
                rd_kafka_op_t *rko;
                int state_version = rd_kafka_brokers_get_state_version(rk);

                rko = rd_kafka_op_new(RD_KAFKA_OP_OFFSET_FETCH);
                rd_kafka_op_set_replyq(rko, rkq, NULL);

                /* Issue #827
                 * Copy partition list to avoid use-after-free if we time out
                 * here, the app frees the list, and then cgrp starts
                 * processing the op. */
                rko->rko_u.offset_fetch.partitions =
                    rd_kafka_topic_partition_list_copy(partitions);
                rko->rko_u.offset_fetch.require_stable_offsets =
                    rk->rk_conf.isolation_level == RD_KAFKA_READ_COMMITTED;
                rko->rko_u.offset_fetch.do_free = 1;

                if (!rd_kafka_q_enq(rkcg->rkcg_ops, rko)) {
                        err = RD_KAFKA_RESP_ERR__DESTROY;
                        break;
                }

                rko =
                    rd_kafka_q_pop(rkq, rd_timeout_remains_us(abs_timeout), 0);
                if (rko) {
                        if (!(err = rko->rko_err))
                                rd_kafka_topic_partition_list_update(
                                    partitions,
                                    rko->rko_u.offset_fetch.partitions);
                        else if ((err == RD_KAFKA_RESP_ERR__WAIT_COORD ||
                                  err == RD_KAFKA_RESP_ERR__TRANSPORT) &&
                                 !rd_kafka_brokers_wait_state_change(
                                     rk, state_version,
                                     rd_timeout_remains(abs_timeout)))
                                err = RD_KAFKA_RESP_ERR__TIMED_OUT;

                        rd_kafka_op_destroy(rko);
                } else
                        err = RD_KAFKA_RESP_ERR__TIMED_OUT;
        } while (err == RD_KAFKA_RESP_ERR__TRANSPORT ||
                 err == RD_KAFKA_RESP_ERR__WAIT_COORD);

        rd_kafka_q_destroy_owner(rkq);

        return err;
}



rd_kafka_resp_err_t
rd_kafka_position(rd_kafka_t *rk, rd_kafka_topic_partition_list_t *partitions) {
        int i;

        for (i = 0; i < partitions->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &partitions->elems[i];
                rd_kafka_toppar_t *rktp;

                if (!(rktp = rd_kafka_toppar_get2(rk, rktpar->topic,
                                                  rktpar->partition, 0, 1))) {
                        rktpar->err    = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
                        rktpar->offset = RD_KAFKA_OFFSET_INVALID;
                        continue;
                }

                rd_kafka_toppar_lock(rktp);
                rd_kafka_topic_partition_set_from_fetch_pos(rktpar,
                                                            rktp->rktp_app_pos);
                rd_kafka_toppar_unlock(rktp);
                rd_kafka_toppar_destroy(rktp);

                rktpar->err = RD_KAFKA_RESP_ERR_NO_ERROR;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



struct _query_wmark_offsets_state {
        rd_kafka_resp_err_t err;
        const char *topic;
        int32_t partition;
        int64_t offsets[2];
        int offidx; /* next offset to set from response */
        rd_ts_t ts_end;
        int state_version; /* Broker state version */
};

static void rd_kafka_query_wmark_offsets_resp_cb(rd_kafka_t *rk,
                                                 rd_kafka_broker_t *rkb,
                                                 rd_kafka_resp_err_t err,
                                                 rd_kafka_buf_t *rkbuf,
                                                 rd_kafka_buf_t *request,
                                                 void *opaque) {
        struct _query_wmark_offsets_state *state;
        rd_kafka_topic_partition_list_t *offsets;
        rd_kafka_topic_partition_t *rktpar;
        int actions = 0;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* 'state' has gone out of scope when query_watermark..()
                 * timed out and returned to the caller. */
                return;
        }

        state = opaque;

        offsets = rd_kafka_topic_partition_list_new(1);
        err = rd_kafka_handle_ListOffsets(rk, rkb, err, rkbuf, request, offsets,
                                          &actions);

        if (actions & RD_KAFKA_ERR_ACTION_REFRESH) {
                /* Remove its cache in case the topic isn't a known topic. */
                rd_kafka_wrlock(rk);
                rd_kafka_metadata_cache_delete_by_name(rk, state->topic);
                rd_kafka_wrunlock(rk);
        }

        if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS) {
                rd_kafka_topic_partition_list_destroy(offsets);
                return; /* Retrying */
        }

        /* Retry if no broker connection is available yet. */
        if (err == RD_KAFKA_RESP_ERR__TRANSPORT && rkb &&
            rd_kafka_brokers_wait_state_change(
                rkb->rkb_rk, state->state_version,
                rd_timeout_remains(state->ts_end))) {
                /* Retry */
                state->state_version   = rd_kafka_brokers_get_state_version(rk);
                request->rkbuf_retries = 0;
                if (rd_kafka_buf_retry(rkb, request)) {
                        rd_kafka_topic_partition_list_destroy(offsets);
                        return; /* Retry in progress */
                }
                /* FALLTHRU */
        }

        rktpar = rd_kafka_topic_partition_list_find(offsets, state->topic,
                                                    state->partition);
        if (!rktpar && err > RD_KAFKA_RESP_ERR__END) {
                /* Partition not seen in response,
                 * not a local error. */
                err = RD_KAFKA_RESP_ERR__BAD_MSG;
        } else if (rktpar) {
                if (rktpar->err)
                        err = rktpar->err;
                else
                        state->offsets[state->offidx] = rktpar->offset;
        }

        state->offidx++;

        if (err || state->offidx == 2) /* Error or Done */
                state->err = err;

        rd_kafka_topic_partition_list_destroy(offsets);
}


rd_kafka_resp_err_t rd_kafka_query_watermark_offsets(rd_kafka_t *rk,
                                                     const char *topic,
                                                     int32_t partition,
                                                     int64_t *low,
                                                     int64_t *high,
                                                     int timeout_ms) {
        rd_kafka_q_t *rkq;
        struct _query_wmark_offsets_state state;
        rd_ts_t ts_end = rd_timeout_init(timeout_ms);
        rd_kafka_topic_partition_list_t *partitions;
        rd_kafka_topic_partition_t *rktpar;
        struct rd_kafka_partition_leader *leader;
        rd_list_t leaders;
        rd_kafka_resp_err_t err;

        partitions = rd_kafka_topic_partition_list_new(1);
        rktpar =
            rd_kafka_topic_partition_list_add(partitions, topic, partition);

        rd_list_init(&leaders, partitions->cnt,
                     (void *)rd_kafka_partition_leader_destroy);

        err = rd_kafka_topic_partition_list_query_leaders(rk, partitions,
                                                          &leaders, timeout_ms);
        if (err) {
                rd_list_destroy(&leaders);
                rd_kafka_topic_partition_list_destroy(partitions);
                return err;
        }

        leader = rd_list_elem(&leaders, 0);

        rkq = rd_kafka_q_new(rk);

        /* Due to KAFKA-1588 we need to send a request for each wanted offset,
         * in this case one for the low watermark and one for the high. */
        state.topic         = topic;
        state.partition     = partition;
        state.offsets[0]    = RD_KAFKA_OFFSET_BEGINNING;
        state.offsets[1]    = RD_KAFKA_OFFSET_END;
        state.offidx        = 0;
        state.err           = RD_KAFKA_RESP_ERR__IN_PROGRESS;
        state.ts_end        = ts_end;
        state.state_version = rd_kafka_brokers_get_state_version(rk);

        rktpar->offset = RD_KAFKA_OFFSET_BEGINNING;
        rd_kafka_ListOffsetsRequest(
            leader->rkb, partitions, RD_KAFKA_REPLYQ(rkq, 0),
            rd_kafka_query_wmark_offsets_resp_cb, timeout_ms, &state);

        rktpar->offset = RD_KAFKA_OFFSET_END;
        rd_kafka_ListOffsetsRequest(
            leader->rkb, partitions, RD_KAFKA_REPLYQ(rkq, 0),
            rd_kafka_query_wmark_offsets_resp_cb, timeout_ms, &state);

        rd_kafka_topic_partition_list_destroy(partitions);
        rd_list_destroy(&leaders);

        /* Wait for reply (or timeout) */
        while (state.err == RD_KAFKA_RESP_ERR__IN_PROGRESS) {
                rd_kafka_q_serve(rkq, RD_POLL_INFINITE, 0,
                                 RD_KAFKA_Q_CB_CALLBACK, rd_kafka_poll_cb,
                                 NULL);
        }

        rd_kafka_q_destroy_owner(rkq);

        if (state.err)
                return state.err;
        else if (state.offidx != 2)
                return RD_KAFKA_RESP_ERR__FAIL;

        /* We are not certain about the returned order. */
        if (state.offsets[0] < state.offsets[1]) {
                *low  = state.offsets[0];
                *high = state.offsets[1];
        } else {
                *low  = state.offsets[1];
                *high = state.offsets[0];
        }

        /* If partition is empty only one offset (the last) will be returned. */
        if (*low < 0 && *high >= 0)
                *low = *high;

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


rd_kafka_resp_err_t rd_kafka_get_watermark_offsets(rd_kafka_t *rk,
                                                   const char *topic,
                                                   int32_t partition,
                                                   int64_t *low,
                                                   int64_t *high) {
        rd_kafka_toppar_t *rktp;

        rktp = rd_kafka_toppar_get2(rk, topic, partition, 0, 1);
        if (!rktp)
                return RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;

        rd_kafka_toppar_lock(rktp);
        *low  = rktp->rktp_lo_offset;
        *high = rktp->rktp_hi_offset;
        rd_kafka_toppar_unlock(rktp);

        rd_kafka_toppar_destroy(rktp);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief get_offsets_for_times() state
 */
struct _get_offsets_for_times {
        rd_kafka_topic_partition_list_t *results;
        rd_kafka_resp_err_t err;
        int wait_reply;
        int state_version;
        rd_ts_t ts_end;
};

/**
 * @brief Handle OffsetRequest responses
 */
static void rd_kafka_get_offsets_for_times_resp_cb(rd_kafka_t *rk,
                                                   rd_kafka_broker_t *rkb,
                                                   rd_kafka_resp_err_t err,
                                                   rd_kafka_buf_t *rkbuf,
                                                   rd_kafka_buf_t *request,
                                                   void *opaque) {
        struct _get_offsets_for_times *state;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* 'state' has gone out of scope when offsets_for_times()
                 * timed out and returned to the caller. */
                return;
        }

        state = opaque;

        err = rd_kafka_handle_ListOffsets(rk, rkb, err, rkbuf, request,
                                          state->results, NULL);
        if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS)
                return; /* Retrying */

        /* Retry if no broker connection is available yet. */
        if (err == RD_KAFKA_RESP_ERR__TRANSPORT && rkb &&
            rd_kafka_brokers_wait_state_change(
                rkb->rkb_rk, state->state_version,
                rd_timeout_remains(state->ts_end))) {
                /* Retry */
                state->state_version   = rd_kafka_brokers_get_state_version(rk);
                request->rkbuf_retries = 0;
                if (rd_kafka_buf_retry(rkb, request))
                        return; /* Retry in progress */
                /* FALLTHRU */
        }

        if (err && !state->err)
                state->err = err;

        state->wait_reply--;
}


rd_kafka_resp_err_t
rd_kafka_offsets_for_times(rd_kafka_t *rk,
                           rd_kafka_topic_partition_list_t *offsets,
                           int timeout_ms) {
        rd_kafka_q_t *rkq;
        struct _get_offsets_for_times state = RD_ZERO_INIT;
        rd_ts_t ts_end                      = rd_timeout_init(timeout_ms);
        rd_list_t leaders;
        int i;
        rd_kafka_resp_err_t err;
        struct rd_kafka_partition_leader *leader;
        int tmout;

        if (offsets->cnt == 0)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        rd_list_init(&leaders, offsets->cnt,
                     (void *)rd_kafka_partition_leader_destroy);

        err = rd_kafka_topic_partition_list_query_leaders(rk, offsets, &leaders,
                                                          timeout_ms);
        if (err) {
                rd_list_destroy(&leaders);
                return err;
        }


        rkq = rd_kafka_q_new(rk);

        state.wait_reply = 0;
        state.results    = rd_kafka_topic_partition_list_new(offsets->cnt);

        /* For each leader send a request for its partitions */
        RD_LIST_FOREACH(leader, &leaders, i) {
                state.wait_reply++;
                rd_kafka_ListOffsetsRequest(
                    leader->rkb, leader->partitions, RD_KAFKA_REPLYQ(rkq, 0),
                    rd_kafka_get_offsets_for_times_resp_cb, timeout_ms, &state);
        }

        rd_list_destroy(&leaders);

        /* Wait for reply (or timeout) */
        while (state.wait_reply > 0 &&
               !rd_timeout_expired((tmout = rd_timeout_remains(ts_end))))
                rd_kafka_q_serve(rkq, tmout, 0, RD_KAFKA_Q_CB_CALLBACK,
                                 rd_kafka_poll_cb, NULL);

        rd_kafka_q_destroy_owner(rkq);

        if (state.wait_reply > 0 && !state.err)
                state.err = RD_KAFKA_RESP_ERR__TIMED_OUT;

        /* Then update the queried partitions. */
        if (!state.err)
                rd_kafka_topic_partition_list_update(offsets, state.results);

        rd_kafka_topic_partition_list_destroy(state.results);

        return state.err;
}


/**
 * @brief rd_kafka_poll() (and similar) op callback handler.
 *        Will either call registered callback depending on cb_type and op type
 *        or return op to application, if applicable (e.g., fetch message).
 *
 * @returns RD_KAFKA_OP_RES_HANDLED if op was handled, else one of the
 *          other res types (such as OP_RES_PASS).
 *
 * @locality any thread that serves op queues
 */
rd_kafka_op_res_t rd_kafka_poll_cb(rd_kafka_t *rk,
                                   rd_kafka_q_t *rkq,
                                   rd_kafka_op_t *rko,
                                   rd_kafka_q_cb_type_t cb_type,
                                   void *opaque) {
        rd_kafka_msg_t *rkm;
        rd_kafka_op_res_t res = RD_KAFKA_OP_RES_HANDLED;

        /* Special handling for events based on cb_type */
        if (cb_type == RD_KAFKA_Q_CB_EVENT && rd_kafka_event_setup(rk, rko)) {
                /* Return-as-event requested. */
                return RD_KAFKA_OP_RES_PASS; /* Return as event */
        }

        switch ((int)rko->rko_type) {
        case RD_KAFKA_OP_FETCH:
                if (!rk->rk_conf.consume_cb ||
                    cb_type == RD_KAFKA_Q_CB_RETURN ||
                    cb_type == RD_KAFKA_Q_CB_FORCE_RETURN)
                        return RD_KAFKA_OP_RES_PASS; /* Dont handle here */
                else {
                        rk->rk_ts_last_poll_end = rd_clock();
                        struct consume_ctx ctx  = {.consume_cb =
                                                      rk->rk_conf.consume_cb,
                                                  .opaque = rk->rk_conf.opaque};

                        return rd_kafka_consume_cb(rk, rkq, rko, cb_type, &ctx);
                }
                break;

        case RD_KAFKA_OP_REBALANCE:
                if (rk->rk_conf.rebalance_cb)
                        rk->rk_conf.rebalance_cb(
                            rk, rko->rko_err, rko->rko_u.rebalance.partitions,
                            rk->rk_conf.opaque);
                else {
                        /** If EVENT_REBALANCE is enabled but rebalance_cb
                         *  isn't, we need to perform a dummy assign for the
                         *  application. This might happen during termination
                         *  with consumer_close() */
                        rd_kafka_dbg(rk, CGRP, "UNASSIGN",
                                     "Forcing unassign of %d partition(s)",
                                     rko->rko_u.rebalance.partitions
                                         ? rko->rko_u.rebalance.partitions->cnt
                                         : 0);
                        rd_kafka_assign(rk, NULL);
                }
                break;

        case RD_KAFKA_OP_OFFSET_COMMIT | RD_KAFKA_OP_REPLY:
                if (!rko->rko_u.offset_commit.cb)
                        return RD_KAFKA_OP_RES_PASS; /* Dont handle here */
                rko->rko_u.offset_commit.cb(rk, rko->rko_err,
                                            rko->rko_u.offset_commit.partitions,
                                            rko->rko_u.offset_commit.opaque);
                break;

        case RD_KAFKA_OP_FETCH_STOP | RD_KAFKA_OP_REPLY:
                /* Reply from toppar FETCH_STOP */
                rd_kafka_assignment_partition_stopped(rk, rko->rko_rktp);
                break;

        case RD_KAFKA_OP_CONSUMER_ERR:
                /* rd_kafka_consumer_poll() (_Q_CB_CONSUMER):
                 *   Consumer errors are returned to the application
                 *   as rkmessages, not error callbacks.
                 *
                 * rd_kafka_poll() (_Q_CB_GLOBAL):
                 *   convert to ERR op (fallthru)
                 */
                if (cb_type == RD_KAFKA_Q_CB_RETURN ||
                    cb_type == RD_KAFKA_Q_CB_FORCE_RETURN) {
                        /* return as message_t to application */
                        return RD_KAFKA_OP_RES_PASS;
                }
                /* FALLTHRU */

        case RD_KAFKA_OP_ERR:
                if (rk->rk_conf.error_cb)
                        rk->rk_conf.error_cb(rk, rko->rko_err,
                                             rko->rko_u.err.errstr,
                                             rk->rk_conf.opaque);
                else
                        rd_kafka_log(rk, LOG_ERR, "ERROR", "%s: %s",
                                     rk->rk_name, rko->rko_u.err.errstr);
                break;

        case RD_KAFKA_OP_DR:
                /* Delivery report:
                 * call application DR callback for each message. */
                while ((rkm = TAILQ_FIRST(&rko->rko_u.dr.msgq.rkmq_msgs))) {
                        rd_kafka_message_t *rkmessage;

                        TAILQ_REMOVE(&rko->rko_u.dr.msgq.rkmq_msgs, rkm,
                                     rkm_link);

                        rkmessage = rd_kafka_message_get_from_rkm(rko, rkm);

                        if (likely(rk->rk_conf.dr_msg_cb != NULL)) {
                                rk->rk_conf.dr_msg_cb(rk, rkmessage,
                                                      rk->rk_conf.opaque);

                        } else if (rk->rk_conf.dr_cb) {
                                rk->rk_conf.dr_cb(
                                    rk, rkmessage->payload, rkmessage->len,
                                    rkmessage->err, rk->rk_conf.opaque,
                                    rkmessage->_private);
                        } else if (rk->rk_drmode == RD_KAFKA_DR_MODE_EVENT) {
                                rd_kafka_log(
                                    rk, LOG_WARNING, "DRDROP",
                                    "Dropped delivery report for "
                                    "message to "
                                    "%s [%" PRId32
                                    "] (%s) with "
                                    "opaque %p: flush() or poll() "
                                    "should not be called when "
                                    "EVENT_DR is enabled",
                                    rd_kafka_topic_name(rkmessage->rkt),
                                    rkmessage->partition,
                                    rd_kafka_err2name(rkmessage->err),
                                    rkmessage->_private);
                        } else {
                                rd_assert(!*"BUG: neither a delivery report "
                                          "callback or EVENT_DR flag set");
                        }

                        rd_kafka_msg_destroy(rk, rkm);

                        if (unlikely(rd_kafka_yield_thread)) {
                                /* Callback called yield(),
                                 * re-enqueue the op (if there are any
                                 * remaining messages). */
                                if (!TAILQ_EMPTY(&rko->rko_u.dr.msgq.rkmq_msgs))
                                        rd_kafka_q_reenq(rkq, rko);
                                else
                                        rd_kafka_op_destroy(rko);
                                return RD_KAFKA_OP_RES_YIELD;
                        }
                }

                rd_kafka_msgq_init(&rko->rko_u.dr.msgq);

                break;

        case RD_KAFKA_OP_THROTTLE:
                if (rk->rk_conf.throttle_cb)
                        rk->rk_conf.throttle_cb(
                            rk, rko->rko_u.throttle.nodename,
                            rko->rko_u.throttle.nodeid,
                            rko->rko_u.throttle.throttle_time,
                            rk->rk_conf.opaque);
                break;

        case RD_KAFKA_OP_STATS:
                /* Statistics */
                if (rk->rk_conf.stats_cb &&
                    rk->rk_conf.stats_cb(rk, rko->rko_u.stats.json,
                                         rko->rko_u.stats.json_len,
                                         rk->rk_conf.opaque) == 1)
                        rko->rko_u.stats.json =
                            NULL; /* Application wanted json ptr */
                break;

        case RD_KAFKA_OP_LOG:
                if (likely(rk->rk_conf.log_cb &&
                           rk->rk_conf.log_level >= rko->rko_u.log.level))
                        rk->rk_conf.log_cb(rk, rko->rko_u.log.level,
                                           rko->rko_u.log.fac,
                                           rko->rko_u.log.str);
                break;

        case RD_KAFKA_OP_TERMINATE:
                /* nop: just a wake-up */
                res = RD_KAFKA_OP_RES_YIELD;
                rd_kafka_op_destroy(rko);
                break;

        case RD_KAFKA_OP_CREATETOPICS:
        case RD_KAFKA_OP_DELETETOPICS:
        case RD_KAFKA_OP_CREATEPARTITIONS:
        case RD_KAFKA_OP_ALTERCONFIGS:
        case RD_KAFKA_OP_INCREMENTALALTERCONFIGS:
        case RD_KAFKA_OP_DESCRIBECONFIGS:
        case RD_KAFKA_OP_DELETERECORDS:
        case RD_KAFKA_OP_DELETEGROUPS:
        case RD_KAFKA_OP_ADMIN_FANOUT:
        case RD_KAFKA_OP_CREATEACLS:
        case RD_KAFKA_OP_DESCRIBEACLS:
        case RD_KAFKA_OP_DELETEACLS:
        case RD_KAFKA_OP_LISTOFFSETS:
                /* Calls op_destroy() from worker callback,
                 * when the time comes. */
                res = rd_kafka_op_call(rk, rkq, rko);
                break;

        case RD_KAFKA_OP_ADMIN_RESULT:
                if (cb_type == RD_KAFKA_Q_CB_RETURN ||
                    cb_type == RD_KAFKA_Q_CB_FORCE_RETURN)
                        return RD_KAFKA_OP_RES_PASS; /* Don't handle here */

                /* Op is silently destroyed below */
                break;

        case RD_KAFKA_OP_TXN:
                /* Must only be handled by rdkafka main thread */
                rd_assert(thrd_is_current(rk->rk_thread));
                res = rd_kafka_op_call(rk, rkq, rko);
                break;

        case RD_KAFKA_OP_BARRIER:
                break;

        case RD_KAFKA_OP_PURGE:
                rd_kafka_purge(rk, rko->rko_u.purge.flags);
                break;

        case RD_KAFKA_OP_SET_TELEMETRY_BROKER:
                rd_kafka_set_telemetry_broker_maybe(
                    rk, rko->rko_u.telemetry_broker.rkb);
                break;

        case RD_KAFKA_OP_TERMINATE_TELEMETRY:
                rd_kafka_telemetry_schedule_termination(rko->rko_rk);
                break;

        case RD_KAFKA_OP_METADATA_UPDATE:
                res = rd_kafka_metadata_update_op(rk, rko->rko_u.metadata.mdi);
                break;

        default:
                /* If op has a callback set (e.g., OAUTHBEARER_REFRESH),
                 * call it. */
                if (rko->rko_type & RD_KAFKA_OP_CB) {
                        res = rd_kafka_op_call(rk, rkq, rko);
                        break;
                }

                RD_BUG("Can't handle op type %s (0x%x)",
                       rd_kafka_op2str(rko->rko_type), rko->rko_type);
                break;
        }

        if (res == RD_KAFKA_OP_RES_HANDLED)
                rd_kafka_op_destroy(rko);

        return res;
}

int rd_kafka_poll(rd_kafka_t *rk, int timeout_ms) {
        int r;

        r = rd_kafka_q_serve(rk->rk_rep, timeout_ms, 0, RD_KAFKA_Q_CB_CALLBACK,
                             rd_kafka_poll_cb, NULL);
        return r;
}


rd_kafka_event_t *rd_kafka_queue_poll(rd_kafka_queue_t *rkqu, int timeout_ms) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_q_pop_serve(rkqu->rkqu_q, rd_timeout_us(timeout_ms), 0,
                                   RD_KAFKA_Q_CB_EVENT, rd_kafka_poll_cb, NULL);


        if (!rko)
                return NULL;

        return rko;
}

int rd_kafka_queue_poll_callback(rd_kafka_queue_t *rkqu, int timeout_ms) {
        int r;

        r = rd_kafka_q_serve(rkqu->rkqu_q, timeout_ms, 0,
                             RD_KAFKA_Q_CB_CALLBACK, rd_kafka_poll_cb, NULL);
        return r;
}



static void
rd_kafka_toppar_dump(FILE *fp, const char *indent, rd_kafka_toppar_t *rktp) {

        fprintf(fp,
                "%s%.*s [%" PRId32
                "] broker %s, "
                "leader_id %s\n",
                indent, RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                rktp->rktp_partition,
                rktp->rktp_broker ? rktp->rktp_broker->rkb_name : "none",
                rktp->rktp_leader ? rktp->rktp_leader->rkb_name : "none");
        fprintf(fp,
                "%s refcnt %i\n"
                "%s msgq:      %i messages\n"
                "%s xmit_msgq: %i messages\n"
                "%s total:     %" PRIu64 " messages, %" PRIu64 " bytes\n",
                indent, rd_refcnt_get(&rktp->rktp_refcnt), indent,
                rktp->rktp_msgq.rkmq_msg_cnt, indent,
                rktp->rktp_xmit_msgq.rkmq_msg_cnt, indent,
                rd_atomic64_get(&rktp->rktp_c.tx_msgs),
                rd_atomic64_get(&rktp->rktp_c.tx_msg_bytes));
}

static void rd_kafka_broker_dump(FILE *fp, rd_kafka_broker_t *rkb, int locks) {
        rd_kafka_toppar_t *rktp;

        if (locks)
                rd_kafka_broker_lock(rkb);
        fprintf(fp,
                " rd_kafka_broker_t %p: %s NodeId %" PRId32
                " in state %s (for %.3fs)\n",
                rkb, rkb->rkb_name, rkb->rkb_nodeid,
                rd_kafka_broker_state_names[rkb->rkb_state],
                rkb->rkb_ts_state
                    ? (float)(rd_clock() - rkb->rkb_ts_state) / 1000000.0f
                    : 0.0f);
        fprintf(fp, "  refcnt %i\n", rd_refcnt_get(&rkb->rkb_refcnt));
        fprintf(fp, "  outbuf_cnt: %i waitresp_cnt: %i\n",
                rd_atomic32_get(&rkb->rkb_outbufs.rkbq_cnt),
                rd_atomic32_get(&rkb->rkb_waitresps.rkbq_cnt));
        fprintf(fp,
                "  %" PRIu64 " messages sent, %" PRIu64
                " bytes, "
                "%" PRIu64 " errors, %" PRIu64
                " timeouts\n"
                "  %" PRIu64 " messages received, %" PRIu64
                " bytes, "
                "%" PRIu64
                " errors\n"
                "  %" PRIu64 " messageset transmissions were retried\n",
                rd_atomic64_get(&rkb->rkb_c.tx),
                rd_atomic64_get(&rkb->rkb_c.tx_bytes),
                rd_atomic64_get(&rkb->rkb_c.tx_err),
                rd_atomic64_get(&rkb->rkb_c.req_timeouts),
                rd_atomic64_get(&rkb->rkb_c.rx),
                rd_atomic64_get(&rkb->rkb_c.rx_bytes),
                rd_atomic64_get(&rkb->rkb_c.rx_err),
                rd_atomic64_get(&rkb->rkb_c.tx_retries));

        fprintf(fp, "  %i toppars:\n", rkb->rkb_toppar_cnt);
        TAILQ_FOREACH(rktp, &rkb->rkb_toppars, rktp_rkblink)
        rd_kafka_toppar_dump(fp, "   ", rktp);
        if (locks) {
                rd_kafka_broker_unlock(rkb);
        }
}


static void rd_kafka_dump0(FILE *fp, rd_kafka_t *rk, int locks) {
        rd_kafka_broker_t *rkb;
        rd_kafka_topic_t *rkt;
        rd_kafka_toppar_t *rktp;
        int i;
        unsigned int tot_cnt;
        size_t tot_size;

        rd_kafka_curr_msgs_get(rk, &tot_cnt, &tot_size);

        if (locks)
                rd_kafka_rdlock(rk);
#if ENABLE_DEVEL
        fprintf(fp, "rd_kafka_op_cnt: %d\n", rd_atomic32_get(&rd_kafka_op_cnt));
#endif
        fprintf(fp, "rd_kafka_t %p: %s\n", rk, rk->rk_name);

        fprintf(fp, " producer.msg_cnt %u (%" PRIusz " bytes)\n", tot_cnt,
                tot_size);
        fprintf(fp, " rk_rep reply queue: %i ops\n",
                rd_kafka_q_len(rk->rk_rep));

        fprintf(fp, " brokers:\n");
        if (locks)
                mtx_lock(&rk->rk_internal_rkb_lock);
        if (rk->rk_internal_rkb)
                rd_kafka_broker_dump(fp, rk->rk_internal_rkb, locks);
        if (locks)
                mtx_unlock(&rk->rk_internal_rkb_lock);

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_kafka_broker_dump(fp, rkb, locks);
        }

        fprintf(fp, " cgrp:\n");
        if (rk->rk_cgrp) {
                rd_kafka_cgrp_t *rkcg = rk->rk_cgrp;
                fprintf(fp, "  %.*s in state %s, flags 0x%x\n",
                        RD_KAFKAP_STR_PR(rkcg->rkcg_group_id),
                        rd_kafka_cgrp_state_names[rkcg->rkcg_state],
                        rkcg->rkcg_flags);
                fprintf(fp, "   coord_id %" PRId32 ", broker %s\n",
                        rkcg->rkcg_coord_id,
                        rkcg->rkcg_curr_coord
                            ? rd_kafka_broker_name(rkcg->rkcg_curr_coord)
                            : "(none)");

                fprintf(fp, "  toppars:\n");
                RD_LIST_FOREACH(rktp, &rkcg->rkcg_toppars, i) {
                        fprintf(fp, "   %.*s [%" PRId32 "] in state %s\n",
                                RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                                rktp->rktp_partition,
                                rd_kafka_fetch_states[rktp->rktp_fetch_state]);
                }
        }

        fprintf(fp, " topics:\n");
        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                fprintf(fp,
                        "  %.*s with %" PRId32
                        " partitions, state %s, "
                        "refcnt %i\n",
                        RD_KAFKAP_STR_PR(rkt->rkt_topic),
                        rkt->rkt_partition_cnt,
                        rd_kafka_topic_state_names[rkt->rkt_state],
                        rd_refcnt_get(&rkt->rkt_refcnt));
                if (rkt->rkt_ua)
                        rd_kafka_toppar_dump(fp, "   ", rkt->rkt_ua);
                if (rd_list_empty(&rkt->rkt_desp)) {
                        fprintf(fp, "   desired partitions:");
                        RD_LIST_FOREACH(rktp, &rkt->rkt_desp, i)
                        fprintf(fp, " %" PRId32, rktp->rktp_partition);
                        fprintf(fp, "\n");
                }
        }

        fprintf(fp, "\n");
        rd_kafka_metadata_cache_dump(fp, rk);

        if (locks)
                rd_kafka_rdunlock(rk);
}

void rd_kafka_dump(FILE *fp, rd_kafka_t *rk) {
        if (rk)
                rd_kafka_dump0(fp, rk, 1 /*locks*/);
}



const char *rd_kafka_name(const rd_kafka_t *rk) {
        return rk->rk_name;
}

rd_kafka_type_t rd_kafka_type(const rd_kafka_t *rk) {
        return rk->rk_type;
}


char *rd_kafka_memberid(const rd_kafka_t *rk) {
        rd_kafka_op_t *rko;
        rd_kafka_cgrp_t *rkcg;
        char *memberid;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return NULL;

        rko = rd_kafka_op_req2(rkcg->rkcg_ops, RD_KAFKA_OP_NAME);
        if (!rko)
                return NULL;
        memberid            = rko->rko_u.name.str;
        rko->rko_u.name.str = NULL;
        rd_kafka_op_destroy(rko);

        return memberid;
}


char *rd_kafka_clusterid(rd_kafka_t *rk, int timeout_ms) {
        rd_ts_t abs_timeout = rd_timeout_init(timeout_ms);

        /* ClusterId is returned in Metadata >=V2 responses and
         * cached on the rk. If no cached value is available
         * it means no metadata has been received yet, or we're
         * using a lower protocol version
         * (e.g., lack of api.version.request=true). */

        while (1) {
                int remains_ms;

                rd_kafka_rdlock(rk);

                if (rk->rk_clusterid) {
                        /* Cached clusterid available. */
                        char *ret = rd_strdup(rk->rk_clusterid);
                        rd_kafka_rdunlock(rk);
                        return ret;
                } else if (rk->rk_ts_metadata > 0) {
                        /* Metadata received but no clusterid,
                         * this probably means the broker is too old
                         * or api.version.request=false. */
                        rd_kafka_rdunlock(rk);
                        return NULL;
                }

                rd_kafka_rdunlock(rk);

                /* Wait for up to timeout_ms for a metadata refresh,
                 * if permitted by application. */
                remains_ms = rd_timeout_remains(abs_timeout);
                if (rd_timeout_expired(remains_ms))
                        return NULL;

                rd_kafka_metadata_cache_wait_change(rk, remains_ms);
        }

        return NULL;
}


int32_t rd_kafka_controllerid(rd_kafka_t *rk, int timeout_ms) {
        rd_ts_t abs_timeout = rd_timeout_init(timeout_ms);

        /* ControllerId is returned in Metadata >=V1 responses and
         * cached on the rk. If no cached value is available
         * it means no metadata has been received yet, or we're
         * using a lower protocol version
         * (e.g., lack of api.version.request=true). */

        while (1) {
                int remains_ms;
                int version;

                version = rd_kafka_brokers_get_state_version(rk);

                rd_kafka_rdlock(rk);

                if (rk->rk_controllerid != -1) {
                        /* Cached controllerid available. */
                        rd_kafka_rdunlock(rk);
                        return rk->rk_controllerid;
                } else if (rk->rk_ts_metadata > 0) {
                        /* Metadata received but no clusterid,
                         * this probably means the broker is too old
                         * or api.version.request=false. */
                        rd_kafka_rdunlock(rk);
                        return -1;
                }

                rd_kafka_rdunlock(rk);

                /* Wait for up to timeout_ms for a metadata refresh,
                 * if permitted by application. */
                remains_ms = rd_timeout_remains(abs_timeout);
                if (rd_timeout_expired(remains_ms))
                        return -1;

                rd_kafka_brokers_wait_state_change(rk, version, remains_ms);
        }

        return -1;
}


void *rd_kafka_opaque(const rd_kafka_t *rk) {
        return rk->rk_conf.opaque;
}


int rd_kafka_outq_len(rd_kafka_t *rk) {
        return rd_kafka_curr_msgs_cnt(rk) + rd_kafka_q_len(rk->rk_rep) +
               (rk->rk_background.q ? rd_kafka_q_len(rk->rk_background.q) : 0);
}


rd_kafka_resp_err_t rd_kafka_flush(rd_kafka_t *rk, int timeout_ms) {
        unsigned int msg_cnt = 0;

        if (rk->rk_type != RD_KAFKA_PRODUCER)
                return RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;

        rd_kafka_yield_thread = 0;

        /* Set flushing flag on the producer for the duration of the
         * flush() call. This tells producer_serve() that the linger.ms
         * time should be considered immediate. */
        rd_atomic32_add(&rk->rk_flushing, 1);

        /* Wake up all broker threads to trigger the produce_serve() call.
         * If this flush() call finishes before the broker wakes up
         * then no flushing will be performed by that broker thread. */
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_UP, "flushing");

        if (rk->rk_drmode == RD_KAFKA_DR_MODE_EVENT) {
                /* Application wants delivery reports as events rather
                 * than callbacks, we must thus not serve this queue
                 * with rd_kafka_poll() since that would trigger non-existent
                 * delivery report callbacks, which would result
                 * in the delivery reports being dropped.
                 * Instead we rely on the application to serve the event
                 * queue in another thread, so all we do here is wait
                 * for the current message count to reach zero. */
                rd_kafka_curr_msgs_wait_zero(rk, timeout_ms, &msg_cnt);

        } else {
                /* Standard poll interface.
                 *
                 * First poll call is non-blocking for the case
                 * where timeout_ms==RD_POLL_NOWAIT to make sure poll is
                 * called at least once. */
                rd_ts_t ts_end = rd_timeout_init(timeout_ms);
                int tmout      = RD_POLL_NOWAIT;
                int qlen       = 0;

                do {
                        rd_kafka_poll(rk, tmout);
                        qlen    = rd_kafka_q_len(rk->rk_rep);
                        msg_cnt = rd_kafka_curr_msgs_cnt(rk);
                } while (qlen + msg_cnt > 0 && !rd_kafka_yield_thread &&
                         (tmout = rd_timeout_remains_limit(ts_end, 10)) !=
                             RD_POLL_NOWAIT);

                msg_cnt += qlen;
        }

        rd_atomic32_sub(&rk->rk_flushing, 1);

        return msg_cnt > 0 ? RD_KAFKA_RESP_ERR__TIMED_OUT
                           : RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Purge the partition message queue (according to \p purge_flags) for
 *        all toppars.
 *
 * This is a necessity to avoid the race condition when a purge() is scheduled
 * shortly in-between an rktp has been created but before it has been
 * joined to a broker handler thread.
 *
 * The rktp_xmit_msgq is handled by the broker-thread purge.
 *
 * @returns the number of messages purged.
 *
 * @locks_required rd_kafka_*lock()
 * @locks_acquired rd_kafka_topic_rdlock()
 */
static int rd_kafka_purge_toppars(rd_kafka_t *rk, int purge_flags) {
        rd_kafka_topic_t *rkt;
        int cnt = 0;

        TAILQ_FOREACH(rkt, &rk->rk_topics, rkt_link) {
                rd_kafka_toppar_t *rktp;
                int i;

                rd_kafka_topic_rdlock(rkt);
                for (i = 0; i < rkt->rkt_partition_cnt; i++)
                        cnt += rd_kafka_toppar_purge_queues(
                            rkt->rkt_p[i], purge_flags, rd_false /*!xmit*/);

                RD_LIST_FOREACH(rktp, &rkt->rkt_desp, i)
                cnt += rd_kafka_toppar_purge_queues(rktp, purge_flags,
                                                    rd_false /*!xmit*/);

                if (rkt->rkt_ua)
                        cnt += rd_kafka_toppar_purge_queues(
                            rkt->rkt_ua, purge_flags, rd_false /*!xmit*/);
                rd_kafka_topic_rdunlock(rkt);
        }

        return cnt;
}


rd_kafka_resp_err_t rd_kafka_purge(rd_kafka_t *rk, int purge_flags) {
        rd_kafka_broker_t *rkb;
        rd_kafka_q_t *tmpq = NULL;
        int waitcnt        = 0;

        if (rk->rk_type != RD_KAFKA_PRODUCER)
                return RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED;

        /* Check that future flags are not passed */
        if ((purge_flags & ~RD_KAFKA_PURGE_F_MASK) != 0)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        /* Nothing to purge */
        if (!purge_flags)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        /* Set up a reply queue to wait for broker thread signalling
         * completion, unless non-blocking. */
        if (!(purge_flags & RD_KAFKA_PURGE_F_NON_BLOCKING))
                tmpq = rd_kafka_q_new(rk);

        rd_kafka_rdlock(rk);

        /* Purge msgq for all toppars. */
        rd_kafka_purge_toppars(rk, purge_flags);

        /* Send purge request to all broker threads */
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_kafka_broker_purge_queues(rkb, purge_flags,
                                             RD_KAFKA_REPLYQ(tmpq, 0));
                waitcnt++;
        }

        rd_kafka_rdunlock(rk);


        if (tmpq) {
                /* Wait for responses */
                while (waitcnt-- > 0)
                        rd_kafka_q_wait_result(tmpq, RD_POLL_INFINITE);

                rd_kafka_q_destroy_owner(tmpq);
        }

        /* Purge messages for the UA(-1) partitions (which are not
         * handled by a broker thread) */
        if (purge_flags & RD_KAFKA_PURGE_F_QUEUE)
                rd_kafka_purge_ua_toppar_queues(rk);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * @returns a csv string of purge flags in thread-local storage
 */
const char *rd_kafka_purge_flags2str(int flags) {
        static const char *names[] = {"queue", "inflight", "non-blocking",
                                      NULL};
        static RD_TLS char ret[64];

        return rd_flags2str(ret, sizeof(ret), names, flags);
}


int rd_kafka_version(void) {
        return RD_KAFKA_VERSION;
}

const char *rd_kafka_version_str(void) {
        static RD_TLS char ret[128];
        size_t of = 0, r;

        if (*ret)
                return ret;

#ifdef LIBRDKAFKA_GIT_VERSION
        if (*LIBRDKAFKA_GIT_VERSION) {
                of = rd_snprintf(ret, sizeof(ret), "%s",
                                 *LIBRDKAFKA_GIT_VERSION == 'v'
                                     ? &LIBRDKAFKA_GIT_VERSION[1]
                                     : LIBRDKAFKA_GIT_VERSION);
                if (of > sizeof(ret))
                        of = sizeof(ret);
        }
#endif

#define _my_sprintf(...)                                                       \
        do {                                                                   \
                r = rd_snprintf(ret + of, sizeof(ret) - of, __VA_ARGS__);      \
                if (r > sizeof(ret) - of)                                      \
                        r = sizeof(ret) - of;                                  \
                of += r;                                                       \
        } while (0)

        if (of == 0) {
                int ver  = rd_kafka_version();
                int prel = (ver & 0xff);
                _my_sprintf("%i.%i.%i", (ver >> 24) & 0xff, (ver >> 16) & 0xff,
                            (ver >> 8) & 0xff);
                if (prel != 0xff) {
                        /* pre-builds below 200 are just running numbers,
                         * above 200 are RC numbers. */
                        if (prel <= 200)
                                _my_sprintf("-pre%d", prel);
                        else
                                _my_sprintf("-RC%d", prel - 200);
                }
        }

#if ENABLE_DEVEL
        _my_sprintf("-devel");
#endif

#if WITHOUT_OPTIMIZATION
        _my_sprintf("-O0");
#endif

        return ret;
}


/**
 * Assert trampoline to print some debugging information on crash.
 */
void RD_NORETURN rd_kafka_crash(const char *file,
                                int line,
                                const char *function,
                                rd_kafka_t *rk,
                                const char *reason) {
        fprintf(stderr, "*** %s:%i:%s: %s ***\n", file, line, function, reason);
        if (rk)
                rd_kafka_dump0(stderr, rk, 0 /*no locks*/);
        abort();
}



struct list_groups_state {
        rd_kafka_q_t *q;
        rd_kafka_resp_err_t err;
        int wait_cnt;
        const char *desired_group;
        struct rd_kafka_group_list *grplist;
        int grplist_size;
};

static const char *rd_kafka_consumer_group_state_names[] = {
    "Unknown", "PreparingRebalance", "CompletingRebalance", "Stable", "Dead",
    "Empty"};

const char *
rd_kafka_consumer_group_state_name(rd_kafka_consumer_group_state_t state) {
        if (state < 0 || state >= RD_KAFKA_CONSUMER_GROUP_STATE__CNT)
                return NULL;
        return rd_kafka_consumer_group_state_names[state];
}

rd_kafka_consumer_group_state_t
rd_kafka_consumer_group_state_code(const char *name) {
        size_t i;
        for (i = 0; i < RD_KAFKA_CONSUMER_GROUP_STATE__CNT; i++) {
                if (!rd_strcasecmp(rd_kafka_consumer_group_state_names[i],
                                   name))
                        return i;
        }
        return RD_KAFKA_CONSUMER_GROUP_STATE_UNKNOWN;
}

static const char *rd_kafka_consumer_group_type_names[] = {
    "Unknown", "Consumer", "Classic"};

const char *
rd_kafka_consumer_group_type_name(rd_kafka_consumer_group_type_t type) {
        if (type < 0 || type >= RD_KAFKA_CONSUMER_GROUP_TYPE__CNT)
                return NULL;
        return rd_kafka_consumer_group_type_names[type];
}

rd_kafka_consumer_group_type_t
rd_kafka_consumer_group_type_code(const char *name) {
        size_t i;
        for (i = 0; i < RD_KAFKA_CONSUMER_GROUP_TYPE__CNT; i++) {
                if (!rd_strcasecmp(rd_kafka_consumer_group_type_names[i], name))
                        return i;
        }
        return RD_KAFKA_CONSUMER_GROUP_TYPE_UNKNOWN;
}

static void rd_kafka_DescribeGroups_resp_cb(rd_kafka_t *rk,
                                            rd_kafka_broker_t *rkb,
                                            rd_kafka_resp_err_t err,
                                            rd_kafka_buf_t *reply,
                                            rd_kafka_buf_t *request,
                                            void *opaque) {
        struct list_groups_state *state;
        const int log_decode_errors = LOG_ERR;
        int cnt;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* 'state' has gone out of scope due to list_groups()
                 * timing out and returning. */
                return;
        }

        state = opaque;
        state->wait_cnt--;

        if (err)
                goto err;

        rd_kafka_buf_read_i32(reply, &cnt);

        while (cnt-- > 0) {
                int16_t ErrorCode;
                rd_kafkap_str_t Group, GroupState, ProtoType, Proto;
                int MemberCnt;
                struct rd_kafka_group_info *gi;

                if (state->grplist->group_cnt == state->grplist_size) {
                        /* Grow group array */
                        state->grplist_size *= 2;
                        state->grplist->groups =
                            rd_realloc(state->grplist->groups,
                                       state->grplist_size *
                                           sizeof(*state->grplist->groups));
                }

                gi = &state->grplist->groups[state->grplist->group_cnt++];
                memset(gi, 0, sizeof(*gi));

                rd_kafka_buf_read_i16(reply, &ErrorCode);
                rd_kafka_buf_read_str(reply, &Group);
                rd_kafka_buf_read_str(reply, &GroupState);
                rd_kafka_buf_read_str(reply, &ProtoType);
                rd_kafka_buf_read_str(reply, &Proto);
                rd_kafka_buf_read_i32(reply, &MemberCnt);

                if (MemberCnt > 100000) {
                        err = RD_KAFKA_RESP_ERR__BAD_MSG;
                        goto err;
                }

                rd_kafka_broker_lock(rkb);
                gi->broker.id   = rkb->rkb_nodeid;
                gi->broker.host = rd_strdup(rkb->rkb_origname);
                gi->broker.port = rkb->rkb_port;
                rd_kafka_broker_unlock(rkb);

                gi->err           = ErrorCode;
                gi->group         = RD_KAFKAP_STR_DUP(&Group);
                gi->state         = RD_KAFKAP_STR_DUP(&GroupState);
                gi->protocol_type = RD_KAFKAP_STR_DUP(&ProtoType);
                gi->protocol      = RD_KAFKAP_STR_DUP(&Proto);

                if (MemberCnt > 0)
                        gi->members =
                            rd_malloc(MemberCnt * sizeof(*gi->members));

                while (MemberCnt-- > 0) {
                        rd_kafkap_str_t MemberId, ClientId, ClientHost;
                        rd_kafkap_bytes_t Meta, Assignment;
                        struct rd_kafka_group_member_info *mi;

                        mi = &gi->members[gi->member_cnt++];
                        memset(mi, 0, sizeof(*mi));

                        rd_kafka_buf_read_str(reply, &MemberId);
                        rd_kafka_buf_read_str(reply, &ClientId);
                        rd_kafka_buf_read_str(reply, &ClientHost);
                        rd_kafka_buf_read_kbytes(reply, &Meta);
                        rd_kafka_buf_read_kbytes(reply, &Assignment);

                        mi->member_id   = RD_KAFKAP_STR_DUP(&MemberId);
                        mi->client_id   = RD_KAFKAP_STR_DUP(&ClientId);
                        mi->client_host = RD_KAFKAP_STR_DUP(&ClientHost);

                        if (RD_KAFKAP_BYTES_LEN(&Meta) == 0) {
                                mi->member_metadata_size = 0;
                                mi->member_metadata      = NULL;
                        } else {
                                mi->member_metadata_size =
                                    RD_KAFKAP_BYTES_LEN(&Meta);
                                mi->member_metadata = rd_memdup(
                                    Meta.data, mi->member_metadata_size);
                        }

                        if (RD_KAFKAP_BYTES_LEN(&Assignment) == 0) {
                                mi->member_assignment_size = 0;
                                mi->member_assignment      = NULL;
                        } else {
                                mi->member_assignment_size =
                                    RD_KAFKAP_BYTES_LEN(&Assignment);
                                mi->member_assignment =
                                    rd_memdup(Assignment.data,
                                              mi->member_assignment_size);
                        }
                }
        }

err:
        state->err = err;
        return;

err_parse:
        state->err = reply->rkbuf_err;
}

static void rd_kafka_ListGroups_resp_cb(rd_kafka_t *rk,
                                        rd_kafka_broker_t *rkb,
                                        rd_kafka_resp_err_t err,
                                        rd_kafka_buf_t *reply,
                                        rd_kafka_buf_t *request,
                                        void *opaque) {
        struct list_groups_state *state;
        const int log_decode_errors = LOG_ERR;
        int16_t ErrorCode;
        char **grps = NULL;
        int cnt, grpcnt, i = 0;

        if (err == RD_KAFKA_RESP_ERR__DESTROY) {
                /* 'state' is no longer in scope because
                 * list_groups() timed out and returned to the caller.
                 * We must not touch anything here but simply return. */
                return;
        }

        state = opaque;

        state->wait_cnt--;

        if (err)
                goto err;

        rd_kafka_buf_read_i16(reply, &ErrorCode);
        if (ErrorCode) {
                err = ErrorCode;
                goto err;
        }

        rd_kafka_buf_read_i32(reply, &cnt);

        if (state->desired_group)
                grpcnt = 1;
        else
                grpcnt = cnt;

        if (cnt == 0 || grpcnt == 0)
                return;

        grps = rd_malloc(sizeof(*grps) * grpcnt);

        while (cnt-- > 0) {
                rd_kafkap_str_t grp, proto;

                rd_kafka_buf_read_str(reply, &grp);
                rd_kafka_buf_read_str(reply, &proto);

                if (state->desired_group &&
                    rd_kafkap_str_cmp_str(&grp, state->desired_group))
                        continue;

                grps[i++] = RD_KAFKAP_STR_DUP(&grp);

                if (i == grpcnt)
                        break;
        }

        if (i > 0) {
                rd_kafka_error_t *error;

                state->wait_cnt++;
                error = rd_kafka_DescribeGroupsRequest(
                    rkb, 0, grps, i,
                    rd_false /* don't include authorized operations */,
                    RD_KAFKA_REPLYQ(state->q, 0),
                    rd_kafka_DescribeGroups_resp_cb, state);
                if (error) {
                        rd_kafka_DescribeGroups_resp_cb(
                            rk, rkb, rd_kafka_error_code(error), reply, request,
                            opaque);
                        rd_kafka_error_destroy(error);
                }

                while (i-- > 0)
                        rd_free(grps[i]);
        }


        rd_free(grps);

err:
        state->err = err;
        return;

err_parse:
        if (grps)
                rd_free(grps);
        state->err = reply->rkbuf_err;
}

rd_kafka_resp_err_t
rd_kafka_list_groups(rd_kafka_t *rk,
                     const char *group,
                     const struct rd_kafka_group_list **grplistp,
                     int timeout_ms) {
        rd_kafka_broker_t *rkb;
        int rkb_cnt                    = 0;
        struct list_groups_state state = RD_ZERO_INIT;
        rd_ts_t ts_end                 = rd_timeout_init(timeout_ms);

        /* Wait until metadata has been fetched from cluster so
         * that we have a full broker list.
         * This state only happens during initial client setup, after that
         * there'll always be a cached metadata copy. */
        while (1) {
                int state_version = rd_kafka_brokers_get_state_version(rk);
                rd_bool_t has_metadata;

                rd_kafka_rdlock(rk);
                has_metadata = rk->rk_ts_metadata != 0;
                rd_kafka_rdunlock(rk);

                if (has_metadata)
                        break;

                if (!rd_kafka_brokers_wait_state_change(
                        rk, state_version, rd_timeout_remains(ts_end)))
                        return RD_KAFKA_RESP_ERR__TIMED_OUT;
        }


        state.q             = rd_kafka_q_new(rk);
        state.desired_group = group;
        state.grplist       = rd_calloc(1, sizeof(*state.grplist));
        state.grplist_size  = group ? 1 : 32;

        state.grplist->groups =
            rd_malloc(state.grplist_size * sizeof(*state.grplist->groups));

        /* Query each broker for its list of groups */
        rd_kafka_rdlock(rk);
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rd_kafka_error_t *error;
                rd_kafka_broker_lock(rkb);
                if (rkb->rkb_nodeid == -1 || RD_KAFKA_BROKER_IS_LOGICAL(rkb)) {
                        rd_kafka_broker_unlock(rkb);
                        continue;
                }
                rd_kafka_broker_unlock(rkb);

                state.wait_cnt++;
                rkb_cnt++;
                error = rd_kafka_ListGroupsRequest(
                    rkb, 0, NULL, 0, NULL, 0, RD_KAFKA_REPLYQ(state.q, 0),
                    rd_kafka_ListGroups_resp_cb, &state);
                if (error) {
                        rd_kafka_ListGroups_resp_cb(rk, rkb,
                                                    rd_kafka_error_code(error),
                                                    NULL, NULL, &state);
                        rd_kafka_error_destroy(error);
                }
        }
        rd_kafka_rdunlock(rk);

        if (rkb_cnt == 0) {
                state.err = RD_KAFKA_RESP_ERR__TRANSPORT;

        } else {
                int remains;

                while (state.wait_cnt > 0 &&
                       !rd_timeout_expired(
                           (remains = rd_timeout_remains(ts_end)))) {
                        rd_kafka_q_serve(state.q, remains, 0,
                                         RD_KAFKA_Q_CB_CALLBACK,
                                         rd_kafka_poll_cb, NULL);
                        /* Ignore yields */
                }
        }

        rd_kafka_q_destroy_owner(state.q);

        if (state.wait_cnt > 0 && !state.err) {
                if (state.grplist->group_cnt == 0)
                        state.err = RD_KAFKA_RESP_ERR__TIMED_OUT;
                else {
                        *grplistp = state.grplist;
                        return RD_KAFKA_RESP_ERR__PARTIAL;
                }
        }

        if (state.err)
                rd_kafka_group_list_destroy(state.grplist);
        else
                *grplistp = state.grplist;

        return state.err;
}


void rd_kafka_group_list_destroy(const struct rd_kafka_group_list *grplist0) {
        struct rd_kafka_group_list *grplist =
            (struct rd_kafka_group_list *)grplist0;

        while (grplist->group_cnt-- > 0) {
                struct rd_kafka_group_info *gi;
                gi = &grplist->groups[grplist->group_cnt];

                if (gi->broker.host)
                        rd_free(gi->broker.host);
                if (gi->group)
                        rd_free(gi->group);
                if (gi->state)
                        rd_free(gi->state);
                if (gi->protocol_type)
                        rd_free(gi->protocol_type);
                if (gi->protocol)
                        rd_free(gi->protocol);

                while (gi->member_cnt-- > 0) {
                        struct rd_kafka_group_member_info *mi;
                        mi = &gi->members[gi->member_cnt];

                        if (mi->member_id)
                                rd_free(mi->member_id);
                        if (mi->client_id)
                                rd_free(mi->client_id);
                        if (mi->client_host)
                                rd_free(mi->client_host);
                        if (mi->member_metadata)
                                rd_free(mi->member_metadata);
                        if (mi->member_assignment)
                                rd_free(mi->member_assignment);
                }

                if (gi->members)
                        rd_free(gi->members);
        }

        if (grplist->groups)
                rd_free(grplist->groups);

        rd_free(grplist);
}



const char *rd_kafka_get_debug_contexts(void) {
        return RD_KAFKA_DEBUG_CONTEXTS;
}


int rd_kafka_path_is_dir(const char *path) {
#ifdef _WIN32
        struct _stat st;
        return (_stat(path, &st) == 0 && st.st_mode & S_IFDIR);
#else
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}


/**
 * @returns true if directory is empty or can't be accessed, else false.
 */
rd_bool_t rd_kafka_dir_is_empty(const char *path) {
#if _WIN32
        /* FIXME: Unsupported */
        return rd_true;
#else
        DIR *dir;
        struct dirent *d;
#if defined(__sun)
        struct stat st;
        int ret = 0;
#endif

        dir = opendir(path);
        if (!dir)
                return rd_true;

        while ((d = readdir(dir))) {

                if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
                        continue;

#if defined(__sun)
                ret = stat(d->d_name, &st);
                if (ret != 0) {
                        return rd_true;  // Can't be accessed
                }
                if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ||
                    S_ISLNK(st.st_mode)) {
#else
                if (d->d_type == DT_REG || d->d_type == DT_LNK ||
                    d->d_type == DT_DIR) {
#endif
                        closedir(dir);
                        return rd_false;
                }
        }

        closedir(dir);
        return rd_true;
#endif
}


void *rd_kafka_mem_malloc(rd_kafka_t *rk, size_t size) {
        return rd_malloc(size);
}

void *rd_kafka_mem_calloc(rd_kafka_t *rk, size_t num, size_t size) {
        return rd_calloc(num, size);
}

void rd_kafka_mem_free(rd_kafka_t *rk, void *ptr) {
        rd_free(ptr);
}


int rd_kafka_errno(void) {
        return errno;
}

int rd_kafka_unittest(void) {
        return rd_unittest();
}


/**
 * Creates a new UUID.
 *
 * @return A newly allocated UUID.
 */
rd_kafka_Uuid_t *rd_kafka_Uuid_new(int64_t most_significant_bits,
                                   int64_t least_significant_bits) {
        rd_kafka_Uuid_t *uuid        = rd_calloc(1, sizeof(rd_kafka_Uuid_t));
        uuid->most_significant_bits  = most_significant_bits;
        uuid->least_significant_bits = least_significant_bits;
        return uuid;
}

/**
 * Returns a newly allocated copy of the given UUID.
 *
 * @param uuid UUID to copy.
 * @return Copy of the provided UUID.
 *
 * @remark Dynamically allocated. Deallocate (free) after use.
 */
rd_kafka_Uuid_t *rd_kafka_Uuid_copy(const rd_kafka_Uuid_t *uuid) {
        rd_kafka_Uuid_t *copy_uuid = rd_kafka_Uuid_new(
            uuid->most_significant_bits, uuid->least_significant_bits);
        if (*uuid->base64str)
                memcpy(copy_uuid->base64str, uuid->base64str, 23);
        return copy_uuid;
}

/**
 * Returns a new non cryptographically secure UUIDv4 (random).
 *
 * @return A UUIDv4.
 *
 * @remark Must be freed after use using rd_kafka_Uuid_destroy().
 */
rd_kafka_Uuid_t rd_kafka_Uuid_random() {
        int i;
        unsigned char rand_values_bytes[16] = {0};
        uint64_t *rand_values_uint64        = (uint64_t *)rand_values_bytes;
        unsigned char *rand_values_app;
        rd_kafka_Uuid_t ret = RD_KAFKA_UUID_ZERO;
        for (i = 0; i < 16; i += 2) {
                uint16_t rand_uint16 = (uint16_t)rd_jitter(0, INT16_MAX - 1);
                /* No need to convert endianess here because it's still only
                 * a random value. */
                rand_values_app = (unsigned char *)&rand_uint16;
                rand_values_bytes[i] |= rand_values_app[0];
                rand_values_bytes[i + 1] |= rand_values_app[1];
        }

        rand_values_bytes[6] &= 0x0f; /* clear version */
        rand_values_bytes[6] |= 0x40; /* version 4 */
        rand_values_bytes[8] &= 0x3f; /* clear variant */
        rand_values_bytes[8] |= 0x80; /* IETF variant */

        ret.most_significant_bits  = be64toh(rand_values_uint64[0]);
        ret.least_significant_bits = be64toh(rand_values_uint64[1]);
        return ret;
}

/**
 * @brief Destroy the provided uuid.
 *
 * @param uuid UUID
 */
void rd_kafka_Uuid_destroy(rd_kafka_Uuid_t *uuid) {
        rd_free(uuid);
}

/**
 * @brief Computes canonical encoding for the given uuid string.
 *        Mainly useful for testing.
 *
 * @param uuid UUID for which canonical encoding is required.
 *
 * @return canonical encoded string for the given UUID.
 *
 * @remark  Must be freed after use.
 */
const char *rd_kafka_Uuid_str(const rd_kafka_Uuid_t *uuid) {
        int i, j;
        unsigned char bytes[16];
        char *ret = rd_calloc(37, sizeof(*ret));

        for (i = 0; i < 8; i++) {
#if __BYTE_ORDER == __LITTLE_ENDIAN
                j = 7 - i;
#elif __BYTE_ORDER == __BIG_ENDIAN
                j = i;
#endif
                bytes[i]     = (uuid->most_significant_bits >> (8 * j)) & 0xFF;
                bytes[8 + i] = (uuid->least_significant_bits >> (8 * j)) & 0xFF;
        }

        rd_snprintf(ret, 37,
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%"
                    "02x%02x%02x",
                    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                    bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                    bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
        return ret;
}

const char *rd_kafka_Uuid_base64str(const rd_kafka_Uuid_t *uuid) {
        if (*uuid->base64str)
                return uuid->base64str;

        rd_chariov_t in_base64;
        char *out_base64_str;
        char *uuid_bytes;
        uint64_t input_uuid[2];

        input_uuid[0]  = htobe64(uuid->most_significant_bits);
        input_uuid[1]  = htobe64(uuid->least_significant_bits);
        uuid_bytes     = (char *)input_uuid;
        in_base64.ptr  = uuid_bytes;
        in_base64.size = sizeof(uuid->most_significant_bits) +
                         sizeof(uuid->least_significant_bits);

        out_base64_str = rd_base64_encode_str(&in_base64);
        if (!out_base64_str)
                return NULL;

        rd_strlcpy((char *)uuid->base64str, out_base64_str,
                   23 /* Removing extra ('=') padding */);
        rd_free(out_base64_str);
        return uuid->base64str;
}

unsigned int rd_kafka_Uuid_hash(const rd_kafka_Uuid_t *uuid) {
        unsigned char bytes[16];
        memcpy(bytes, &uuid->most_significant_bits, 8);
        memcpy(&bytes[8], &uuid->least_significant_bits, 8);
        return rd_bytes_hash(bytes, 16);
}

unsigned int rd_kafka_Uuid_map_hash(const void *key) {
        return rd_kafka_Uuid_hash(key);
}

int64_t rd_kafka_Uuid_least_significant_bits(const rd_kafka_Uuid_t *uuid) {
        return uuid->least_significant_bits;
}


int64_t rd_kafka_Uuid_most_significant_bits(const rd_kafka_Uuid_t *uuid) {
        return uuid->most_significant_bits;
}
