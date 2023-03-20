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
#ifndef _RDKAFKA_FEATURE_H_
#define _RDKAFKA_FEATURE_H_


/**
 * @brief Kafka protocol features
 */

/* Message version 1 (MagicByte=1):
 *  + relative offsets (KIP-31)
 *  + timestamps (KIP-32) */
#define RD_KAFKA_FEATURE_MSGVER1 0x1

/* ApiVersionQuery support (KIP-35) */
#define RD_KAFKA_FEATURE_APIVERSION 0x2

/* >= 0.9: Broker-based Balanced Consumer */
#define RD_KAFKA_FEATURE_BROKER_BALANCED_CONSUMER 0x4

/* >= 0.9: Produce/Fetch ThrottleTime reporting */
#define RD_KAFKA_FEATURE_THROTTLETIME 0x8

/* >= 0.9: SASL GSSAPI support */
#define RD_KAFKA_FEATURE_SASL_GSSAPI 0x10

/* >= 0.10: SaslMechanismRequest (KIP-43) */
#define RD_KAFKA_FEATURE_SASL_HANDSHAKE 0x20

/* >= 0.8.2.0: Broker-based Group coordinator */
#define RD_KAFKA_FEATURE_BROKER_GROUP_COORD 0x40

/* >= 0.8.2.0: KLZ4 compression (with bad and proper HC checksums) */
#define RD_KAFKA_FEATURE_KLZ4 0x80

/* >= 0.10.1.0: Time-based Offset fetch (KIP-79) */
#define RD_KAFKA_FEATURE_OFFSET_TIME 0x100

/* >= 0.11.0.0: Message version 2 (MagicByte=2):
 *  + EOS message format KIP-98 */
#define RD_KAFKA_FEATURE_MSGVER2 0x200

/* >= 0.11.0.0: Idempotent Producer support */
#define RD_KAFKA_FEATURE_IDEMPOTENT_PRODUCER 0x400

/* >= 2.1.0-IV2: ZSTD compression */
#define RD_KAFKA_FEATURE_ZSTD 0x800

/* >= 1.0.0: SaslAuthenticateRequest */
#define RD_KAFKA_FEATURE_SASL_AUTH_REQ 0x1000

/* Unit-test mock broker: broker supports everything.
 * Should be used with RD_KAFKA_FEATURE_ALL, but not be included in bitmask */
#define RD_KAFKA_FEATURE_UNITTEST 0x4000

/* All features (except UNITTEST) */
#define RD_KAFKA_FEATURE_ALL (0xffff & ~RD_KAFKA_FEATURE_UNITTEST)



int rd_kafka_get_legacy_ApiVersions(const char *broker_version,
                                    struct rd_kafka_ApiVersion **apisp,
                                    size_t *api_cntp,
                                    const char *fallback);
int rd_kafka_ApiVersion_is_queryable(const char *broker_version);
void rd_kafka_ApiVersions_copy(const struct rd_kafka_ApiVersion *src,
                               size_t src_cnt,
                               struct rd_kafka_ApiVersion **dstp,
                               size_t *dst_cntp);
int rd_kafka_features_check(rd_kafka_broker_t *rkb,
                            struct rd_kafka_ApiVersion *broker_apis,
                            size_t broker_api_cnt);

const char *rd_kafka_features2str(int features);

#endif /* _RDKAFKA_FEATURE_H_ */
