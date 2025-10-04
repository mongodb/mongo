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

#include <stdarg.h>

#include "rdkafka_int.h"
#include "rdkafka_op.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_proto.h"
#include "rdkafka_offset.h"
#include "rdkafka_error.h"

/* Current number of rd_kafka_op_t */
rd_atomic32_t rd_kafka_op_cnt;


const char *rd_kafka_op2str(rd_kafka_op_type_t type) {
        int skiplen                                = 6;
        static const char *names[RD_KAFKA_OP__END] = {
            [RD_KAFKA_OP_NONE]             = "REPLY:NONE",
            [RD_KAFKA_OP_FETCH]            = "REPLY:FETCH",
            [RD_KAFKA_OP_ERR]              = "REPLY:ERR",
            [RD_KAFKA_OP_CONSUMER_ERR]     = "REPLY:CONSUMER_ERR",
            [RD_KAFKA_OP_DR]               = "REPLY:DR",
            [RD_KAFKA_OP_STATS]            = "REPLY:STATS",
            [RD_KAFKA_OP_OFFSET_COMMIT]    = "REPLY:OFFSET_COMMIT",
            [RD_KAFKA_OP_NODE_UPDATE]      = "REPLY:NODE_UPDATE",
            [RD_KAFKA_OP_XMIT_BUF]         = "REPLY:XMIT_BUF",
            [RD_KAFKA_OP_RECV_BUF]         = "REPLY:RECV_BUF",
            [RD_KAFKA_OP_XMIT_RETRY]       = "REPLY:XMIT_RETRY",
            [RD_KAFKA_OP_FETCH_START]      = "REPLY:FETCH_START",
            [RD_KAFKA_OP_FETCH_STOP]       = "REPLY:FETCH_STOP",
            [RD_KAFKA_OP_SEEK]             = "REPLY:SEEK",
            [RD_KAFKA_OP_PAUSE]            = "REPLY:PAUSE",
            [RD_KAFKA_OP_OFFSET_FETCH]     = "REPLY:OFFSET_FETCH",
            [RD_KAFKA_OP_PARTITION_JOIN]   = "REPLY:PARTITION_JOIN",
            [RD_KAFKA_OP_PARTITION_LEAVE]  = "REPLY:PARTITION_LEAVE",
            [RD_KAFKA_OP_REBALANCE]        = "REPLY:REBALANCE",
            [RD_KAFKA_OP_TERMINATE]        = "REPLY:TERMINATE",
            [RD_KAFKA_OP_COORD_QUERY]      = "REPLY:COORD_QUERY",
            [RD_KAFKA_OP_SUBSCRIBE]        = "REPLY:SUBSCRIBE",
            [RD_KAFKA_OP_ASSIGN]           = "REPLY:ASSIGN",
            [RD_KAFKA_OP_GET_SUBSCRIPTION] = "REPLY:GET_SUBSCRIPTION",
            [RD_KAFKA_OP_GET_ASSIGNMENT]   = "REPLY:GET_ASSIGNMENT",
            [RD_KAFKA_OP_THROTTLE]         = "REPLY:THROTTLE",
            [RD_KAFKA_OP_NAME]             = "REPLY:NAME",
            [RD_KAFKA_OP_CG_METADATA]      = "REPLY:CG_METADATA",
            [RD_KAFKA_OP_OFFSET_RESET]     = "REPLY:OFFSET_RESET",
            [RD_KAFKA_OP_METADATA]         = "REPLY:METADATA",
            [RD_KAFKA_OP_LOG]              = "REPLY:LOG",
            [RD_KAFKA_OP_WAKEUP]           = "REPLY:WAKEUP",
            [RD_KAFKA_OP_CREATETOPICS]     = "REPLY:CREATETOPICS",
            [RD_KAFKA_OP_DELETETOPICS]     = "REPLY:DELETETOPICS",
            [RD_KAFKA_OP_CREATEPARTITIONS] = "REPLY:CREATEPARTITIONS",
            [RD_KAFKA_OP_ALTERCONFIGS]     = "REPLY:ALTERCONFIGS",
            [RD_KAFKA_OP_INCREMENTALALTERCONFIGS] =
                "REPLY:INCREMENTALALTERCONFIGS",
            [RD_KAFKA_OP_DESCRIBECONFIGS]    = "REPLY:DESCRIBECONFIGS",
            [RD_KAFKA_OP_DELETERECORDS]      = "REPLY:DELETERECORDS",
            [RD_KAFKA_OP_LISTCONSUMERGROUPS] = "REPLY:LISTCONSUMERGROUPS",
            [RD_KAFKA_OP_DESCRIBECONSUMERGROUPS] =
                "REPLY:DESCRIBECONSUMERGROUPS",
            [RD_KAFKA_OP_DESCRIBETOPICS]  = "REPLY:DESCRIBETOPICS",
            [RD_KAFKA_OP_DESCRIBECLUSTER] = "REPLY:DESCRIBECLUSTER",
            [RD_KAFKA_OP_DELETEGROUPS]    = "REPLY:DELETEGROUPS",
            [RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS] =
                "REPLY:DELETECONSUMERGROUPOFFSETS",
            [RD_KAFKA_OP_CREATEACLS]   = "REPLY:CREATEACLS",
            [RD_KAFKA_OP_DESCRIBEACLS] = "REPLY:DESCRIBEACLS",
            [RD_KAFKA_OP_DELETEACLS]   = "REPLY:DELETEACLS",
            [RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS] =
                "REPLY:ALTERCONSUMERGROUPOFFSETS",
            [RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS] =
                "REPLY:LISTCONSUMERGROUPOFFSETS",
            [RD_KAFKA_OP_ADMIN_FANOUT]        = "REPLY:ADMIN_FANOUT",
            [RD_KAFKA_OP_ADMIN_RESULT]        = "REPLY:ADMIN_RESULT",
            [RD_KAFKA_OP_PURGE]               = "REPLY:PURGE",
            [RD_KAFKA_OP_CONNECT]             = "REPLY:CONNECT",
            [RD_KAFKA_OP_OAUTHBEARER_REFRESH] = "REPLY:OAUTHBEARER_REFRESH",
            [RD_KAFKA_OP_MOCK]                = "REPLY:MOCK",
            [RD_KAFKA_OP_BROKER_MONITOR]      = "REPLY:BROKER_MONITOR",
            [RD_KAFKA_OP_TXN]                 = "REPLY:TXN",
            [RD_KAFKA_OP_GET_REBALANCE_PROTOCOL] =
                "REPLY:GET_REBALANCE_PROTOCOL",
            [RD_KAFKA_OP_LEADERS]     = "REPLY:LEADERS",
            [RD_KAFKA_OP_BARRIER]     = "REPLY:BARRIER",
            [RD_KAFKA_OP_SASL_REAUTH] = "REPLY:SASL_REAUTH",
            [RD_KAFKA_OP_ALTERUSERSCRAMCREDENTIALS] =
                "REPLY:ALTERUSERSCRAMCREDENTIALS",
            [RD_KAFKA_OP_DESCRIBEUSERSCRAMCREDENTIALS] =
                "REPLY:DESCRIBEUSERSCRAMCREDENTIALS",
            [RD_KAFKA_OP_LISTOFFSETS]     = "REPLY:LISTOFFSETS",
            [RD_KAFKA_OP_METADATA_UPDATE] = "REPLY:METADATA_UPDATE",
            [RD_KAFKA_OP_SET_TELEMETRY_BROKER] =
                "REPLY:RD_KAFKA_OP_SET_TELEMETRY_BROKER",
            [RD_KAFKA_OP_TERMINATE_TELEMETRY] =
                "REPLY:RD_KAFKA_OP_TERMINATE_TELEMETRY",
            [RD_KAFKA_OP_ELECTLEADERS] = "REPLY:ELECTLEADERS",
        };

        if (type & RD_KAFKA_OP_REPLY)
                skiplen = 0;

        rd_assert((names[type & ~RD_KAFKA_OP_FLAGMASK] != NULL) ||
                  !*"add OP type to rd_kafka_op2str()");
        return names[type & ~RD_KAFKA_OP_FLAGMASK] + skiplen;
}


void rd_kafka_op_print(FILE *fp, const char *prefix, rd_kafka_op_t *rko) {
        fprintf(fp,
                "%s((rd_kafka_op_t*)%p)\n"
                "%s Type: %s (0x%x), Version: %" PRId32 "\n",
                prefix, rko, prefix, rd_kafka_op2str(rko->rko_type),
                rko->rko_type, rko->rko_version);
        if (rko->rko_err)
                fprintf(fp, "%s Error: %s\n", prefix,
                        rd_kafka_err2str(rko->rko_err));
        if (rko->rko_replyq.q)
                fprintf(fp, "%s Replyq %p v%d (%s)\n", prefix,
                        rko->rko_replyq.q, rko->rko_replyq.version,
#if ENABLE_DEVEL
                        rko->rko_replyq._id
#else
                        ""
#endif
                );
        if (rko->rko_rktp) {
                fprintf(fp,
                        "%s ((rd_kafka_toppar_t*)%p) "
                        "%s [%" PRId32 "] v%d\n",
                        prefix, rko->rko_rktp,
                        rko->rko_rktp->rktp_rkt->rkt_topic->str,
                        rko->rko_rktp->rktp_partition,
                        rd_atomic32_get(&rko->rko_rktp->rktp_version));
        }

        switch (rko->rko_type & ~RD_KAFKA_OP_FLAGMASK) {
        case RD_KAFKA_OP_FETCH:
                fprintf(fp, "%s Offset: %" PRId64 "\n", prefix,
                        rko->rko_u.fetch.rkm.rkm_offset);
                break;
        case RD_KAFKA_OP_CONSUMER_ERR:
                fprintf(fp, "%s Offset: %" PRId64 "\n", prefix,
                        rko->rko_u.err.offset);
                /* FALLTHRU */
        case RD_KAFKA_OP_ERR:
                fprintf(fp, "%s Reason: %s\n", prefix, rko->rko_u.err.errstr);
                break;
        case RD_KAFKA_OP_DR:
                fprintf(fp, "%s %" PRId32 " messages on %s\n", prefix,
                        rko->rko_u.dr.msgq.rkmq_msg_cnt,
                        rko->rko_u.dr.rkt ? rko->rko_u.dr.rkt->rkt_topic->str
                                          : "(n/a)");
                break;
        case RD_KAFKA_OP_OFFSET_COMMIT:
                fprintf(fp, "%s Callback: %p (opaque %p)\n", prefix,
                        rko->rko_u.offset_commit.cb,
                        rko->rko_u.offset_commit.opaque);
                fprintf(fp, "%s %d partitions\n", prefix,
                        rko->rko_u.offset_commit.partitions
                            ? rko->rko_u.offset_commit.partitions->cnt
                            : 0);
                break;

        case RD_KAFKA_OP_LOG:
                fprintf(fp, "%s Log: %%%d %s: %s\n", prefix,
                        rko->rko_u.log.level, rko->rko_u.log.fac,
                        rko->rko_u.log.str);
                break;

        default:
                break;
        }
}


rd_kafka_op_t *rd_kafka_op_new0(const char *source, rd_kafka_op_type_t type) {
        rd_kafka_op_t *rko;
#define _RD_KAFKA_OP_EMPTY                                                     \
        1234567 /* Special value to be able to assert                          \
                 * on default-initialized (0) sizes                            \
                 * if we forgot to add an op type to                           \
                 * this list. */
        static const size_t op2size[RD_KAFKA_OP__END] = {
            [RD_KAFKA_OP_FETCH]            = sizeof(rko->rko_u.fetch),
            [RD_KAFKA_OP_ERR]              = sizeof(rko->rko_u.err),
            [RD_KAFKA_OP_CONSUMER_ERR]     = sizeof(rko->rko_u.err),
            [RD_KAFKA_OP_DR]               = sizeof(rko->rko_u.dr),
            [RD_KAFKA_OP_STATS]            = sizeof(rko->rko_u.stats),
            [RD_KAFKA_OP_OFFSET_COMMIT]    = sizeof(rko->rko_u.offset_commit),
            [RD_KAFKA_OP_NODE_UPDATE]      = sizeof(rko->rko_u.node),
            [RD_KAFKA_OP_XMIT_BUF]         = sizeof(rko->rko_u.xbuf),
            [RD_KAFKA_OP_RECV_BUF]         = sizeof(rko->rko_u.xbuf),
            [RD_KAFKA_OP_XMIT_RETRY]       = sizeof(rko->rko_u.xbuf),
            [RD_KAFKA_OP_FETCH_START]      = sizeof(rko->rko_u.fetch_start),
            [RD_KAFKA_OP_FETCH_STOP]       = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_SEEK]             = sizeof(rko->rko_u.fetch_start),
            [RD_KAFKA_OP_PAUSE]            = sizeof(rko->rko_u.pause),
            [RD_KAFKA_OP_OFFSET_FETCH]     = sizeof(rko->rko_u.offset_fetch),
            [RD_KAFKA_OP_PARTITION_JOIN]   = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_PARTITION_LEAVE]  = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_REBALANCE]        = sizeof(rko->rko_u.rebalance),
            [RD_KAFKA_OP_TERMINATE]        = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_COORD_QUERY]      = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_SUBSCRIBE]        = sizeof(rko->rko_u.subscribe),
            [RD_KAFKA_OP_ASSIGN]           = sizeof(rko->rko_u.assign),
            [RD_KAFKA_OP_GET_SUBSCRIPTION] = sizeof(rko->rko_u.subscribe),
            [RD_KAFKA_OP_GET_ASSIGNMENT]   = sizeof(rko->rko_u.assign),
            [RD_KAFKA_OP_THROTTLE]         = sizeof(rko->rko_u.throttle),
            [RD_KAFKA_OP_NAME]             = sizeof(rko->rko_u.name),
            [RD_KAFKA_OP_CG_METADATA]      = sizeof(rko->rko_u.cg_metadata),
            [RD_KAFKA_OP_OFFSET_RESET]     = sizeof(rko->rko_u.offset_reset),
            [RD_KAFKA_OP_METADATA]         = sizeof(rko->rko_u.metadata),
            [RD_KAFKA_OP_LOG]              = sizeof(rko->rko_u.log),
            [RD_KAFKA_OP_WAKEUP]           = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_CREATETOPICS]     = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DELETETOPICS]     = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_CREATEPARTITIONS] = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_ALTERCONFIGS]     = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_INCREMENTALALTERCONFIGS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBECONFIGS]    = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DELETERECORDS]      = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_LISTCONSUMERGROUPS] = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBECONSUMERGROUPS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBETOPICS]  = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBECLUSTER] = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DELETEGROUPS]    = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_CREATEACLS]   = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBEACLS] = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DELETEACLS]   = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_ADMIN_FANOUT] = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_ADMIN_RESULT] = sizeof(rko->rko_u.admin_result),
            [RD_KAFKA_OP_PURGE]        = sizeof(rko->rko_u.purge),
            [RD_KAFKA_OP_CONNECT]      = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_OAUTHBEARER_REFRESH] = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_MOCK]                = sizeof(rko->rko_u.mock),
            [RD_KAFKA_OP_BROKER_MONITOR] = sizeof(rko->rko_u.broker_monitor),
            [RD_KAFKA_OP_TXN]            = sizeof(rko->rko_u.txn),
            [RD_KAFKA_OP_GET_REBALANCE_PROTOCOL] =
                sizeof(rko->rko_u.rebalance_protocol),
            [RD_KAFKA_OP_LEADERS]     = sizeof(rko->rko_u.leaders),
            [RD_KAFKA_OP_BARRIER]     = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_SASL_REAUTH] = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_ALTERUSERSCRAMCREDENTIALS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_DESCRIBEUSERSCRAMCREDENTIALS] =
                sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_LISTOFFSETS]     = sizeof(rko->rko_u.admin_request),
            [RD_KAFKA_OP_METADATA_UPDATE] = sizeof(rko->rko_u.metadata),
            [RD_KAFKA_OP_SET_TELEMETRY_BROKER] =
                sizeof(rko->rko_u.telemetry_broker),
            [RD_KAFKA_OP_TERMINATE_TELEMETRY] = _RD_KAFKA_OP_EMPTY,
            [RD_KAFKA_OP_ELECTLEADERS] = sizeof(rko->rko_u.admin_request),
        };
        size_t tsize = op2size[type & ~RD_KAFKA_OP_FLAGMASK];

        rd_assert(tsize > 0 || !*"add OP type to rd_kafka_op_new0()");
        if (tsize == _RD_KAFKA_OP_EMPTY)
                tsize = 0;

        rko           = rd_calloc(1, sizeof(*rko) - sizeof(rko->rko_u) + tsize);
        rko->rko_type = type;

#if ENABLE_DEVEL
        rko->rko_source = source;
        rd_atomic32_add(&rd_kafka_op_cnt, 1);
#endif
        return rko;
}


void rd_kafka_op_destroy(rd_kafka_op_t *rko) {

        /* Call ops callback with ERR__DESTROY to let it
         * clean up its resources. */
        if ((rko->rko_type & RD_KAFKA_OP_CB) && rko->rko_op_cb) {
                rd_kafka_op_res_t res;
                rko->rko_err = RD_KAFKA_RESP_ERR__DESTROY;
                res          = rko->rko_op_cb(rko->rko_rk, NULL, rko);
                rd_assert(res != RD_KAFKA_OP_RES_YIELD);
                rd_assert(res != RD_KAFKA_OP_RES_KEEP);
        }


        switch (rko->rko_type & ~RD_KAFKA_OP_FLAGMASK) {
        case RD_KAFKA_OP_FETCH:
                rd_kafka_msg_destroy(NULL, &rko->rko_u.fetch.rkm);
                /* Decrease refcount on rkbuf to eventually rd_free shared buf*/
                if (rko->rko_u.fetch.rkbuf)
                        rd_kafka_buf_handle_op(rko, RD_KAFKA_RESP_ERR__DESTROY);

                break;

        case RD_KAFKA_OP_OFFSET_FETCH:
                if (rko->rko_u.offset_fetch.partitions &&
                    rko->rko_u.offset_fetch.do_free)
                        rd_kafka_topic_partition_list_destroy(
                            rko->rko_u.offset_fetch.partitions);
                break;

        case RD_KAFKA_OP_OFFSET_COMMIT:
                RD_IF_FREE(rko->rko_u.offset_commit.partitions,
                           rd_kafka_topic_partition_list_destroy);
                RD_IF_FREE(rko->rko_u.offset_commit.reason, rd_free);
                break;

        case RD_KAFKA_OP_SUBSCRIBE:
        case RD_KAFKA_OP_GET_SUBSCRIPTION:
                RD_IF_FREE(rko->rko_u.subscribe.topics,
                           rd_kafka_topic_partition_list_destroy);
                break;

        case RD_KAFKA_OP_ASSIGN:
        case RD_KAFKA_OP_GET_ASSIGNMENT:
                RD_IF_FREE(rko->rko_u.assign.partitions,
                           rd_kafka_topic_partition_list_destroy);
                break;

        case RD_KAFKA_OP_REBALANCE:
                RD_IF_FREE(rko->rko_u.rebalance.partitions,
                           rd_kafka_topic_partition_list_destroy);
                break;

        case RD_KAFKA_OP_NAME:
                RD_IF_FREE(rko->rko_u.name.str, rd_free);
                break;

        case RD_KAFKA_OP_CG_METADATA:
                RD_IF_FREE(rko->rko_u.cg_metadata,
                           rd_kafka_consumer_group_metadata_destroy);
                break;

        case RD_KAFKA_OP_ERR:
        case RD_KAFKA_OP_CONSUMER_ERR:
                RD_IF_FREE(rko->rko_u.err.errstr, rd_free);
                rd_kafka_msg_destroy(NULL, &rko->rko_u.err.rkm);
                break;

                break;

        case RD_KAFKA_OP_THROTTLE:
                RD_IF_FREE(rko->rko_u.throttle.nodename, rd_free);
                break;

        case RD_KAFKA_OP_STATS:
                RD_IF_FREE(rko->rko_u.stats.json, rd_free);
                break;

        case RD_KAFKA_OP_XMIT_RETRY:
        case RD_KAFKA_OP_XMIT_BUF:
        case RD_KAFKA_OP_RECV_BUF:
                if (rko->rko_u.xbuf.rkbuf)
                        rd_kafka_buf_handle_op(rko, RD_KAFKA_RESP_ERR__DESTROY);

                RD_IF_FREE(rko->rko_u.xbuf.rkbuf, rd_kafka_buf_destroy);
                break;

        case RD_KAFKA_OP_DR:
                rd_kafka_msgq_purge(rko->rko_rk, &rko->rko_u.dr.msgq);
                if (rko->rko_u.dr.do_purge2)
                        rd_kafka_msgq_purge(rko->rko_rk, &rko->rko_u.dr.msgq2);

                if (rko->rko_u.dr.rkt)
                        rd_kafka_topic_destroy0(rko->rko_u.dr.rkt);
                if (rko->rko_u.dr.presult)
                        rd_kafka_Produce_result_destroy(rko->rko_u.dr.presult);
                break;

        case RD_KAFKA_OP_OFFSET_RESET:
                RD_IF_FREE(rko->rko_u.offset_reset.reason, rd_free);
                break;

        case RD_KAFKA_OP_METADATA:
                RD_IF_FREE(rko->rko_u.metadata.md, rd_kafka_metadata_destroy);
                /* It's not needed to free metadata.mdi because they
                   are the in the same memory allocation. */
                break;

        case RD_KAFKA_OP_LOG:
                rd_free(rko->rko_u.log.str);
                break;

        case RD_KAFKA_OP_ADMIN_FANOUT:
                rd_assert(rko->rko_u.admin_request.fanout.outstanding == 0);
                rd_list_destroy(&rko->rko_u.admin_request.fanout.results);
        case RD_KAFKA_OP_CREATETOPICS:
        case RD_KAFKA_OP_DELETETOPICS:
        case RD_KAFKA_OP_CREATEPARTITIONS:
        case RD_KAFKA_OP_ALTERCONFIGS:
        case RD_KAFKA_OP_INCREMENTALALTERCONFIGS:
        case RD_KAFKA_OP_DESCRIBECONFIGS:
        case RD_KAFKA_OP_DELETERECORDS:
        case RD_KAFKA_OP_LISTCONSUMERGROUPS:
        case RD_KAFKA_OP_DESCRIBECONSUMERGROUPS:
        case RD_KAFKA_OP_DELETEGROUPS:
        case RD_KAFKA_OP_DELETECONSUMERGROUPOFFSETS:
        case RD_KAFKA_OP_CREATEACLS:
        case RD_KAFKA_OP_DESCRIBEACLS:
        case RD_KAFKA_OP_DELETEACLS:
        case RD_KAFKA_OP_ALTERCONSUMERGROUPOFFSETS:
        case RD_KAFKA_OP_DESCRIBETOPICS:
        case RD_KAFKA_OP_DESCRIBECLUSTER:
        case RD_KAFKA_OP_LISTCONSUMERGROUPOFFSETS:
        case RD_KAFKA_OP_ALTERUSERSCRAMCREDENTIALS:
        case RD_KAFKA_OP_DESCRIBEUSERSCRAMCREDENTIALS:
        case RD_KAFKA_OP_LISTOFFSETS:
        case RD_KAFKA_OP_ELECTLEADERS:
                rd_kafka_replyq_destroy(&rko->rko_u.admin_request.replyq);
                rd_list_destroy(&rko->rko_u.admin_request.args);
                if (rko->rko_u.admin_request.options.match_consumer_group_states
                        .u.PTR) {
                        rd_list_destroy(rko->rko_u.admin_request.options
                                            .match_consumer_group_states.u.PTR);
                }
                if (rko->rko_u.admin_request.options.match_consumer_group_types
                        .u.PTR) {
                        rd_list_destroy(rko->rko_u.admin_request.options
                                            .match_consumer_group_types.u.PTR);
                }
                rd_assert(!rko->rko_u.admin_request.fanout_parent);
                RD_IF_FREE(rko->rko_u.admin_request.coordkey, rd_free);
                break;

        case RD_KAFKA_OP_ADMIN_RESULT:
                rd_list_destroy(&rko->rko_u.admin_result.args);
                rd_list_destroy(&rko->rko_u.admin_result.results);
                RD_IF_FREE(rko->rko_u.admin_result.errstr, rd_free);
                rd_assert(!rko->rko_u.admin_result.fanout_parent);
                ;
                break;

        case RD_KAFKA_OP_MOCK:
                RD_IF_FREE(rko->rko_u.mock.name, rd_free);
                RD_IF_FREE(rko->rko_u.mock.str, rd_free);
                if (rko->rko_u.mock.metrics) {
                        int64_t i;
                        for (i = 0; i < rko->rko_u.mock.hi; i++)
                                rd_free(rko->rko_u.mock.metrics[i]);
                        rd_free(rko->rko_u.mock.metrics);
                }
                break;

        case RD_KAFKA_OP_BROKER_MONITOR:
                rd_kafka_broker_destroy(rko->rko_u.broker_monitor.rkb);
                break;

        case RD_KAFKA_OP_TXN:
                RD_IF_FREE(rko->rko_u.txn.group_id, rd_free);
                RD_IF_FREE(rko->rko_u.txn.offsets,
                           rd_kafka_topic_partition_list_destroy);
                RD_IF_FREE(rko->rko_u.txn.cgmetadata,
                           rd_kafka_consumer_group_metadata_destroy);
                break;

        case RD_KAFKA_OP_LEADERS:
                rd_assert(!rko->rko_u.leaders.eonce);
                rd_assert(!rko->rko_u.leaders.replyq.q);
                RD_IF_FREE(rko->rko_u.leaders.leaders, rd_list_destroy);
                RD_IF_FREE(rko->rko_u.leaders.partitions,
                           rd_kafka_topic_partition_list_destroy);
                break;

        case RD_KAFKA_OP_METADATA_UPDATE:
                RD_IF_FREE(rko->rko_u.metadata.md, rd_kafka_metadata_destroy);
                /* It's not needed to free metadata.mdi because they
                   are the in the same memory allocation. */
                break;

        case RD_KAFKA_OP_SET_TELEMETRY_BROKER:
                RD_IF_FREE(rko->rko_u.telemetry_broker.rkb,
                           rd_kafka_broker_destroy);
                break;

        default:
                break;
        }

        RD_IF_FREE(rko->rko_rktp, rd_kafka_toppar_destroy);

        RD_IF_FREE(rko->rko_error, rd_kafka_error_destroy);

        rd_kafka_replyq_destroy(&rko->rko_replyq);

#if ENABLE_DEVEL
        if (rd_atomic32_sub(&rd_kafka_op_cnt, 1) < 0)
                rd_kafka_assert(NULL, !*"rd_kafka_op_cnt < 0");
#endif

        rd_free(rko);
}



/**
 * Propagate an error event to the application on a specific queue.
 */
void rd_kafka_q_op_err(rd_kafka_q_t *rkq,
                       rd_kafka_resp_err_t err,
                       const char *fmt,
                       ...) {
        va_list ap;
        char buf[2048];
        rd_kafka_op_t *rko;

        va_start(ap, fmt);
        rd_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        rko                   = rd_kafka_op_new(RD_KAFKA_OP_ERR);
        rko->rko_err          = err;
        rko->rko_u.err.errstr = rd_strdup(buf);

        rd_kafka_q_enq(rkq, rko);
}



/**
 * @brief Enqueue RD_KAFKA_OP_CONSUMER_ERR on \p rkq.
 *
 * @param broker_id Is the relevant broker id, or RD_KAFKA_NODEID_UA (-1)
 *                  if not applicable.
 * @param err Error code.
 * @param version Queue version barrier, or 0 if not applicable.
 * @param topic May be NULL.
 * @param rktp May be NULL. Takes precedence over \p topic.
 * @param offset RD_KAFKA_OFFSET_INVALID if not applicable.
 *
 * @sa rd_kafka_q_op_err()
 */
void rd_kafka_consumer_err(rd_kafka_q_t *rkq,
                           int32_t broker_id,
                           rd_kafka_resp_err_t err,
                           int32_t version,
                           const char *topic,
                           rd_kafka_toppar_t *rktp,
                           int64_t offset,
                           const char *fmt,
                           ...) {
        va_list ap;
        char buf[2048];
        rd_kafka_op_t *rko;

        va_start(ap, fmt);
        rd_vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        rko                   = rd_kafka_op_new(RD_KAFKA_OP_CONSUMER_ERR);
        rko->rko_version      = version;
        rko->rko_err          = err;
        rko->rko_u.err.offset = offset;
        rko->rko_u.err.errstr = rd_strdup(buf);
        rko->rko_u.err.rkm.rkm_broker_id = broker_id;

        if (rktp)
                rko->rko_rktp = rd_kafka_toppar_keep(rktp);
        else if (topic)
                rko->rko_u.err.rkm.rkm_rkmessage.rkt =
                    (rd_kafka_topic_t *)rd_kafka_lwtopic_new(rkq->rkq_rk,
                                                             topic);


        rd_kafka_q_enq(rkq, rko);
}


/**
 * Creates a reply op based on 'rko_orig'.
 * If 'rko_orig' has rko_op_cb set the reply op will be OR:ed with
 * RD_KAFKA_OP_CB, else the reply type will be the original rko_type OR:ed
 * with RD_KAFKA_OP_REPLY.
 */
rd_kafka_op_t *rd_kafka_op_new_reply(rd_kafka_op_t *rko_orig,
                                     rd_kafka_resp_err_t err) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_op_new(rko_orig->rko_type | RD_KAFKA_OP_REPLY);
        rd_kafka_op_get_reply_version(rko, rko_orig);
        rko->rko_err = err;
        if (rko_orig->rko_rktp)
                rko->rko_rktp = rd_kafka_toppar_keep(rko_orig->rko_rktp);

        return rko;
}


/**
 * @brief Create new callback op for type \p type
 */
rd_kafka_op_t *rd_kafka_op_new_cb(rd_kafka_t *rk,
                                  rd_kafka_op_type_t type,
                                  rd_kafka_op_cb_t *cb) {
        rd_kafka_op_t *rko;
        rko            = rd_kafka_op_new(type | RD_KAFKA_OP_CB);
        rko->rko_op_cb = cb;
        rko->rko_rk    = rk;
        return rko;
}


/**
 * @brief Reply to 'rko' re-using the same rko with rko_err
 *        specified by \p err. rko_error is set to NULL.
 *
 * If there is no replyq the rko is destroyed.
 *
 * @returns 1 if op was enqueued, else 0 and rko is destroyed.
 */
int rd_kafka_op_reply(rd_kafka_op_t *rko, rd_kafka_resp_err_t err) {

        if (!rko->rko_replyq.q) {
                rd_kafka_op_destroy(rko);
                return 0;
        }

        rko->rko_type |= (rko->rko_op_cb ? RD_KAFKA_OP_CB : RD_KAFKA_OP_REPLY);
        rko->rko_err   = err;
        rko->rko_error = NULL;

        return rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
}


/**
 * @brief Reply to 'rko' re-using the same rko with rko_error specified
 *        by \p error (may be NULL) and rko_err set to the corresponding
 *        error code. Assumes ownership of \p error.
 *
 * If there is no replyq the rko is destroyed.
 *
 * @returns 1 if op was enqueued, else 0 and rko is destroyed.
 */
int rd_kafka_op_error_reply(rd_kafka_op_t *rko, rd_kafka_error_t *error) {

        if (!rko->rko_replyq.q) {
                RD_IF_FREE(error, rd_kafka_error_destroy);
                rd_kafka_op_destroy(rko);
                return 0;
        }

        rko->rko_type |= (rko->rko_op_cb ? RD_KAFKA_OP_CB : RD_KAFKA_OP_REPLY);
        rko->rko_err =
            error ? rd_kafka_error_code(error) : RD_KAFKA_RESP_ERR_NO_ERROR;
        rko->rko_error = error;

        return rd_kafka_replyq_enq(&rko->rko_replyq, rko, 0);
}


/**
 * @brief Send request to queue, wait for response.
 *
 * @returns response on success or NULL if destq is disabled.
 */
rd_kafka_op_t *rd_kafka_op_req0(rd_kafka_q_t *destq,
                                rd_kafka_q_t *recvq,
                                rd_kafka_op_t *rko,
                                int timeout_ms) {
        rd_kafka_op_t *reply;

        /* Indicate to destination where to send reply. */
        rd_kafka_op_set_replyq(rko, recvq, NULL);

        /* Enqueue op */
        if (!rd_kafka_q_enq(destq, rko))
                return NULL;

        /* Wait for reply */
        reply = rd_kafka_q_pop(recvq, rd_timeout_us(timeout_ms), 0);

        /* May be NULL for timeout */
        return reply;
}

/**
 * Send request to queue, wait for response.
 * Creates a temporary reply queue.
 */
rd_kafka_op_t *
rd_kafka_op_req(rd_kafka_q_t *destq, rd_kafka_op_t *rko, int timeout_ms) {
        rd_kafka_q_t *recvq;
        rd_kafka_op_t *reply;

        recvq = rd_kafka_q_new(destq->rkq_rk);

        reply = rd_kafka_op_req0(destq, recvq, rko, timeout_ms);

        rd_kafka_q_destroy_owner(recvq);

        return reply;
}


/**
 * Send simple type-only request to queue, wait for response.
 */
rd_kafka_op_t *rd_kafka_op_req2(rd_kafka_q_t *destq, rd_kafka_op_type_t type) {
        rd_kafka_op_t *rko;

        rko = rd_kafka_op_new(type);
        return rd_kafka_op_req(destq, rko, RD_POLL_INFINITE);
}


/**
 * Destroys the rko and returns its err.
 */
rd_kafka_resp_err_t rd_kafka_op_err_destroy(rd_kafka_op_t *rko) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR__TIMED_OUT;

        if (rko) {
                err = rko->rko_err;
                rd_kafka_op_destroy(rko);
        }
        return err;
}


/**
 * Destroys the rko and returns its error object or NULL if no error.
 */
rd_kafka_error_t *rd_kafka_op_error_destroy(rd_kafka_op_t *rko) {
        if (rko) {
                rd_kafka_error_t *error = rko->rko_error;
                rko->rko_error          = NULL;
                rd_kafka_op_destroy(rko);
                return error;
        }

        return rd_kafka_error_new(RD_KAFKA_RESP_ERR__TIMED_OUT,
                                  "Operation timed out");
}


/**
 * Call op callback
 */
rd_kafka_op_res_t
rd_kafka_op_call(rd_kafka_t *rk, rd_kafka_q_t *rkq, rd_kafka_op_t *rko) {
        rd_kafka_op_res_t res;
        rd_assert(rko->rko_op_cb);
        res = rko->rko_op_cb(rk, rkq, rko);
        if (unlikely(res == RD_KAFKA_OP_RES_YIELD || rd_kafka_yield_thread))
                return RD_KAFKA_OP_RES_YIELD;
        if (res != RD_KAFKA_OP_RES_KEEP)
                rko->rko_op_cb = NULL;
        return res;
}


/**
 * @brief Creates a new RD_KAFKA_OP_FETCH op representing a
 *        control message. The rkm_flags property is set to
 *        RD_KAFKA_MSG_F_CONTROL.
 */
rd_kafka_op_t *rd_kafka_op_new_ctrl_msg(rd_kafka_toppar_t *rktp,
                                        int32_t version,
                                        rd_kafka_buf_t *rkbuf,
                                        rd_kafka_fetch_pos_t pos) {
        rd_kafka_msg_t *rkm;
        rd_kafka_op_t *rko;

        rko = rd_kafka_op_new_fetch_msg(&rkm, rktp, version, rkbuf, pos, 0,
                                        NULL, 0, NULL);

        rkm->rkm_flags |= RD_KAFKA_MSG_F_CONTROL;

        return rko;
}

/**
 * @brief Creates a new RD_KAFKA_OP_FETCH op and sets up the
 *        embedded message according to the parameters.
 *
 * @param rkmp will be set to the embedded rkm in the rko (for convenience)
 * @param offset may be updated later if relative offset.
 */
rd_kafka_op_t *rd_kafka_op_new_fetch_msg(rd_kafka_msg_t **rkmp,
                                         rd_kafka_toppar_t *rktp,
                                         int32_t version,
                                         rd_kafka_buf_t *rkbuf,
                                         rd_kafka_fetch_pos_t pos,
                                         size_t key_len,
                                         const void *key,
                                         size_t val_len,
                                         const void *val) {
        rd_kafka_msg_t *rkm;
        rd_kafka_op_t *rko;

        rko              = rd_kafka_op_new(RD_KAFKA_OP_FETCH);
        rko->rko_rktp    = rd_kafka_toppar_keep(rktp);
        rko->rko_version = version;
        rkm              = &rko->rko_u.fetch.rkm;
        *rkmp            = rkm;

        /* Since all the ops share the same payload buffer
         * a refcnt is used on the rkbuf that makes sure all
         * consume_cb() will have been
         * called for each of these ops before the rkbuf
         * and its memory backing buffers are freed. */
        rko->rko_u.fetch.rkbuf = rkbuf;
        rd_kafka_buf_keep(rkbuf);

        rkm->rkm_offset                  = pos.offset;
        rkm->rkm_u.consumer.leader_epoch = pos.leader_epoch;

        rkm->rkm_key     = (void *)key;
        rkm->rkm_key_len = key_len;

        rkm->rkm_payload = (void *)val;
        rkm->rkm_len     = val_len;
        rko->rko_len     = (int32_t)rkm->rkm_len;

        rkm->rkm_partition = rktp->rktp_partition;

        /* Persistence status is always PERSISTED for consumed messages
         * since we managed to read the message. */
        rkm->rkm_status = RD_KAFKA_MSG_STATUS_PERSISTED;

        return rko;
}


/**
 * Enqueue ERR__THROTTLE op, if desired.
 */
void rd_kafka_op_throttle_time(rd_kafka_broker_t *rkb,
                               rd_kafka_q_t *rkq,
                               int throttle_time) {
        rd_kafka_op_t *rko;

        if (unlikely(throttle_time > 0)) {
                rd_avg_add(&rkb->rkb_avg_throttle, throttle_time);
                rd_avg_add(&rkb->rkb_telemetry.rd_avg_current.rkb_avg_throttle,
                           throttle_time);
        }

        /* We send throttle events when:
         *  - throttle_time > 0
         *  - throttle_time == 0 and last throttle_time > 0
         */
        if (!rkb->rkb_rk->rk_conf.throttle_cb ||
            (!throttle_time &&
             !rd_atomic32_get(&rkb->rkb_rk->rk_last_throttle)))
                return;

        rd_atomic32_set(&rkb->rkb_rk->rk_last_throttle, throttle_time);

        rko = rd_kafka_op_new(RD_KAFKA_OP_THROTTLE);
        rd_kafka_op_set_prio(rko, RD_KAFKA_PRIO_HIGH);
        rko->rko_u.throttle.nodename      = rd_strdup(rkb->rkb_nodename);
        rko->rko_u.throttle.nodeid        = rkb->rkb_nodeid;
        rko->rko_u.throttle.throttle_time = throttle_time;
        rd_kafka_q_enq(rkq, rko);
}


/**
 * @brief Handle standard op types.
 */
rd_kafka_op_res_t rd_kafka_op_handle_std(rd_kafka_t *rk,
                                         rd_kafka_q_t *rkq,
                                         rd_kafka_op_t *rko,
                                         int cb_type) {
        if (cb_type == RD_KAFKA_Q_CB_FORCE_RETURN)
                return RD_KAFKA_OP_RES_PASS;
        else if (unlikely(rd_kafka_op_is_ctrl_msg(rko))) {
                /* Control messages must not be exposed to the application
                 * but we need to store their offsets. */
                rd_kafka_fetch_op_app_prepare(rk, rko);
                return RD_KAFKA_OP_RES_HANDLED;
        } else if (cb_type != RD_KAFKA_Q_CB_EVENT &&
                   rko->rko_type & RD_KAFKA_OP_CB)
                return rd_kafka_op_call(rk, rkq, rko);
        else if (rko->rko_type == RD_KAFKA_OP_RECV_BUF) /* Handle Response */
                rd_kafka_buf_handle_op(rko, rko->rko_err);
        else if (cb_type != RD_KAFKA_Q_CB_RETURN &&
                 rko->rko_type & RD_KAFKA_OP_REPLY &&
                 rko->rko_err == RD_KAFKA_RESP_ERR__DESTROY)
                return RD_KAFKA_OP_RES_HANDLED; /* dest queue was
                                                 * probably disabled. */
        else
                return RD_KAFKA_OP_RES_PASS;

        return RD_KAFKA_OP_RES_HANDLED;
}


/**
 * @brief Attempt to handle op using its queue's serve callback,
 *        or the passed callback, or op_handle_std(), else do nothing.
 *
 * @param rkq is \p rko's queue (which it was unlinked from) with rkq_lock
 *            being held. Callback may re-enqueue the op on this queue
 *            and return YIELD.
 *
 * @returns HANDLED if op was handled (and destroyed), PASS if not,
 *          or YIELD if op was handled (maybe destroyed or re-enqueued)
 *          and caller must propagate yield upwards (cancel and return).
 */
rd_kafka_op_res_t rd_kafka_op_handle(rd_kafka_t *rk,
                                     rd_kafka_q_t *rkq,
                                     rd_kafka_op_t *rko,
                                     rd_kafka_q_cb_type_t cb_type,
                                     void *opaque,
                                     rd_kafka_q_serve_cb_t *callback) {
        rd_kafka_op_res_t res;

        if (rko->rko_serve) {
                callback              = rko->rko_serve;
                opaque                = rko->rko_serve_opaque;
                rko->rko_serve        = NULL;
                rko->rko_serve_opaque = NULL;
        }

        res = rd_kafka_op_handle_std(rk, rkq, rko, cb_type);
        if (res == RD_KAFKA_OP_RES_KEEP) {
                /* Op was handled but must not be destroyed. */
                return res;
        }
        if (res == RD_KAFKA_OP_RES_HANDLED) {
                rd_kafka_op_destroy(rko);
                return res;
        } else if (unlikely(res == RD_KAFKA_OP_RES_YIELD))
                return res;

        if (callback)
                res = callback(rk, rkq, rko, cb_type, opaque);

        return res;
}


/**
 * @brief Prepare passing message to application.
 *        This must be called just prior to passing/returning a consumed
 *        message to the application.
 *
 * Performs:
 *  - Store offset for fetched message + 1.
 *  - Updates the application offset (rktp_app_offset).
 *
 * @locks rktp_lock and rk_lock MUST NOT be held
 */
void rd_kafka_fetch_op_app_prepare(rd_kafka_t *rk, rd_kafka_op_t *rko) {
        rd_kafka_toppar_t *rktp;
        rd_kafka_fetch_pos_t pos;

        if (unlikely(rko->rko_type != RD_KAFKA_OP_FETCH || rko->rko_err))
                return;

        rktp = rko->rko_rktp;

        if (unlikely(!rk))
                rk = rktp->rktp_rkt->rkt_rk;

        pos.offset       = rko->rko_u.fetch.rkm.rkm_rkmessage.offset + 1;
        pos.leader_epoch = rko->rko_u.fetch.rkm.rkm_u.consumer.leader_epoch;

        rd_kafka_update_app_pos(rk, rktp, pos, RD_DO_LOCK);
}
