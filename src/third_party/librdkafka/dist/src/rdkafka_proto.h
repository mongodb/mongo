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

#ifndef _RDKAFKA_PROTO_H_
#define _RDKAFKA_PROTO_H_


#include "rdstring.h"
#include "rdendian.h"
#include "rdvarint.h"
#include "rdbase64.h"

/* Protocol defines */
#include "rdkafka_protocol.h"



/** Default generic retry count for failed requests.
 *  This may be overriden for specific request types. */
#define RD_KAFKA_REQUEST_DEFAULT_RETRIES 2

/** Max (practically infinite) retry count */
#define RD_KAFKA_REQUEST_MAX_RETRIES INT_MAX

/** Do not retry request */
#define RD_KAFKA_REQUEST_NO_RETRIES 0


/**
 * Request types
 */
struct rd_kafkap_reqhdr {
        int32_t Size;
        int16_t ApiKey;
        int16_t ApiVersion;
        int32_t CorrId;
        /* ClientId follows */
};

#define RD_KAFKAP_REQHDR_SIZE (4 + 2 + 2 + 4)
#define RD_KAFKAP_RESHDR_SIZE (4 + 4)

/**
 * Response header
 */
struct rd_kafkap_reshdr {
        int32_t Size;
        int32_t CorrId;
};


/**
 * Request type v1 (flexible version)
 *
 * i32            Size
 * i16            ApiKey
 * i16            ApiVersion
 * i32            CorrId
 * string         ClientId   (2-byte encoding, not compact string)
 * uvarint        Tags
 * <Request payload>
 * uvarint        EndTags
 *
 * Any struct-type (non-primitive or array type) field in the request payload
 * must also have a trailing tags list, this goes for structs in arrays as well.
 */

/**
 * @brief Protocol request type (ApiKey) to name/string.
 *
 * Generate updates to this list with generate_proto.sh.
 */
static RD_UNUSED const char *rd_kafka_ApiKey2str(int16_t ApiKey) {
        static const char *names[] = {
            [RD_KAFKAP_Produce]                 = "Produce",
            [RD_KAFKAP_Fetch]                   = "Fetch",
            [RD_KAFKAP_ListOffsets]             = "ListOffsets",
            [RD_KAFKAP_Metadata]                = "Metadata",
            [RD_KAFKAP_LeaderAndIsr]            = "LeaderAndIsr",
            [RD_KAFKAP_StopReplica]             = "StopReplica",
            [RD_KAFKAP_UpdateMetadata]          = "UpdateMetadata",
            [RD_KAFKAP_ControlledShutdown]      = "ControlledShutdown",
            [RD_KAFKAP_OffsetCommit]            = "OffsetCommit",
            [RD_KAFKAP_OffsetFetch]             = "OffsetFetch",
            [RD_KAFKAP_FindCoordinator]         = "FindCoordinator",
            [RD_KAFKAP_JoinGroup]               = "JoinGroup",
            [RD_KAFKAP_Heartbeat]               = "Heartbeat",
            [RD_KAFKAP_LeaveGroup]              = "LeaveGroup",
            [RD_KAFKAP_SyncGroup]               = "SyncGroup",
            [RD_KAFKAP_DescribeGroups]          = "DescribeGroups",
            [RD_KAFKAP_ListGroups]              = "ListGroups",
            [RD_KAFKAP_SaslHandshake]           = "SaslHandshake",
            [RD_KAFKAP_ApiVersion]              = "ApiVersion",
            [RD_KAFKAP_CreateTopics]            = "CreateTopics",
            [RD_KAFKAP_DeleteTopics]            = "DeleteTopics",
            [RD_KAFKAP_DeleteRecords]           = "DeleteRecords",
            [RD_KAFKAP_InitProducerId]          = "InitProducerId",
            [RD_KAFKAP_OffsetForLeaderEpoch]    = "OffsetForLeaderEpoch",
            [RD_KAFKAP_AddPartitionsToTxn]      = "AddPartitionsToTxn",
            [RD_KAFKAP_AddOffsetsToTxn]         = "AddOffsetsToTxn",
            [RD_KAFKAP_EndTxn]                  = "EndTxn",
            [RD_KAFKAP_WriteTxnMarkers]         = "WriteTxnMarkers",
            [RD_KAFKAP_TxnOffsetCommit]         = "TxnOffsetCommit",
            [RD_KAFKAP_DescribeAcls]            = "DescribeAcls",
            [RD_KAFKAP_CreateAcls]              = "CreateAcls",
            [RD_KAFKAP_DeleteAcls]              = "DeleteAcls",
            [RD_KAFKAP_DescribeConfigs]         = "DescribeConfigs",
            [RD_KAFKAP_AlterConfigs]            = "AlterConfigs",
            [RD_KAFKAP_AlterReplicaLogDirs]     = "AlterReplicaLogDirs",
            [RD_KAFKAP_DescribeLogDirs]         = "DescribeLogDirs",
            [RD_KAFKAP_SaslAuthenticate]        = "SaslAuthenticate",
            [RD_KAFKAP_CreatePartitions]        = "CreatePartitions",
            [RD_KAFKAP_CreateDelegationToken]   = "CreateDelegationToken",
            [RD_KAFKAP_RenewDelegationToken]    = "RenewDelegationToken",
            [RD_KAFKAP_ExpireDelegationToken]   = "ExpireDelegationToken",
            [RD_KAFKAP_DescribeDelegationToken] = "DescribeDelegationToken",
            [RD_KAFKAP_DeleteGroups]            = "DeleteGroups",
            [RD_KAFKAP_ElectLeaders]            = "ElectLeadersRequest",
            [RD_KAFKAP_IncrementalAlterConfigs] =
                "IncrementalAlterConfigsRequest",
            [RD_KAFKAP_AlterPartitionReassignments] =
                "AlterPartitionReassignmentsRequest",
            [RD_KAFKAP_ListPartitionReassignments] =
                "ListPartitionReassignmentsRequest",
            [RD_KAFKAP_OffsetDelete]         = "OffsetDeleteRequest",
            [RD_KAFKAP_DescribeClientQuotas] = "DescribeClientQuotasRequest",
            [RD_KAFKAP_AlterClientQuotas]    = "AlterClientQuotasRequest",
            [RD_KAFKAP_DescribeUserScramCredentials] =
                "DescribeUserScramCredentialsRequest",
            [RD_KAFKAP_AlterUserScramCredentials] =
                "AlterUserScramCredentialsRequest",
            [RD_KAFKAP_Vote]                      = "VoteRequest",
            [RD_KAFKAP_BeginQuorumEpoch]          = "BeginQuorumEpochRequest",
            [RD_KAFKAP_EndQuorumEpoch]            = "EndQuorumEpochRequest",
            [RD_KAFKAP_DescribeQuorum]            = "DescribeQuorumRequest",
            [RD_KAFKAP_AlterIsr]                  = "AlterIsrRequest",
            [RD_KAFKAP_UpdateFeatures]            = "UpdateFeaturesRequest",
            [RD_KAFKAP_Envelope]                  = "EnvelopeRequest",
            [RD_KAFKAP_FetchSnapshot]             = "FetchSnapshot",
            [RD_KAFKAP_DescribeCluster]           = "DescribeCluster",
            [RD_KAFKAP_DescribeProducers]         = "DescribeProducers",
            [RD_KAFKAP_BrokerHeartbeat]           = "BrokerHeartbeat",
            [RD_KAFKAP_UnregisterBroker]          = "UnregisterBroker",
            [RD_KAFKAP_DescribeTransactions]      = "DescribeTransactions",
            [RD_KAFKAP_ListTransactions]          = "ListTransactions",
            [RD_KAFKAP_AllocateProducerIds]       = "AllocateProducerIds",
            [RD_KAFKAP_ConsumerGroupHeartbeat]    = "ConsumerGroupHeartbeat",
            [RD_KAFKAP_GetTelemetrySubscriptions] = "GetTelemetrySubscriptions",
            [RD_KAFKAP_PushTelemetry]             = "PushTelemetry",

        };
        static RD_TLS char ret[64];

        if (ApiKey < 0 || ApiKey >= (int)RD_ARRAYSIZE(names) ||
            !names[ApiKey]) {
                rd_snprintf(ret, sizeof(ret), "Unknown-%hd?", ApiKey);
                return ret;
        }

        return names[ApiKey];
}



/**
 * @brief ApiKey version support tuple.
 */
struct rd_kafka_ApiVersion {
        int16_t ApiKey;
        int16_t MinVer;
        int16_t MaxVer;
};

/**
 * @brief ApiVersion.ApiKey comparator.
 */
static RD_UNUSED int rd_kafka_ApiVersion_key_cmp(const void *_a,
                                                 const void *_b) {
        const struct rd_kafka_ApiVersion *a =
            (const struct rd_kafka_ApiVersion *)_a;
        const struct rd_kafka_ApiVersion *b =
            (const struct rd_kafka_ApiVersion *)_b;
        return RD_CMP(a->ApiKey, b->ApiKey);
}



typedef enum {
        RD_KAFKA_READ_UNCOMMITTED = 0,
        RD_KAFKA_READ_COMMITTED   = 1
} rd_kafka_isolation_level_t;



#define RD_KAFKA_CTRL_MSG_ABORT  0
#define RD_KAFKA_CTRL_MSG_COMMIT 1


/**
 * @enum Coordinator type, used with FindCoordinatorRequest
 */
typedef enum rd_kafka_coordtype_t {
        RD_KAFKA_COORD_GROUP = 0,
        RD_KAFKA_COORD_TXN   = 1
} rd_kafka_coordtype_t;


/**
 *
 * Kafka protocol string representation prefixed with a convenience header
 *
 * Serialized format:
 *  { uint16, data.. }
 *
 */
typedef struct rd_kafkap_str_s {
        /* convenience header (aligned access, host endian) */
        int len;         /* Kafka string length (-1=NULL, 0=empty, >0=string) */
        const char *str; /* points into data[] or other memory,
                          * not NULL-terminated */
} rd_kafkap_str_t;


#define RD_KAFKAP_STR_LEN_NULL      -1
#define RD_KAFKAP_STR_IS_NULL(kstr) ((kstr)->len == RD_KAFKAP_STR_LEN_NULL)

/* Returns the length of the string of a kafka protocol string representation */
#define RD_KAFKAP_STR_LEN0(len) ((len) == RD_KAFKAP_STR_LEN_NULL ? 0 : (len))
#define RD_KAFKAP_STR_LEN(kstr) RD_KAFKAP_STR_LEN0((kstr)->len)

/* Returns the actual size of a kafka protocol string representation. */
#define RD_KAFKAP_STR_SIZE0(len) (2 + RD_KAFKAP_STR_LEN0(len))
#define RD_KAFKAP_STR_SIZE(kstr) RD_KAFKAP_STR_SIZE0((kstr)->len)


/** @returns true if kstr is pre-serialized through .._new() */
#define RD_KAFKAP_STR_IS_SERIALIZED(kstr)                                      \
        (((const char *)((kstr) + 1)) + 2 == (const char *)((kstr)->str))

/* Serialized Kafka string: only works for _new() kstrs.
 * Check with RD_KAFKAP_STR_IS_SERIALIZED */
#define RD_KAFKAP_STR_SER(kstr) ((kstr) + 1)

/* Macro suitable for "%.*s" printing. */
#define RD_KAFKAP_STR_PR(kstr)                                                 \
        (int)((kstr)->len == RD_KAFKAP_STR_LEN_NULL ? 0 : (kstr)->len),        \
            (kstr)->str

/* strndupa() a Kafka string */
#define RD_KAFKAP_STR_DUPA(destptr, kstr)                                      \
        rd_strndupa((destptr), (kstr)->str, RD_KAFKAP_STR_LEN(kstr))

/* strndup() a Kafka string */
#define RD_KAFKAP_STR_DUP(kstr) rd_strndup((kstr)->str, RD_KAFKAP_STR_LEN(kstr))

#define RD_KAFKAP_STR_INITIALIZER                                              \
        { .len = RD_KAFKAP_STR_LEN_NULL, .str = NULL }

/**
 * Frees a Kafka string previously allocated with `rd_kafkap_str_new()`
 */
static RD_UNUSED void rd_kafkap_str_destroy(rd_kafkap_str_t *kstr) {
        rd_free(kstr);
}



/**
 * Allocate a new Kafka string and make a copy of 'str'.
 * If 'len' is -1 the length will be calculated.
 * Supports Kafka NULL strings.
 * Nul-terminates the string, but the trailing \0 is not part of
 * the serialized string.
 */
static RD_INLINE RD_UNUSED rd_kafkap_str_t *rd_kafkap_str_new(const char *str,
                                                              int len) {
        rd_kafkap_str_t *kstr;
        int16_t klen;

        if (!str)
                len = RD_KAFKAP_STR_LEN_NULL;
        else if (len == -1)
                len = (int)strlen(str);

        kstr = (rd_kafkap_str_t *)rd_malloc(
            sizeof(*kstr) + 2 + (len == RD_KAFKAP_STR_LEN_NULL ? 0 : len + 1));
        kstr->len = len;

        /* Serialised format: 16-bit string length */
        klen = htobe16(len);
        memcpy(kstr + 1, &klen, 2);

        /* Pre-Serialised format: non null-terminated string */
        if (len == RD_KAFKAP_STR_LEN_NULL)
                kstr->str = NULL;
        else {
                kstr->str = ((const char *)(kstr + 1)) + 2;
                memcpy((void *)kstr->str, str, len);
                ((char *)kstr->str)[len] = '\0';
        }

        return kstr;
}


/**
 * Makes a copy of `src`. The copy will be fully allocated and should
 * be freed with rd_kafka_pstr_destroy()
 */
static RD_INLINE RD_UNUSED rd_kafkap_str_t *
rd_kafkap_str_copy(const rd_kafkap_str_t *src) {
        return rd_kafkap_str_new(src->str, src->len);
}

static RD_INLINE RD_UNUSED int rd_kafkap_str_cmp(const rd_kafkap_str_t *a,
                                                 const rd_kafkap_str_t *b) {
        int minlen = RD_MIN(a->len, b->len);
        int r      = memcmp(a->str, b->str, minlen);
        if (r)
                return r;
        else
                return RD_CMP(a->len, b->len);
}

static RD_INLINE RD_UNUSED int rd_kafkap_str_cmp_str(const rd_kafkap_str_t *a,
                                                     const char *str) {
        int len    = (int)strlen(str);
        int minlen = RD_MIN(a->len, len);
        int r      = memcmp(a->str, str, minlen);
        if (r)
                return r;
        else
                return RD_CMP(a->len, len);
}

static RD_INLINE RD_UNUSED int
rd_kafkap_str_cmp_str2(const char *str, const rd_kafkap_str_t *b) {
        int len    = (int)strlen(str);
        int minlen = RD_MIN(b->len, len);
        int r      = memcmp(str, b->str, minlen);
        if (r)
                return r;
        else
                return RD_CMP(len, b->len);
}



/**
 *
 * Kafka protocol bytes array representation prefixed with a convenience header
 *
 * Serialized format:
 *  { uint32, data.. }
 *
 */
typedef struct rd_kafkap_bytes_s {
        /* convenience header (aligned access, host endian) */
        int32_t len;      /* Kafka bytes length (-1=NULL, 0=empty, >0=data) */
        const void *data; /* points just past the struct, or other memory,
                           * not NULL-terminated */
        const unsigned char _data[1]; /* Bytes following struct when new()ed */
} rd_kafkap_bytes_t;


#define RD_KAFKAP_BYTES_LEN_NULL -1
#define RD_KAFKAP_BYTES_IS_NULL(kbytes)                                        \
        ((kbytes)->len == RD_KAFKAP_BYTES_LEN_NULL)

/* Returns the length of the bytes of a kafka protocol bytes representation */
#define RD_KAFKAP_BYTES_LEN0(len)                                              \
        ((len) == RD_KAFKAP_BYTES_LEN_NULL ? 0 : (len))
#define RD_KAFKAP_BYTES_LEN(kbytes) RD_KAFKAP_BYTES_LEN0((kbytes)->len)

/* Returns the actual size of a kafka protocol bytes representation. */
#define RD_KAFKAP_BYTES_SIZE0(len)   (4 + RD_KAFKAP_BYTES_LEN0(len))
#define RD_KAFKAP_BYTES_SIZE(kbytes) RD_KAFKAP_BYTES_SIZE0((kbytes)->len)

/** @returns true if kbyes is pre-serialized through .._new() */
#define RD_KAFKAP_BYTES_IS_SERIALIZED(kstr)                                    \
        (((const char *)((kbytes) + 1)) + 2 == (const char *)((kbytes)->data))

/* Serialized Kafka bytes: only works for _new() kbytes */
#define RD_KAFKAP_BYTES_SER(kbytes) ((kbytes) + 1)


/**
 * Frees a Kafka bytes previously allocated with `rd_kafkap_bytes_new()`
 */
static RD_UNUSED void rd_kafkap_bytes_destroy(rd_kafkap_bytes_t *kbytes) {
        rd_free(kbytes);
}


/**
 * @brief Allocate a new Kafka bytes and make a copy of 'bytes'.
 * If \p len > 0 but \p bytes is NULL no copying is performed by
 * the bytes structure will be allocated to fit \p size bytes.
 *
 * Supports:
 *  - Kafka NULL bytes (bytes==NULL,len==0),
 *  - Empty bytes (bytes!=NULL,len==0)
 *  - Copy data (bytes!=NULL,len>0)
 *  - No-copy, just alloc (bytes==NULL,len>0)
 */
static RD_INLINE RD_UNUSED rd_kafkap_bytes_t *
rd_kafkap_bytes_new(const unsigned char *bytes, int32_t len) {
        rd_kafkap_bytes_t *kbytes;
        int32_t klen;

        if (!bytes && !len)
                len = RD_KAFKAP_BYTES_LEN_NULL;

        kbytes = (rd_kafkap_bytes_t *)rd_malloc(
            sizeof(*kbytes) + 4 + (len == RD_KAFKAP_BYTES_LEN_NULL ? 0 : len));
        kbytes->len = len;

        klen = htobe32(len);
        memcpy((void *)(kbytes + 1), &klen, 4);

        if (len == RD_KAFKAP_BYTES_LEN_NULL)
                kbytes->data = NULL;
        else {
                kbytes->data = ((const unsigned char *)(kbytes + 1)) + 4;
                if (bytes)
                        memcpy((void *)kbytes->data, bytes, len);
        }

        return kbytes;
}


/**
 * Makes a copy of `src`. The copy will be fully allocated and should
 * be freed with rd_kafkap_bytes_destroy()
 */
static RD_INLINE RD_UNUSED rd_kafkap_bytes_t *
rd_kafkap_bytes_copy(const rd_kafkap_bytes_t *src) {
        return rd_kafkap_bytes_new((const unsigned char *)src->data, src->len);
}


static RD_INLINE RD_UNUSED int rd_kafkap_bytes_cmp(const rd_kafkap_bytes_t *a,
                                                   const rd_kafkap_bytes_t *b) {
        int minlen = RD_MIN(a->len, b->len);
        int r      = memcmp(a->data, b->data, minlen);
        if (r)
                return r;
        else
                return RD_CMP(a->len, b->len);
}

static RD_INLINE RD_UNUSED int
rd_kafkap_bytes_cmp_data(const rd_kafkap_bytes_t *a,
                         const char *data,
                         int len) {
        int minlen = RD_MIN(a->len, len);
        int r      = memcmp(a->data, data, minlen);
        if (r)
                return r;
        else
                return RD_CMP(a->len, len);
}



typedef struct rd_kafka_buf_s rd_kafka_buf_t;


#define RD_KAFKA_NODENAME_SIZE 256



/**
 * @brief Message overheads (worst-case)
 */

/**
 * MsgVersion v0..v1
 */
/* Offset + MessageSize */
#define RD_KAFKAP_MESSAGESET_V0_HDR_SIZE (8 + 4)
/* CRC + Magic + Attr + KeyLen + ValueLen */
#define RD_KAFKAP_MESSAGE_V0_HDR_SIZE (4 + 1 + 1 + 4 + 4)
/* CRC + Magic + Attr + Timestamp + KeyLen + ValueLen */
#define RD_KAFKAP_MESSAGE_V1_HDR_SIZE (4 + 1 + 1 + 8 + 4 + 4)
/* Maximum per-message overhead */
#define RD_KAFKAP_MESSAGE_V0_OVERHEAD                                          \
        (RD_KAFKAP_MESSAGESET_V0_HDR_SIZE + RD_KAFKAP_MESSAGE_V0_HDR_SIZE)
#define RD_KAFKAP_MESSAGE_V1_OVERHEAD                                          \
        (RD_KAFKAP_MESSAGESET_V0_HDR_SIZE + RD_KAFKAP_MESSAGE_V1_HDR_SIZE)

/**
 * MsgVersion v2
 */
#define RD_KAFKAP_MESSAGE_V2_MAX_OVERHEAD                                      \
        (                                 /* Length (varint) */                \
         RD_UVARINT_ENC_SIZEOF(int32_t) + /* Attributes */                     \
         1 +                              /* TimestampDelta (varint) */        \
         RD_UVARINT_ENC_SIZEOF(int64_t) + /* OffsetDelta (varint) */           \
         RD_UVARINT_ENC_SIZEOF(int32_t) + /* KeyLen (varint) */                \
         RD_UVARINT_ENC_SIZEOF(int32_t) + /* ValueLen (varint) */              \
         RD_UVARINT_ENC_SIZEOF(int32_t) + /* HeaderCnt (varint): */            \
         RD_UVARINT_ENC_SIZEOF(int32_t))

#define RD_KAFKAP_MESSAGE_V2_MIN_OVERHEAD                                      \
        (                          /* Length (varint) */                       \
         RD_UVARINT_ENC_SIZE_0() + /* Attributes */                            \
         1 +                       /* TimestampDelta (varint) */               \
         RD_UVARINT_ENC_SIZE_0() + /* OffsetDelta (varint) */                  \
         RD_UVARINT_ENC_SIZE_0() + /* KeyLen (varint) */                       \
         RD_UVARINT_ENC_SIZE_0() + /* ValueLen (varint) */                     \
         RD_UVARINT_ENC_SIZE_0() + /* HeaderCnt (varint): */                   \
         RD_UVARINT_ENC_SIZE_0())


/**
 * @brief MessageSets are not explicitly versioned but depends on the
 *        Produce/Fetch API version and the encompassed Message versions.
 *        We use the Message version (MsgVersion, aka MagicByte) to describe
 *        the MessageSet version, that is, MsgVersion <= 1 uses the old
 *        MessageSet version (v0?) while MsgVersion 2 uses MessageSet version v2
 */

/* Old MessageSet header: none */
#define RD_KAFKAP_MSGSET_V0_SIZE 0

/* MessageSet v2 header */
#define RD_KAFKAP_MSGSET_V2_SIZE                                               \
        (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8 + 2 + 4 + 4)

/* Byte offsets for MessageSet fields */
#define RD_KAFKAP_MSGSET_V2_OF_Length          (8)
#define RD_KAFKAP_MSGSET_V2_OF_MagicByte       (8 + 4 + 4)
#define RD_KAFKAP_MSGSET_V2_OF_CRC             (8 + 4 + 4 + 1)
#define RD_KAFKAP_MSGSET_V2_OF_Attributes      (8 + 4 + 4 + 1 + 4)
#define RD_KAFKAP_MSGSET_V2_OF_LastOffsetDelta (8 + 4 + 4 + 1 + 4 + 2)
#define RD_KAFKAP_MSGSET_V2_OF_BaseTimestamp   (8 + 4 + 4 + 1 + 4 + 2 + 4)
#define RD_KAFKAP_MSGSET_V2_OF_MaxTimestamp    (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8)
#define RD_KAFKAP_MSGSET_V2_OF_ProducerId      (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8)
#define RD_KAFKAP_MSGSET_V2_OF_ProducerEpoch                                   \
        (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8)
#define RD_KAFKAP_MSGSET_V2_OF_BaseSequence                                    \
        (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8 + 2)
#define RD_KAFKAP_MSGSET_V2_OF_RecordCount                                     \
        (8 + 4 + 4 + 1 + 4 + 2 + 4 + 8 + 8 + 8 + 2 + 4)


/**
 * @struct Struct representing UUID protocol primitive type.
 */
typedef struct rd_kafka_Uuid_s {
        int64_t
            most_significant_bits; /**< Most significant 64 bits for the UUID */
        int64_t least_significant_bits; /**< Least significant 64 bits for the
                                           UUID */
        char base64str[23]; /**< base64 encoding for the uuid. By default, it is
                               lazy loaded. Use function
                               `rd_kafka_Uuid_base64str()` as a getter for this
                               field. */
} rd_kafka_Uuid_t;

#define RD_KAFKA_UUID_ZERO                                                     \
        (rd_kafka_Uuid_t) {                                                    \
                0, 0, ""                                                       \
        }

#define RD_KAFKA_UUID_IS_ZERO(uuid)                                            \
        (!rd_kafka_Uuid_cmp(uuid, RD_KAFKA_UUID_ZERO))

#define RD_KAFKA_UUID_METADATA_TOPIC_ID                                        \
        (rd_kafka_Uuid_t) {                                                    \
                0, 1, ""                                                       \
        }

static RD_INLINE RD_UNUSED int rd_kafka_Uuid_cmp(rd_kafka_Uuid_t a,
                                                 rd_kafka_Uuid_t b) {
        if (a.most_significant_bits < b.most_significant_bits)
                return -1;
        if (a.most_significant_bits > b.most_significant_bits)
                return 1;
        if (a.least_significant_bits < b.least_significant_bits)
                return -1;
        if (a.least_significant_bits > b.least_significant_bits)
                return 1;
        return 0;
}

static RD_INLINE RD_UNUSED int rd_kafka_Uuid_ptr_cmp(void *a, void *b) {
        rd_kafka_Uuid_t *a_uuid = a, *b_uuid = b;
        return rd_kafka_Uuid_cmp(*a_uuid, *b_uuid);
}

rd_kafka_Uuid_t rd_kafka_Uuid_random();

const char *rd_kafka_Uuid_str(const rd_kafka_Uuid_t *uuid);

unsigned int rd_kafka_Uuid_hash(const rd_kafka_Uuid_t *uuid);

unsigned int rd_kafka_Uuid_map_hash(const void *key);

/**
 * @brief UUID copier for rd_list_copy()
 */
static RD_UNUSED void *rd_list_Uuid_copy(const void *elem, void *opaque) {
        return (void *)rd_kafka_Uuid_copy((rd_kafka_Uuid_t *)elem);
}

static RD_INLINE RD_UNUSED void rd_list_Uuid_destroy(void *uuid) {
        rd_kafka_Uuid_destroy((rd_kafka_Uuid_t *)uuid);
}

static RD_INLINE RD_UNUSED int rd_list_Uuid_cmp(const void *uuid1,
                                                const void *uuid2) {
        return rd_kafka_Uuid_cmp(*((rd_kafka_Uuid_t *)uuid1),
                                 *((rd_kafka_Uuid_t *)uuid2));
}


/**
 * @name Producer ID and Epoch for the Idempotent Producer
 * @{
 *
 */

/**
 * @brief Producer ID and Epoch
 */
typedef struct rd_kafka_pid_s {
        int64_t id;    /**< Producer Id */
        int16_t epoch; /**< Producer Epoch */
} rd_kafka_pid_t;

#define RD_KAFKA_PID_INITIALIZER                                               \
        { -1, -1 }

/**
 * @returns true if \p PID is valid
 */
#define rd_kafka_pid_valid(PID) ((PID).id != -1)

/**
 * @brief Check two pids for equality
 */
static RD_UNUSED RD_INLINE int rd_kafka_pid_eq(const rd_kafka_pid_t a,
                                               const rd_kafka_pid_t b) {
        return a.id == b.id && a.epoch == b.epoch;
}

/**
 * @brief Pid+epoch comparator
 */
static RD_UNUSED int rd_kafka_pid_cmp(const void *_a, const void *_b) {
        const rd_kafka_pid_t *a = _a, *b = _b;

        if (a->id < b->id)
                return -1;
        else if (a->id > b->id)
                return 1;

        return (int)a->epoch - (int)b->epoch;
}


/**
 * @returns the string representation of a PID in a thread-safe
 *          static buffer.
 */
static RD_UNUSED const char *rd_kafka_pid2str(const rd_kafka_pid_t pid) {
        static RD_TLS char buf[2][64];
        static RD_TLS int i;

        if (!rd_kafka_pid_valid(pid))
                return "PID{Invalid}";

        i = (i + 1) % 2;

        rd_snprintf(buf[i], sizeof(buf[i]), "PID{Id:%" PRId64 ",Epoch:%hd}",
                    pid.id, pid.epoch);

        return buf[i];
}

/**
 * @brief Reset the PID to invalid/init state
 */
static RD_UNUSED RD_INLINE void rd_kafka_pid_reset(rd_kafka_pid_t *pid) {
        pid->id    = -1;
        pid->epoch = -1;
}


/**
 * @brief Bump the epoch of a valid PID
 */
static RD_UNUSED RD_INLINE rd_kafka_pid_t
rd_kafka_pid_bump(const rd_kafka_pid_t old) {
        rd_kafka_pid_t new_pid = {
            old.id, (int16_t)(((int)old.epoch + 1) & (int)INT16_MAX)};
        return new_pid;
}

/**@}*/


#endif /* _RDKAFKA_PROTO_H_ */
