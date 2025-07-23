/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2016, Magnus Edenhill
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
#include "rdkafka_feature.h"

#include <stdlib.h>

static const char *rd_kafka_feature_names[] = {"MsgVer1",
                                               "ApiVersion",
                                               "BrokerBalancedConsumer",
                                               "ThrottleTime",
                                               "Sasl",
                                               "SaslHandshake",
                                               "BrokerGroupCoordinator",
                                               "KLZ4",
                                               "OffsetTime",
                                               "MsgVer2",
                                               "IdempotentProducer",
                                               "ZSTD",
                                               "SaslAuthReq",
                                               "UnitTest",
                                               NULL};


static const struct rd_kafka_feature_map {
        /* RD_KAFKA_FEATURE_... */
        int feature;

        /* Depends on the following ApiVersions overlapping with
         * what the broker supports: */
        struct rd_kafka_ApiVersion depends[RD_KAFKAP__NUM];

} rd_kafka_feature_map[] = {
    /**
     * @brief List of features and the ApiVersions they depend on.
     *
     * The dependency list consists of the ApiKey followed by this
     * client's supported minimum and maximum API versions.
     * As long as this list and its versions overlaps with the
     * broker supported API versions the feature will be enabled.
     */
    {

        /* @brief >=0.10.0: Message.MagicByte version 1:
         * Relative offsets (KIP-31) and message timestamps (KIP-32). */
        .feature = RD_KAFKA_FEATURE_MSGVER1,
        .depends =
            {
                {RD_KAFKAP_Produce, 2, 2},
                {RD_KAFKAP_Fetch, 2, 2},
                {-1},
            },
    },
    {
        /* @brief >=0.11.0: Message.MagicByte version 2 */
        .feature = RD_KAFKA_FEATURE_MSGVER2,
        .depends =
            {
                {RD_KAFKAP_Produce, 3, 3},
                {RD_KAFKAP_Fetch, 4, 4},
                {-1},
            },
    },
    {
        /* @brief >=0.10.0: ApiVersionQuery support.
         * @remark This is a bit of chicken-and-egg problem but needs to be
         *         set by feature_check() to avoid the feature being cleared
         *         even when broker supports it. */
        .feature = RD_KAFKA_FEATURE_APIVERSION,
        .depends =
            {
                {RD_KAFKAP_ApiVersion, 0, 0},
                {-1},
            },
    },
    {
        /* @brief >=0.8.2.0: Broker-based Group coordinator */
        .feature = RD_KAFKA_FEATURE_BROKER_GROUP_COORD,
        .depends =
            {
                {RD_KAFKAP_FindCoordinator, 0, 0},
                {-1},
            },
    },
    {
        /* @brief >=0.9.0: Broker-based balanced consumer groups. */
        .feature = RD_KAFKA_FEATURE_BROKER_BALANCED_CONSUMER,
        .depends =
            {
                {RD_KAFKAP_FindCoordinator, 0, 0},
                {RD_KAFKAP_OffsetCommit, 1, 2},
                {RD_KAFKAP_OffsetFetch, 1, 1},
                {RD_KAFKAP_JoinGroup, 0, 0},
                {RD_KAFKAP_SyncGroup, 0, 0},
                {RD_KAFKAP_Heartbeat, 0, 0},
                {RD_KAFKAP_LeaveGroup, 0, 0},
                {-1},
            },
    },
    {
        /* @brief >=0.9.0: ThrottleTime */
        .feature = RD_KAFKA_FEATURE_THROTTLETIME,
        .depends =
            {
                {RD_KAFKAP_Produce, 1, 2},
                {RD_KAFKAP_Fetch, 1, 2},
                {-1},
            },

    },
    {
        /* @brief >=0.9.0: SASL (GSSAPI) authentication.
         * Since SASL is not using the Kafka protocol
         * we must use something else to map us to the
         * proper broker version support:
         * JoinGroup was released along with SASL in 0.9.0. */
        .feature = RD_KAFKA_FEATURE_SASL_GSSAPI,
        .depends =
            {
                {RD_KAFKAP_JoinGroup, 0, 0},
                {-1},
            },
    },
    {
        /* @brief >=0.10.0: SASL mechanism handshake (KIP-43)
         *                  to automatically support other mechanisms
         *                  than GSSAPI, such as PLAIN. */
        .feature = RD_KAFKA_FEATURE_SASL_HANDSHAKE,
        .depends =
            {
                {RD_KAFKAP_SaslHandshake, 0, 0},
                {-1},
            },
    },
    {
        /* @brief >=0.8.2: KLZ4 compression.
         * Since KLZ4 initially did not rely on a specific API
         * type or version (it does in >=0.10.0)
         * we must use something else to map us to the
         * proper broker version support:
         * GrooupCoordinator was released in 0.8.2 */
        .feature = RD_KAFKA_FEATURE_KLZ4,
        .depends =
            {
                {RD_KAFKAP_FindCoordinator, 0, 0},
                {-1},
            },
    },
    {/* @brief >=0.10.1.0: Offset v1 (KIP-79)
      * Time-based offset requests */
     .feature = RD_KAFKA_FEATURE_OFFSET_TIME,
     .depends =
         {
             {RD_KAFKAP_ListOffsets, 1, 1},
             {-1},
         }},
    {/* @brief >=0.11.0.0: Idempotent Producer*/
     .feature = RD_KAFKA_FEATURE_IDEMPOTENT_PRODUCER,
     .depends =
         {
             {RD_KAFKAP_InitProducerId, 0, 0},
             {-1},
         }},
    {
        /* @brief >=2.1.0-IV2: Support ZStandard Compression Codec (KIP-110) */
        .feature = RD_KAFKA_FEATURE_ZSTD,
        .depends =
            {
                {RD_KAFKAP_Produce, 7, 7},
                {RD_KAFKAP_Fetch, 10, 10},
                {-1},
            },
    },
    {
        /* @brief >=1.0.0: SaslAuthenticateRequest */
        .feature = RD_KAFKA_FEATURE_SASL_AUTH_REQ,
        .depends =
            {
                {RD_KAFKAP_SaslHandshake, 1, 1},
                {RD_KAFKAP_SaslAuthenticate, 0, 0},
                {-1},
            },
    },
    {.feature = 0}, /* sentinel */
};



/**
 * @brief In absence of KIP-35 support in earlier broker versions we provide
 * hardcoded lists that corresponds to older broker versions.
 */

/* >= 0.10.0.0: dummy for all future versions that support ApiVersionRequest */
static struct rd_kafka_ApiVersion rd_kafka_ApiVersion_Queryable[] = {
    {RD_KAFKAP_ApiVersion, 0, 0}};


/* =~ 0.9.0 */
static struct rd_kafka_ApiVersion rd_kafka_ApiVersion_0_9_0[] = {
    {RD_KAFKAP_Produce, 0, 1},         {RD_KAFKAP_Fetch, 0, 1},
    {RD_KAFKAP_ListOffsets, 0, 0},     {RD_KAFKAP_Metadata, 0, 0},
    {RD_KAFKAP_OffsetCommit, 0, 2},    {RD_KAFKAP_OffsetFetch, 0, 1},
    {RD_KAFKAP_FindCoordinator, 0, 0}, {RD_KAFKAP_JoinGroup, 0, 0},
    {RD_KAFKAP_Heartbeat, 0, 0},       {RD_KAFKAP_LeaveGroup, 0, 0},
    {RD_KAFKAP_SyncGroup, 0, 0},       {RD_KAFKAP_DescribeGroups, 0, 0},
    {RD_KAFKAP_ListGroups, 0, 0}};

/* =~ 0.8.2 */
static struct rd_kafka_ApiVersion rd_kafka_ApiVersion_0_8_2[] = {
    {RD_KAFKAP_Produce, 0, 0},        {RD_KAFKAP_Fetch, 0, 0},
    {RD_KAFKAP_ListOffsets, 0, 0},    {RD_KAFKAP_Metadata, 0, 0},
    {RD_KAFKAP_OffsetCommit, 0, 1},   {RD_KAFKAP_OffsetFetch, 0, 1},
    {RD_KAFKAP_FindCoordinator, 0, 0}};

/* =~ 0.8.1 */
static struct rd_kafka_ApiVersion rd_kafka_ApiVersion_0_8_1[] = {
    {RD_KAFKAP_Produce, 0, 0},      {RD_KAFKAP_Fetch, 0, 0},
    {RD_KAFKAP_ListOffsets, 0, 0},  {RD_KAFKAP_Metadata, 0, 0},
    {RD_KAFKAP_OffsetCommit, 0, 1}, {RD_KAFKAP_OffsetFetch, 0, 0}};

/* =~ 0.8.0 */
static struct rd_kafka_ApiVersion rd_kafka_ApiVersion_0_8_0[] = {
    {RD_KAFKAP_Produce, 0, 0},
    {RD_KAFKAP_Fetch, 0, 0},
    {RD_KAFKAP_ListOffsets, 0, 0},
    {RD_KAFKAP_Metadata, 0, 0}};


/**
 * @brief Returns the ApiVersion list for legacy broker versions that do not
 *        support the ApiVersionQuery request. E.g., brokers <0.10.0.
 *
 * @param broker_version Broker version to match (longest prefix matching).
 * @param use_default If no match is found return the default APIs (but return
 * 0).
 *
 * @returns 1 if \p broker_version was recognized: \p *apisp will point to
 *          the ApiVersion list and *api_cntp will be set to its element count.
 *          0 if \p broker_version was not recognized: \p *apisp remains
 * unchanged.
 *
 */
int rd_kafka_get_legacy_ApiVersions(const char *broker_version,
                                    struct rd_kafka_ApiVersion **apisp,
                                    size_t *api_cntp,
                                    const char *fallback) {
        static const struct {
                const char *pfx;
                struct rd_kafka_ApiVersion *apis;
                size_t api_cnt;
        } vermap[] = {
#define _VERMAP(PFX, APIS) {PFX, APIS, RD_ARRAYSIZE(APIS)}
            _VERMAP("0.9.0", rd_kafka_ApiVersion_0_9_0),
            _VERMAP("0.8.2", rd_kafka_ApiVersion_0_8_2),
            _VERMAP("0.8.1", rd_kafka_ApiVersion_0_8_1),
            _VERMAP("0.8.0", rd_kafka_ApiVersion_0_8_0),
            {"0.7.", NULL}, /* Unsupported */
            {"0.6.", NULL}, /* Unsupported */
            _VERMAP("", rd_kafka_ApiVersion_Queryable),
            {NULL}};
        int i;
        int fallback_i = -1;
        int ret        = 0;

        *apisp    = NULL;
        *api_cntp = 0;

        for (i = 0; vermap[i].pfx; i++) {
                if (!strncmp(vermap[i].pfx, broker_version,
                             strlen(vermap[i].pfx))) {
                        if (!vermap[i].apis)
                                return 0;
                        *apisp    = vermap[i].apis;
                        *api_cntp = vermap[i].api_cnt;
                        ret       = 1;
                        break;
                } else if (fallback && !strcmp(vermap[i].pfx, fallback))
                        fallback_i = i;
        }

        if (!*apisp && fallback) {
                rd_kafka_assert(NULL, fallback_i != -1);
                *apisp    = vermap[fallback_i].apis;
                *api_cntp = vermap[fallback_i].api_cnt;
        }

        return ret;
}


/**
 * @returns 1 if the provided broker version (probably)
 *          supports api.version.request.
 */
int rd_kafka_ApiVersion_is_queryable(const char *broker_version) {
        struct rd_kafka_ApiVersion *apis;
        size_t api_cnt;


        if (!rd_kafka_get_legacy_ApiVersions(broker_version, &apis, &api_cnt,
                                             0))
                return 0;

        return apis == rd_kafka_ApiVersion_Queryable;
}



/**
 * @brief Check if match's versions overlaps with \p apis.
 *
 * @returns 1 if true, else 0.
 * @remark \p apis must be sorted using rd_kafka_ApiVersion_key_cmp()
 */
static RD_INLINE int
rd_kafka_ApiVersion_check(const struct rd_kafka_ApiVersion *apis,
                          size_t api_cnt,
                          const struct rd_kafka_ApiVersion *match) {
        const struct rd_kafka_ApiVersion *api;

        api = bsearch(match, apis, api_cnt, sizeof(*apis),
                      rd_kafka_ApiVersion_key_cmp);
        if (unlikely(!api))
                return 0;

        return match->MinVer <= api->MaxVer && api->MinVer <= match->MaxVer;
}


/**
 * @brief Compare broker's supported API versions to our feature request map
 *        and enable/disable features accordingly.
 *
 * @param broker_apis Broker's supported APIs. If NULL the
 *        \p broker.version.fallback configuration property will specify a
 *        default legacy version to use.
 * @param broker_api_cnt Number of elements in \p broker_apis
 *
 * @returns the supported features (bitmask) to enable.
 */
int rd_kafka_features_check(rd_kafka_broker_t *rkb,
                            struct rd_kafka_ApiVersion *broker_apis,
                            size_t broker_api_cnt) {
        int features = 0;
        int i;

        /* Scan through features. */
        for (i = 0; rd_kafka_feature_map[i].feature != 0; i++) {
                const struct rd_kafka_ApiVersion *match;
                int fails = 0;

                /* For each feature check that all its API dependencies
                 * can be fullfilled. */

                for (match = &rd_kafka_feature_map[i].depends[0];
                     match->ApiKey != -1; match++) {
                        int r;

                        r = rd_kafka_ApiVersion_check(broker_apis,
                                                      broker_api_cnt, match);

                        rd_rkb_dbg(rkb, FEATURE, "APIVERSION",
                                   " Feature %s: %s (%hd..%hd) "
                                   "%ssupported by broker",
                                   rd_kafka_features2str(
                                       rd_kafka_feature_map[i].feature),
                                   rd_kafka_ApiKey2str(match->ApiKey),
                                   match->MinVer, match->MaxVer,
                                   r ? "" : "NOT ");

                        fails += !r;
                }

                rd_rkb_dbg(
                    rkb, FEATURE, "APIVERSION", "%s feature %s",
                    fails ? "Disabling" : "Enabling",
                    rd_kafka_features2str(rd_kafka_feature_map[i].feature));


                if (!fails)
                        features |= rd_kafka_feature_map[i].feature;
        }

        return features;
}



/**
 * @brief Make an allocated and sorted copy of \p src.
 */
void rd_kafka_ApiVersions_copy(const struct rd_kafka_ApiVersion *src,
                               size_t src_cnt,
                               struct rd_kafka_ApiVersion **dstp,
                               size_t *dst_cntp) {
        *dstp     = rd_memdup(src, sizeof(*src) * src_cnt);
        *dst_cntp = src_cnt;
        qsort(*dstp, *dst_cntp, sizeof(**dstp), rd_kafka_ApiVersion_key_cmp);
}



/**
 * @returns a human-readable feature flag string.
 */
const char *rd_kafka_features2str(int features) {
        static RD_TLS char ret[4][256];
        size_t of              = 0;
        static RD_TLS int reti = 0;
        int i;

        reti = (reti + 1) % 4;

        *ret[reti] = '\0';
        for (i = 0; rd_kafka_feature_names[i]; i++) {
                int r;
                if (!(features & (1 << i)))
                        continue;

                r = rd_snprintf(ret[reti] + of, sizeof(ret[reti]) - of, "%s%s",
                                of == 0 ? "" : ",", rd_kafka_feature_names[i]);
                if ((size_t)r > sizeof(ret[reti]) - of) {
                        /* Out of space */
                        memcpy(&ret[reti][sizeof(ret[reti]) - 3], "..", 3);
                        break;
                }

                of += r;
        }

        return ret[reti];
}
