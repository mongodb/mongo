/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2016-2022, Magnus Edenhill
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


/**
 * @brief Converts op type to event type.
 * @returns the event type, or 0 if the op cannot be mapped to an event.
 */
static RD_UNUSED RD_INLINE rd_kafka_event_type_t
rd_kafka_op2event(rd_kafka_op_type_t optype) {
        static const rd_kafka_event_type_t map[RD_KAFKA_OP__END] = {
            [RD_KAFKA_OP_DR]            = RD_KAFKA_EVENT_DR,
            [RD_KAFKA_OP_FETCH]         = RD_KAFKA_EVENT_FETCH,
            [RD_KAFKA_OP_ERR]           = RD_KAFKA_EVENT_ERROR,
            [RD_KAFKA_OP_CONSUMER_ERR]  = RD_KAFKA_EVENT_ERROR,
            [RD_KAFKA_OP_REBALANCE]     = RD_KAFKA_EVENT_REBALANCE,
            [RD_KAFKA_OP_OFFSET_COMMIT] = RD_KAFKA_EVENT_OFFSET_COMMIT,
            [RD_KAFKA_OP_LOG]           = RD_KAFKA_EVENT_LOG,
            [RD_KAFKA_OP_STATS]         = RD_KAFKA_EVENT_STATS,
            [RD_KAFKA_OP_OAUTHBEARER_REFRESH] =
                RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH};

        return map[(int)optype & ~RD_KAFKA_OP_FLAGMASK];
}


/**
 * @brief Attempt to set up an event based on rko.
 * @returns 1 if op is event:able and set up, else 0.
 */
static RD_UNUSED RD_INLINE int rd_kafka_event_setup(rd_kafka_t *rk,
                                                    rd_kafka_op_t *rko) {

        if (unlikely(rko->rko_flags & RD_KAFKA_OP_F_FORCE_CB))
                return 0;

        if (!rko->rko_evtype)
                rko->rko_evtype = rd_kafka_op2event(rko->rko_type);

        switch (rko->rko_evtype) {
        case RD_KAFKA_EVENT_NONE:
                return 0;

        case RD_KAFKA_EVENT_DR:
                rko->rko_rk = rk;
                rd_dassert(!rko->rko_u.dr.do_purge2);
                rd_kafka_msgq_init(&rko->rko_u.dr.msgq2);
                rko->rko_u.dr.do_purge2 = 1;
                return 1;

        case RD_KAFKA_EVENT_ERROR:
                if (rko->rko_err == RD_KAFKA_RESP_ERR__FATAL) {
                        /* Translate ERR__FATAL to the underlying fatal error
                         * code and string */
                        rd_kafka_resp_err_t ferr;
                        char errstr[512];
                        ferr = rd_kafka_fatal_error(rk, errstr, sizeof(errstr));
                        if (likely(ferr)) {
                                rko->rko_err = ferr;
                                if (rko->rko_u.err.errstr)
                                        rd_free(rko->rko_u.err.errstr);
                                rko->rko_u.err.errstr = rd_strdup(errstr);
                                rko->rko_u.err.fatal  = 1;
                        }
                }
                return 1;

        case RD_KAFKA_EVENT_REBALANCE:
        case RD_KAFKA_EVENT_LOG:
        case RD_KAFKA_EVENT_OFFSET_COMMIT:
        case RD_KAFKA_EVENT_STATS:
        case RD_KAFKA_EVENT_CREATETOPICS_RESULT:
        case RD_KAFKA_EVENT_DELETETOPICS_RESULT:
        case RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT:
        case RD_KAFKA_EVENT_ALTERCONFIGS_RESULT:
        case RD_KAFKA_EVENT_INCREMENTALALTERCONFIGS_RESULT:
        case RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT:
        case RD_KAFKA_EVENT_DELETERECORDS_RESULT:
        case RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT:
        case RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT:
        case RD_KAFKA_EVENT_DESCRIBETOPICS_RESULT:
        case RD_KAFKA_EVENT_DESCRIBECLUSTER_RESULT:
        case RD_KAFKA_EVENT_DELETEGROUPS_RESULT:
        case RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT:
        case RD_KAFKA_EVENT_CREATEACLS_RESULT:
        case RD_KAFKA_EVENT_DESCRIBEACLS_RESULT:
        case RD_KAFKA_EVENT_DELETEACLS_RESULT:
        case RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT:
        case RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT:
        case RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH:
        case RD_KAFKA_EVENT_DESCRIBEUSERSCRAMCREDENTIALS_RESULT:
        case RD_KAFKA_EVENT_ALTERUSERSCRAMCREDENTIALS_RESULT:
        case RD_KAFKA_EVENT_LISTOFFSETS_RESULT:
        case RD_KAFKA_EVENT_ELECTLEADERS_RESULT:
                return 1;

        default:
                return 0;
        }
}
