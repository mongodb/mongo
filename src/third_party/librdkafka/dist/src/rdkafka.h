/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012-2022 Magnus Edenhill
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
 * @file rdkafka.h
 * @brief Apache Kafka C/C++ consumer and producer client library.
 *
 * rdkafka.h contains the public API for librdkafka.
 * The API is documented in this file as comments prefixing the function, type,
 * enum, define, etc.
 *
 * @sa For the C++ interface see rdkafkacpp.h
 *
 * @tableofcontents
 */


/* @cond NO_DOC */
#ifndef _RDKAFKA_H_
#define _RDKAFKA_H_

#include <stdio.h>
#include <inttypes.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#if 0
} /* Restore indent */
#endif
#endif

#ifdef _WIN32
#include <basetsd.h>
#ifndef WIN32_MEAN_AND_LEAN
#define WIN32_MEAN_AND_LEAN
#endif
#include <winsock2.h> /* for sockaddr, .. */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#define RD_UNUSED
#define RD_INLINE     __inline
#define RD_DEPRECATED __declspec(deprecated)
#define RD_FORMAT(...)
#undef RD_EXPORT
#ifdef LIBRDKAFKA_STATICLIB
#define RD_EXPORT
#else
#ifdef LIBRDKAFKA_EXPORTS
#define RD_EXPORT __declspec(dllexport)
#else
#define RD_EXPORT __declspec(dllimport)
#endif
#ifndef LIBRDKAFKA_TYPECHECKS
#define LIBRDKAFKA_TYPECHECKS 0
#endif
#endif

#else
#include <sys/socket.h> /* for sockaddr, .. */

#define RD_UNUSED __attribute__((unused))
#define RD_INLINE inline
#define RD_EXPORT
#define RD_DEPRECATED __attribute__((deprecated))

#if defined(__clang__) || defined(__GNUC__) || defined(__GNUG__)
#define RD_FORMAT(...) __attribute__((format(__VA_ARGS__)))
#else
#define RD_FORMAT(...)
#endif

#ifndef LIBRDKAFKA_TYPECHECKS
#define LIBRDKAFKA_TYPECHECKS 1
#endif
#endif


/**
 * @brief Type-checking macros
 * Compile-time checking that \p ARG is of type \p TYPE.
 * @returns \p RET
 */
#if LIBRDKAFKA_TYPECHECKS
#define _LRK_TYPECHECK(RET, TYPE, ARG)                                         \
        ({                                                                     \
                if (0) {                                                       \
                        TYPE __t RD_UNUSED = (ARG);                            \
                }                                                              \
                RET;                                                           \
        })

#define _LRK_TYPECHECK2(RET, TYPE, ARG, TYPE2, ARG2)                           \
        ({                                                                     \
                if (0) {                                                       \
                        TYPE __t RD_UNUSED   = (ARG);                          \
                        TYPE2 __t2 RD_UNUSED = (ARG2);                         \
                }                                                              \
                RET;                                                           \
        })

#define _LRK_TYPECHECK3(RET, TYPE, ARG, TYPE2, ARG2, TYPE3, ARG3)              \
        ({                                                                     \
                if (0) {                                                       \
                        TYPE __t RD_UNUSED   = (ARG);                          \
                        TYPE2 __t2 RD_UNUSED = (ARG2);                         \
                        TYPE3 __t3 RD_UNUSED = (ARG3);                         \
                }                                                              \
                RET;                                                           \
        })
#else
#define _LRK_TYPECHECK(RET, TYPE, ARG)                            (RET)
#define _LRK_TYPECHECK2(RET, TYPE, ARG, TYPE2, ARG2)              (RET)
#define _LRK_TYPECHECK3(RET, TYPE, ARG, TYPE2, ARG2, TYPE3, ARG3) (RET)
#endif

/* @endcond */


/**
 * @name librdkafka version
 * @{
 *
 *
 */

/**
 * @brief librdkafka version
 *
 * Interpreted as hex \c MM.mm.rr.xx:
 *  - MM = Major
 *  - mm = minor
 *  - rr = revision
 *  - xx = pre-release id (0xff is the final release)
 *
 * E.g.: \c 0x000801ff = 0.8.1
 *
 * @remark This value should only be used during compile time,
 *         for runtime checks of version use rd_kafka_version()
 */
#define RD_KAFKA_VERSION 0x020002ff

/**
 * @brief Returns the librdkafka version as integer.
 *
 * @returns Version integer.
 *
 * @sa See RD_KAFKA_VERSION for how to parse the integer format.
 * @sa Use rd_kafka_version_str() to retreive the version as a string.
 */
RD_EXPORT
int rd_kafka_version(void);

/**
 * @brief Returns the librdkafka version as string.
 *
 * @returns Version string
 */
RD_EXPORT
const char *rd_kafka_version_str(void);

/**@}*/


/**
 * @name Constants, errors, types
 * @{
 *
 *
 */


/**
 * @enum rd_kafka_type_t
 *
 * @brief rd_kafka_t handle type.
 *
 * @sa rd_kafka_new()
 */
typedef enum rd_kafka_type_t {
        RD_KAFKA_PRODUCER, /**< Producer client */
        RD_KAFKA_CONSUMER  /**< Consumer client */
} rd_kafka_type_t;


/*!
 * Timestamp types
 *
 * @sa rd_kafka_message_timestamp()
 */
typedef enum rd_kafka_timestamp_type_t {
        RD_KAFKA_TIMESTAMP_NOT_AVAILABLE,  /**< Timestamp not available */
        RD_KAFKA_TIMESTAMP_CREATE_TIME,    /**< Message creation time */
        RD_KAFKA_TIMESTAMP_LOG_APPEND_TIME /**< Log append time */
} rd_kafka_timestamp_type_t;



/**
 * @brief Retrieve supported debug contexts for use with the \c \"debug\"
 *        configuration property. (runtime)
 *
 * @returns Comma-separated list of available debugging contexts.
 */
RD_EXPORT
const char *rd_kafka_get_debug_contexts(void);

/**
 * @brief Supported debug contexts. (compile time)
 *
 * @deprecated This compile time value may be outdated at runtime due to
 *             linking another version of the library.
 *             Use rd_kafka_get_debug_contexts() instead.
 */
#define RD_KAFKA_DEBUG_CONTEXTS                                                \
        "all,generic,broker,topic,metadata,feature,queue,msg,protocol,cgrp,"   \
        "security,fetch,interceptor,plugin,consumer,admin,eos,mock,assignor,"  \
        "conf"


/* @cond NO_DOC */
/* Private types to provide ABI compatibility */
typedef struct rd_kafka_s rd_kafka_t;
typedef struct rd_kafka_topic_s rd_kafka_topic_t;
typedef struct rd_kafka_conf_s rd_kafka_conf_t;
typedef struct rd_kafka_topic_conf_s rd_kafka_topic_conf_t;
typedef struct rd_kafka_queue_s rd_kafka_queue_t;
typedef struct rd_kafka_op_s rd_kafka_event_t;
typedef struct rd_kafka_topic_result_s rd_kafka_topic_result_t;
typedef struct rd_kafka_consumer_group_metadata_s
    rd_kafka_consumer_group_metadata_t;
typedef struct rd_kafka_error_s rd_kafka_error_t;
typedef struct rd_kafka_headers_s rd_kafka_headers_t;
typedef struct rd_kafka_group_result_s rd_kafka_group_result_t;
typedef struct rd_kafka_acl_result_s rd_kafka_acl_result_t;
/* @endcond */


/**
 * @enum rd_kafka_resp_err_t
 * @brief Error codes.
 *
 * The negative error codes delimited by two underscores
 * (\c RD_KAFKA_RESP_ERR__..) denotes errors internal to librdkafka and are
 * displayed as \c \"Local: \<error string..\>\", while the error codes
 * delimited by a single underscore (\c RD_KAFKA_RESP_ERR_..) denote broker
 * errors and are displayed as \c \"Broker: \<error string..\>\".
 *
 * @sa Use rd_kafka_err2str() to translate an error code a human readable string
 */
typedef enum {
        /* Internal errors to rdkafka: */
        /** Begin internal error codes */
        RD_KAFKA_RESP_ERR__BEGIN = -200,
        /** Received message is incorrect */
        RD_KAFKA_RESP_ERR__BAD_MSG = -199,
        /** Bad/unknown compression */
        RD_KAFKA_RESP_ERR__BAD_COMPRESSION = -198,
        /** Broker is going away */
        RD_KAFKA_RESP_ERR__DESTROY = -197,
        /** Generic failure */
        RD_KAFKA_RESP_ERR__FAIL = -196,
        /** Broker transport failure */
        RD_KAFKA_RESP_ERR__TRANSPORT = -195,
        /** Critical system resource */
        RD_KAFKA_RESP_ERR__CRIT_SYS_RESOURCE = -194,
        /** Failed to resolve broker */
        RD_KAFKA_RESP_ERR__RESOLVE = -193,
        /** Produced message timed out*/
        RD_KAFKA_RESP_ERR__MSG_TIMED_OUT = -192,
        /** Reached the end of the topic+partition queue on
         * the broker. Not really an error.
         * This event is disabled by default,
         * see the `enable.partition.eof` configuration property. */
        RD_KAFKA_RESP_ERR__PARTITION_EOF = -191,
        /** Permanent: Partition does not exist in cluster. */
        RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION = -190,
        /** File or filesystem error */
        RD_KAFKA_RESP_ERR__FS = -189,
        /** Permanent: Topic does not exist in cluster. */
        RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC = -188,
        /** All broker connections are down. */
        RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN = -187,
        /** Invalid argument, or invalid configuration */
        RD_KAFKA_RESP_ERR__INVALID_ARG = -186,
        /** Operation timed out */
        RD_KAFKA_RESP_ERR__TIMED_OUT = -185,
        /** Queue is full */
        RD_KAFKA_RESP_ERR__QUEUE_FULL = -184,
        /** ISR count < required.acks */
        RD_KAFKA_RESP_ERR__ISR_INSUFF = -183,
        /** Broker node update */
        RD_KAFKA_RESP_ERR__NODE_UPDATE = -182,
        /** SSL error */
        RD_KAFKA_RESP_ERR__SSL = -181,
        /** Waiting for coordinator to become available. */
        RD_KAFKA_RESP_ERR__WAIT_COORD = -180,
        /** Unknown client group */
        RD_KAFKA_RESP_ERR__UNKNOWN_GROUP = -179,
        /** Operation in progress */
        RD_KAFKA_RESP_ERR__IN_PROGRESS = -178,
        /** Previous operation in progress, wait for it to finish. */
        RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS = -177,
        /** This operation would interfere with an existing subscription */
        RD_KAFKA_RESP_ERR__EXISTING_SUBSCRIPTION = -176,
        /** Assigned partitions (rebalance_cb) */
        RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS = -175,
        /** Revoked partitions (rebalance_cb) */
        RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS = -174,
        /** Conflicting use */
        RD_KAFKA_RESP_ERR__CONFLICT = -173,
        /** Wrong state */
        RD_KAFKA_RESP_ERR__STATE = -172,
        /** Unknown protocol */
        RD_KAFKA_RESP_ERR__UNKNOWN_PROTOCOL = -171,
        /** Not implemented */
        RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED = -170,
        /** Authentication failure*/
        RD_KAFKA_RESP_ERR__AUTHENTICATION = -169,
        /** No stored offset */
        RD_KAFKA_RESP_ERR__NO_OFFSET = -168,
        /** Outdated */
        RD_KAFKA_RESP_ERR__OUTDATED = -167,
        /** Timed out in queue */
        RD_KAFKA_RESP_ERR__TIMED_OUT_QUEUE = -166,
        /** Feature not supported by broker */
        RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE = -165,
        /** Awaiting cache update */
        RD_KAFKA_RESP_ERR__WAIT_CACHE = -164,
        /** Operation interrupted (e.g., due to yield)) */
        RD_KAFKA_RESP_ERR__INTR = -163,
        /** Key serialization error */
        RD_KAFKA_RESP_ERR__KEY_SERIALIZATION = -162,
        /** Value serialization error */
        RD_KAFKA_RESP_ERR__VALUE_SERIALIZATION = -161,
        /** Key deserialization error */
        RD_KAFKA_RESP_ERR__KEY_DESERIALIZATION = -160,
        /** Value deserialization error */
        RD_KAFKA_RESP_ERR__VALUE_DESERIALIZATION = -159,
        /** Partial response */
        RD_KAFKA_RESP_ERR__PARTIAL = -158,
        /** Modification attempted on read-only object */
        RD_KAFKA_RESP_ERR__READ_ONLY = -157,
        /** No such entry / item not found */
        RD_KAFKA_RESP_ERR__NOENT = -156,
        /** Read underflow */
        RD_KAFKA_RESP_ERR__UNDERFLOW = -155,
        /** Invalid type */
        RD_KAFKA_RESP_ERR__INVALID_TYPE = -154,
        /** Retry operation */
        RD_KAFKA_RESP_ERR__RETRY = -153,
        /** Purged in queue */
        RD_KAFKA_RESP_ERR__PURGE_QUEUE = -152,
        /** Purged in flight */
        RD_KAFKA_RESP_ERR__PURGE_INFLIGHT = -151,
        /** Fatal error: see rd_kafka_fatal_error() */
        RD_KAFKA_RESP_ERR__FATAL = -150,
        /** Inconsistent state */
        RD_KAFKA_RESP_ERR__INCONSISTENT = -149,
        /** Gap-less ordering would not be guaranteed if proceeding */
        RD_KAFKA_RESP_ERR__GAPLESS_GUARANTEE = -148,
        /** Maximum poll interval exceeded */
        RD_KAFKA_RESP_ERR__MAX_POLL_EXCEEDED = -147,
        /** Unknown broker */
        RD_KAFKA_RESP_ERR__UNKNOWN_BROKER = -146,
        /** Functionality not configured */
        RD_KAFKA_RESP_ERR__NOT_CONFIGURED = -145,
        /** Instance has been fenced */
        RD_KAFKA_RESP_ERR__FENCED = -144,
        /** Application generated error */
        RD_KAFKA_RESP_ERR__APPLICATION = -143,
        /** Assignment lost */
        RD_KAFKA_RESP_ERR__ASSIGNMENT_LOST = -142,
        /** No operation performed */
        RD_KAFKA_RESP_ERR__NOOP = -141,
        /** No offset to automatically reset to */
        RD_KAFKA_RESP_ERR__AUTO_OFFSET_RESET = -140,

        /** End internal error codes */
        RD_KAFKA_RESP_ERR__END = -100,

        /* Kafka broker errors: */
        /** Unknown broker error */
        RD_KAFKA_RESP_ERR_UNKNOWN = -1,
        /** Success */
        RD_KAFKA_RESP_ERR_NO_ERROR = 0,
        /** Offset out of range */
        RD_KAFKA_RESP_ERR_OFFSET_OUT_OF_RANGE = 1,
        /** Invalid message */
        RD_KAFKA_RESP_ERR_INVALID_MSG = 2,
        /** Unknown topic or partition */
        RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART = 3,
        /** Invalid message size */
        RD_KAFKA_RESP_ERR_INVALID_MSG_SIZE = 4,
        /** Leader not available */
        RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE = 5,
        /** Not leader for partition */
        RD_KAFKA_RESP_ERR_NOT_LEADER_FOR_PARTITION = 6,
        /** Request timed out */
        RD_KAFKA_RESP_ERR_REQUEST_TIMED_OUT = 7,
        /** Broker not available */
        RD_KAFKA_RESP_ERR_BROKER_NOT_AVAILABLE = 8,
        /** Replica not available */
        RD_KAFKA_RESP_ERR_REPLICA_NOT_AVAILABLE = 9,
        /** Message size too large */
        RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE = 10,
        /** StaleControllerEpochCode */
        RD_KAFKA_RESP_ERR_STALE_CTRL_EPOCH = 11,
        /** Offset metadata string too large */
        RD_KAFKA_RESP_ERR_OFFSET_METADATA_TOO_LARGE = 12,
        /** Broker disconnected before response received */
        RD_KAFKA_RESP_ERR_NETWORK_EXCEPTION = 13,
        /** Coordinator load in progress */
        RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS = 14,
/** Group coordinator load in progress */
#define RD_KAFKA_RESP_ERR_GROUP_LOAD_IN_PROGRESS                               \
        RD_KAFKA_RESP_ERR_COORDINATOR_LOAD_IN_PROGRESS
        /** Coordinator not available */
        RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE = 15,
/** Group coordinator not available */
#define RD_KAFKA_RESP_ERR_GROUP_COORDINATOR_NOT_AVAILABLE                      \
        RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE
        /** Not coordinator */
        RD_KAFKA_RESP_ERR_NOT_COORDINATOR = 16,
/** Not coordinator for group */
#define RD_KAFKA_RESP_ERR_NOT_COORDINATOR_FOR_GROUP                            \
        RD_KAFKA_RESP_ERR_NOT_COORDINATOR
        /** Invalid topic */
        RD_KAFKA_RESP_ERR_TOPIC_EXCEPTION = 17,
        /** Message batch larger than configured server segment size */
        RD_KAFKA_RESP_ERR_RECORD_LIST_TOO_LARGE = 18,
        /** Not enough in-sync replicas */
        RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS = 19,
        /** Message(s) written to insufficient number of in-sync replicas */
        RD_KAFKA_RESP_ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND = 20,
        /** Invalid required acks value */
        RD_KAFKA_RESP_ERR_INVALID_REQUIRED_ACKS = 21,
        /** Specified group generation id is not valid */
        RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION = 22,
        /** Inconsistent group protocol */
        RD_KAFKA_RESP_ERR_INCONSISTENT_GROUP_PROTOCOL = 23,
        /** Invalid group.id */
        RD_KAFKA_RESP_ERR_INVALID_GROUP_ID = 24,
        /** Unknown member */
        RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID = 25,
        /** Invalid session timeout */
        RD_KAFKA_RESP_ERR_INVALID_SESSION_TIMEOUT = 26,
        /** Group rebalance in progress */
        RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS = 27,
        /** Commit offset data size is not valid */
        RD_KAFKA_RESP_ERR_INVALID_COMMIT_OFFSET_SIZE = 28,
        /** Topic authorization failed */
        RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED = 29,
        /** Group authorization failed */
        RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED = 30,
        /** Cluster authorization failed */
        RD_KAFKA_RESP_ERR_CLUSTER_AUTHORIZATION_FAILED = 31,
        /** Invalid timestamp */
        RD_KAFKA_RESP_ERR_INVALID_TIMESTAMP = 32,
        /** Unsupported SASL mechanism */
        RD_KAFKA_RESP_ERR_UNSUPPORTED_SASL_MECHANISM = 33,
        /** Illegal SASL state */
        RD_KAFKA_RESP_ERR_ILLEGAL_SASL_STATE = 34,
        /** Unuspported version */
        RD_KAFKA_RESP_ERR_UNSUPPORTED_VERSION = 35,
        /** Topic already exists */
        RD_KAFKA_RESP_ERR_TOPIC_ALREADY_EXISTS = 36,
        /** Invalid number of partitions */
        RD_KAFKA_RESP_ERR_INVALID_PARTITIONS = 37,
        /** Invalid replication factor */
        RD_KAFKA_RESP_ERR_INVALID_REPLICATION_FACTOR = 38,
        /** Invalid replica assignment */
        RD_KAFKA_RESP_ERR_INVALID_REPLICA_ASSIGNMENT = 39,
        /** Invalid config */
        RD_KAFKA_RESP_ERR_INVALID_CONFIG = 40,
        /** Not controller for cluster */
        RD_KAFKA_RESP_ERR_NOT_CONTROLLER = 41,
        /** Invalid request */
        RD_KAFKA_RESP_ERR_INVALID_REQUEST = 42,
        /** Message format on broker does not support request */
        RD_KAFKA_RESP_ERR_UNSUPPORTED_FOR_MESSAGE_FORMAT = 43,
        /** Policy violation */
        RD_KAFKA_RESP_ERR_POLICY_VIOLATION = 44,
        /** Broker received an out of order sequence number */
        RD_KAFKA_RESP_ERR_OUT_OF_ORDER_SEQUENCE_NUMBER = 45,
        /** Broker received a duplicate sequence number */
        RD_KAFKA_RESP_ERR_DUPLICATE_SEQUENCE_NUMBER = 46,
        /** Producer attempted an operation with an old epoch */
        RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH = 47,
        /** Producer attempted a transactional operation in an invalid state */
        RD_KAFKA_RESP_ERR_INVALID_TXN_STATE = 48,
        /** Producer attempted to use a producer id which is not
         *  currently assigned to its transactional id */
        RD_KAFKA_RESP_ERR_INVALID_PRODUCER_ID_MAPPING = 49,
        /** Transaction timeout is larger than the maximum
         *  value allowed by the broker's max.transaction.timeout.ms */
        RD_KAFKA_RESP_ERR_INVALID_TRANSACTION_TIMEOUT = 50,
        /** Producer attempted to update a transaction while another
         *  concurrent operation on the same transaction was ongoing */
        RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS = 51,
        /** Indicates that the transaction coordinator sending a
         *  WriteTxnMarker is no longer the current coordinator for a
         *  given producer */
        RD_KAFKA_RESP_ERR_TRANSACTION_COORDINATOR_FENCED = 52,
        /** Transactional Id authorization failed */
        RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED = 53,
        /** Security features are disabled */
        RD_KAFKA_RESP_ERR_SECURITY_DISABLED = 54,
        /** Operation not attempted */
        RD_KAFKA_RESP_ERR_OPERATION_NOT_ATTEMPTED = 55,
        /** Disk error when trying to access log file on the disk */
        RD_KAFKA_RESP_ERR_KAFKA_STORAGE_ERROR = 56,
        /** The user-specified log directory is not found in the broker config
         */
        RD_KAFKA_RESP_ERR_LOG_DIR_NOT_FOUND = 57,
        /** SASL Authentication failed */
        RD_KAFKA_RESP_ERR_SASL_AUTHENTICATION_FAILED = 58,
        /** Unknown Producer Id */
        RD_KAFKA_RESP_ERR_UNKNOWN_PRODUCER_ID = 59,
        /** Partition reassignment is in progress */
        RD_KAFKA_RESP_ERR_REASSIGNMENT_IN_PROGRESS = 60,
        /** Delegation Token feature is not enabled */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTH_DISABLED = 61,
        /** Delegation Token is not found on server */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_NOT_FOUND = 62,
        /** Specified Principal is not valid Owner/Renewer */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_OWNER_MISMATCH = 63,
        /** Delegation Token requests are not allowed on this connection */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_REQUEST_NOT_ALLOWED = 64,
        /** Delegation Token authorization failed */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED = 65,
        /** Delegation Token is expired */
        RD_KAFKA_RESP_ERR_DELEGATION_TOKEN_EXPIRED = 66,
        /** Supplied principalType is not supported */
        RD_KAFKA_RESP_ERR_INVALID_PRINCIPAL_TYPE = 67,
        /** The group is not empty */
        RD_KAFKA_RESP_ERR_NON_EMPTY_GROUP = 68,
        /** The group id does not exist */
        RD_KAFKA_RESP_ERR_GROUP_ID_NOT_FOUND = 69,
        /** The fetch session ID was not found */
        RD_KAFKA_RESP_ERR_FETCH_SESSION_ID_NOT_FOUND = 70,
        /** The fetch session epoch is invalid */
        RD_KAFKA_RESP_ERR_INVALID_FETCH_SESSION_EPOCH = 71,
        /** No matching listener */
        RD_KAFKA_RESP_ERR_LISTENER_NOT_FOUND = 72,
        /** Topic deletion is disabled */
        RD_KAFKA_RESP_ERR_TOPIC_DELETION_DISABLED = 73,
        /** Leader epoch is older than broker epoch */
        RD_KAFKA_RESP_ERR_FENCED_LEADER_EPOCH = 74,
        /** Leader epoch is newer than broker epoch */
        RD_KAFKA_RESP_ERR_UNKNOWN_LEADER_EPOCH = 75,
        /** Unsupported compression type */
        RD_KAFKA_RESP_ERR_UNSUPPORTED_COMPRESSION_TYPE = 76,
        /** Broker epoch has changed */
        RD_KAFKA_RESP_ERR_STALE_BROKER_EPOCH = 77,
        /** Leader high watermark is not caught up */
        RD_KAFKA_RESP_ERR_OFFSET_NOT_AVAILABLE = 78,
        /** Group member needs a valid member ID */
        RD_KAFKA_RESP_ERR_MEMBER_ID_REQUIRED = 79,
        /** Preferred leader was not available */
        RD_KAFKA_RESP_ERR_PREFERRED_LEADER_NOT_AVAILABLE = 80,
        /** Consumer group has reached maximum size */
        RD_KAFKA_RESP_ERR_GROUP_MAX_SIZE_REACHED = 81,
        /** Static consumer fenced by other consumer with same
         *  group.instance.id. */
        RD_KAFKA_RESP_ERR_FENCED_INSTANCE_ID = 82,
        /** Eligible partition leaders are not available */
        RD_KAFKA_RESP_ERR_ELIGIBLE_LEADERS_NOT_AVAILABLE = 83,
        /** Leader election not needed for topic partition */
        RD_KAFKA_RESP_ERR_ELECTION_NOT_NEEDED = 84,
        /** No partition reassignment is in progress */
        RD_KAFKA_RESP_ERR_NO_REASSIGNMENT_IN_PROGRESS = 85,
        /** Deleting offsets of a topic while the consumer group is
         *  subscribed to it */
        RD_KAFKA_RESP_ERR_GROUP_SUBSCRIBED_TO_TOPIC = 86,
        /** Broker failed to validate record */
        RD_KAFKA_RESP_ERR_INVALID_RECORD = 87,
        /** There are unstable offsets that need to be cleared */
        RD_KAFKA_RESP_ERR_UNSTABLE_OFFSET_COMMIT = 88,
        /** Throttling quota has been exceeded */
        RD_KAFKA_RESP_ERR_THROTTLING_QUOTA_EXCEEDED = 89,
        /** There is a newer producer with the same transactionalId
         *  which fences the current one */
        RD_KAFKA_RESP_ERR_PRODUCER_FENCED = 90,
        /** Request illegally referred to resource that does not exist */
        RD_KAFKA_RESP_ERR_RESOURCE_NOT_FOUND = 91,
        /** Request illegally referred to the same resource twice */
        RD_KAFKA_RESP_ERR_DUPLICATE_RESOURCE = 92,
        /** Requested credential would not meet criteria for acceptability */
        RD_KAFKA_RESP_ERR_UNACCEPTABLE_CREDENTIAL = 93,
        /** Indicates that the either the sender or recipient of a
         *  voter-only request is not one of the expected voters */
        RD_KAFKA_RESP_ERR_INCONSISTENT_VOTER_SET = 94,
        /** Invalid update version */
        RD_KAFKA_RESP_ERR_INVALID_UPDATE_VERSION = 95,
        /** Unable to update finalized features due to server error */
        RD_KAFKA_RESP_ERR_FEATURE_UPDATE_FAILED = 96,
        /** Request principal deserialization failed during forwarding */
        RD_KAFKA_RESP_ERR_PRINCIPAL_DESERIALIZATION_FAILURE = 97,

        RD_KAFKA_RESP_ERR_END_ALL,
} rd_kafka_resp_err_t;


/**
 * @brief Error code value, name and description.
 *        Typically for use with language bindings to automatically expose
 *        the full set of librdkafka error codes.
 */
struct rd_kafka_err_desc {
        rd_kafka_resp_err_t code; /**< Error code */
        const char *name; /**< Error name, same as code enum sans prefix */
        const char *desc; /**< Human readable error description. */
};


/**
 * @brief Returns the full list of error codes.
 */
RD_EXPORT
void rd_kafka_get_err_descs(const struct rd_kafka_err_desc **errdescs,
                            size_t *cntp);



/**
 * @brief Returns a human readable representation of a kafka error.
 *
 * @param err Error code to translate
 */
RD_EXPORT
const char *rd_kafka_err2str(rd_kafka_resp_err_t err);



/**
 * @brief Returns the error code name (enum name).
 *
 * @param err Error code to translate
 */
RD_EXPORT
const char *rd_kafka_err2name(rd_kafka_resp_err_t err);


/**
 * @brief Returns the last error code generated by a legacy API call
 *        in the current thread.
 *
 * The legacy APIs are the ones using errno to propagate error value, namely:
 *  - rd_kafka_topic_new()
 *  - rd_kafka_consume_start()
 *  - rd_kafka_consume_stop()
 *  - rd_kafka_consume()
 *  - rd_kafka_consume_batch()
 *  - rd_kafka_consume_callback()
 *  - rd_kafka_consume_queue()
 *  - rd_kafka_produce()
 *
 * The main use for this function is to avoid converting system \p errno
 * values to rd_kafka_resp_err_t codes for legacy APIs.
 *
 * @remark The last error is stored per-thread, if multiple rd_kafka_t handles
 *         are used in the same application thread the developer needs to
 *         make sure rd_kafka_last_error() is called immediately after
 *         a failed API call.
 *
 * @remark errno propagation from librdkafka is not safe on Windows
 *         and should not be used, use rd_kafka_last_error() instead.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_last_error(void);


/**
 * @brief Converts the system errno value \p errnox to a rd_kafka_resp_err_t
 *        error code upon failure from the following functions:
 *  - rd_kafka_topic_new()
 *  - rd_kafka_consume_start()
 *  - rd_kafka_consume_stop()
 *  - rd_kafka_consume()
 *  - rd_kafka_consume_batch()
 *  - rd_kafka_consume_callback()
 *  - rd_kafka_consume_queue()
 *  - rd_kafka_produce()
 *
 * @param errnox  System errno value to convert
 *
 * @returns Appropriate error code for \p errnox
 *
 * @remark A better alternative is to call rd_kafka_last_error() immediately
 *         after any of the above functions return -1 or NULL.
 *
 * @deprecated Use rd_kafka_last_error() to retrieve the last error code
 *             set by the legacy librdkafka APIs.
 *
 * @sa rd_kafka_last_error()
 */
RD_EXPORT RD_DEPRECATED rd_kafka_resp_err_t rd_kafka_errno2err(int errnox);


/**
 * @brief Returns the thread-local system errno
 *
 * On most platforms this is the same as \p errno but in case of different
 * runtimes between library and application (e.g., Windows static DLLs)
 * this provides a means for exposing the errno librdkafka uses.
 *
 * @remark The value is local to the current calling thread.
 *
 * @deprecated Use rd_kafka_last_error() to retrieve the last error code
 *             set by the legacy librdkafka APIs.
 */
RD_EXPORT RD_DEPRECATED int rd_kafka_errno(void);



/**
 * @brief Returns the first fatal error set on this client instance,
 *        or RD_KAFKA_RESP_ERR_NO_ERROR if no fatal error has occurred.
 *
 * This function is to be used with the Idempotent Producer and \c error_cb
 * to detect fatal errors.
 *
 * Generally all errors raised by \c error_cb are to be considered
 * informational and temporary, the client will try to recover from all
 * errors in a graceful fashion (by retrying, etc).
 *
 * However, some errors should logically be considered fatal to retain
 * consistency; in particular a set of errors that may occur when using the
 * Idempotent Producer and the in-order or exactly-once producer guarantees
 * can't be satisfied.
 *
 * @param rk Client instance.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written to if there is a fatal error.
 * @param errstr_size Writable size in \p errstr.
 *
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if no fatal error has been raised, else
 *          any other error code.
 */
RD_EXPORT
rd_kafka_resp_err_t
rd_kafka_fatal_error(rd_kafka_t *rk, char *errstr, size_t errstr_size);


/**
 * @brief Trigger a fatal error for testing purposes.
 *
 * Since there is no practical way to trigger real fatal errors in the
 * idempotent producer, this method allows an application to trigger
 * fabricated fatal errors in tests to check its error handling code.
 *
 * @param rk Client instance.
 * @param err The underlying error code.
 * @param reason A human readable error reason.
 *               Will be prefixed with "test_fatal_error: " to differentiate
 *               from real fatal errors.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if a fatal error was triggered, or
 *          RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS if a previous fatal error
 *          has already been triggered.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_test_fatal_error(rd_kafka_t *rk,
                                                        rd_kafka_resp_err_t err,
                                                        const char *reason);


/**
 * @returns the error code for \p error or RD_KAFKA_RESP_ERR_NO_ERROR if
 *          \p error is NULL.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_error_code(const rd_kafka_error_t *error);

/**
 * @returns the error code name for \p error, e.g, "ERR_UNKNOWN_MEMBER_ID",
 *          or an empty string if \p error is NULL.
 *
 * @remark The lifetime of the returned pointer is the same as the error object.
 *
 * @sa rd_kafka_err2name()
 */
RD_EXPORT
const char *rd_kafka_error_name(const rd_kafka_error_t *error);

/**
 * @returns a human readable error string for \p error,
 *          or an empty string if \p error is NULL.
 *
 * @remark The lifetime of the returned pointer is the same as the error object.
 */
RD_EXPORT
const char *rd_kafka_error_string(const rd_kafka_error_t *error);


/**
 * @returns 1 if the error is a fatal error, indicating that the client
 *          instance is no longer usable, else 0 (also if \p error is NULL).
 */
RD_EXPORT
int rd_kafka_error_is_fatal(const rd_kafka_error_t *error);


/**
 * @returns 1 if the operation may be retried,
 *          else 0 (also if \p error is NULL).
 */
RD_EXPORT
int rd_kafka_error_is_retriable(const rd_kafka_error_t *error);


/**
 * @returns 1 if the error is an abortable transaction error in which case
 *          the application must call rd_kafka_abort_transaction() and
 *          start a new transaction with rd_kafka_begin_transaction() if it
 *          wishes to proceed with transactions.
 *          Else returns 0 (also if \p error is NULL).
 *
 * @remark The return value of this method is only valid for errors returned
 *         by the transactional API.
 */
RD_EXPORT
int rd_kafka_error_txn_requires_abort(const rd_kafka_error_t *error);

/**
 * @brief Free and destroy an error object.
 *
 * @remark As a conveniance it is permitted to pass a NULL \p error.
 */
RD_EXPORT
void rd_kafka_error_destroy(rd_kafka_error_t *error);


/**
 * @brief Create a new error object with error \p code and optional
 *        human readable error string in \p fmt.
 *
 * This method is mainly to be used for mocking errors in application test code.
 *
 * The returned object must be destroyed with rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_error_new(rd_kafka_resp_err_t code,
                                     const char *fmt,
                                     ...) RD_FORMAT(printf, 2, 3);


/**
 * @brief Topic+Partition place holder
 *
 * Generic place holder for a Topic+Partition and its related information
 * used for multiple purposes:
 *   - consumer offset (see rd_kafka_commit(), et.al.)
 *   - group rebalancing callback (rd_kafka_conf_set_rebalance_cb())
 *   - offset commit result callback (rd_kafka_conf_set_offset_commit_cb())
 */

/**
 * @brief Generic place holder for a specific Topic+Partition.
 *
 * @sa rd_kafka_topic_partition_list_new()
 */
typedef struct rd_kafka_topic_partition_s {
        char *topic;             /**< Topic name */
        int32_t partition;       /**< Partition */
        int64_t offset;          /**< Offset */
        void *metadata;          /**< Metadata */
        size_t metadata_size;    /**< Metadata size */
        void *opaque;            /**< Opaque value for application use */
        rd_kafka_resp_err_t err; /**< Error code, depending on use. */
        void *_private;          /**< INTERNAL USE ONLY,
                                  *   INITIALIZE TO ZERO, DO NOT TOUCH */
} rd_kafka_topic_partition_t;


/**
 * @brief Destroy a rd_kafka_topic_partition_t.
 * @remark This must not be called for elements in a topic partition list.
 */
RD_EXPORT
void rd_kafka_topic_partition_destroy(rd_kafka_topic_partition_t *rktpar);


/**
 * @brief A growable list of Topic+Partitions.
 *
 */
typedef struct rd_kafka_topic_partition_list_s {
        int cnt;                           /**< Current number of elements */
        int size;                          /**< Current allocated size */
        rd_kafka_topic_partition_t *elems; /**< Element array[] */
} rd_kafka_topic_partition_list_t;


/**
 * @brief Create a new list/vector Topic+Partition container.
 *
 * @param size  Initial allocated size used when the expected number of
 *              elements is known or can be estimated.
 *              Avoids reallocation and possibly relocation of the
 *              elems array.
 *
 * @returns A newly allocated Topic+Partition list.
 *
 * @remark Use rd_kafka_topic_partition_list_destroy() to free all resources
 *         in use by a list and the list itself.
 * @sa     rd_kafka_topic_partition_list_add()
 */
RD_EXPORT
rd_kafka_topic_partition_list_t *rd_kafka_topic_partition_list_new(int size);


/**
 * @brief Free all resources used by the list and the list itself.
 */
RD_EXPORT
void rd_kafka_topic_partition_list_destroy(
    rd_kafka_topic_partition_list_t *rkparlist);

/**
 * @brief Add topic+partition to list
 *
 * @param rktparlist List to extend
 * @param topic      Topic name (copied)
 * @param partition  Partition id
 *
 * @returns The object which can be used to fill in additionals fields.
 */
RD_EXPORT
rd_kafka_topic_partition_t *
rd_kafka_topic_partition_list_add(rd_kafka_topic_partition_list_t *rktparlist,
                                  const char *topic,
                                  int32_t partition);


/**
 * @brief Add range of partitions from \p start to \p stop inclusive.
 *
 * @param rktparlist List to extend
 * @param topic      Topic name (copied)
 * @param start      Start partition of range
 * @param stop       Last partition of range (inclusive)
 */
RD_EXPORT
void rd_kafka_topic_partition_list_add_range(
    rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t start,
    int32_t stop);



/**
 * @brief Delete partition from list.
 *
 * @param rktparlist List to modify
 * @param topic      Topic name to match
 * @param partition  Partition to match
 *
 * @returns 1 if partition was found (and removed), else 0.
 *
 * @remark Any held indices to elems[] are unusable after this call returns 1.
 */
RD_EXPORT
int rd_kafka_topic_partition_list_del(
    rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition);


/**
 * @brief Delete partition from list by elems[] index.
 *
 * @returns 1 if partition was found (and removed), else 0.
 *
 * @sa rd_kafka_topic_partition_list_del()
 */
RD_EXPORT
int rd_kafka_topic_partition_list_del_by_idx(
    rd_kafka_topic_partition_list_t *rktparlist,
    int idx);


/**
 * @brief Make a copy of an existing list.
 *
 * @param src   The existing list to copy.
 *
 * @returns A new list fully populated to be identical to \p src
 */
RD_EXPORT
rd_kafka_topic_partition_list_t *
rd_kafka_topic_partition_list_copy(const rd_kafka_topic_partition_list_t *src);



/**
 * @brief Set offset to \p offset for \p topic and \p partition
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or
 *          RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION if \p partition was not found
 *          in the list.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_topic_partition_list_set_offset(
    rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition,
    int64_t offset);



/**
 * @brief Find element by \p topic and \p partition.
 *
 * @returns a pointer to the first matching element, or NULL if not found.
 */
RD_EXPORT
rd_kafka_topic_partition_t *rd_kafka_topic_partition_list_find(
    const rd_kafka_topic_partition_list_t *rktparlist,
    const char *topic,
    int32_t partition);


/**
 * @brief Sort list using comparator \p cmp.
 *
 * If \p cmp is NULL the default comparator will be used that
 * sorts by ascending topic name and partition.
 *
 * \p cmp_opaque is provided as the \p cmp_opaque argument to \p cmp.
 *
 */
RD_EXPORT void rd_kafka_topic_partition_list_sort(
    rd_kafka_topic_partition_list_t *rktparlist,
    int (*cmp)(const void *a, const void *b, void *cmp_opaque),
    void *cmp_opaque);


/**@}*/



/**
 * @name Var-arg tag types
 * @{
 *
 */

/**
 * @enum rd_kafka_vtype_t
 *
 * @brief Var-arg tag types
 *
 * @sa rd_kafka_producev()
 */
typedef enum rd_kafka_vtype_t {
        RD_KAFKA_VTYPE_END,       /**< va-arg sentinel */
        RD_KAFKA_VTYPE_TOPIC,     /**< (const char *) Topic name */
        RD_KAFKA_VTYPE_RKT,       /**< (rd_kafka_topic_t *) Topic handle */
        RD_KAFKA_VTYPE_PARTITION, /**< (int32_t) Partition */
        RD_KAFKA_VTYPE_VALUE,    /**< (void *, size_t) Message value (payload)*/
        RD_KAFKA_VTYPE_KEY,      /**< (void *, size_t) Message key */
        RD_KAFKA_VTYPE_OPAQUE,   /**< (void *) Per-message application opaque
                                  *            value. This is the same as
                                  *            the _private field in
                                  *            rd_kafka_message_t, also known
                                  *            as the msg_opaque. */
        RD_KAFKA_VTYPE_MSGFLAGS, /**< (int) RD_KAFKA_MSG_F_.. flags */
        RD_KAFKA_VTYPE_TIMESTAMP, /**< (int64_t) Milliseconds since epoch UTC */
        RD_KAFKA_VTYPE_HEADER,    /**< (const char *, const void *, ssize_t)
                                   *   Message Header */
        RD_KAFKA_VTYPE_HEADERS,   /**< (rd_kafka_headers_t *) Headers list */
} rd_kafka_vtype_t;


/**
 * @brief VTYPE + argument container for use with rd_kafka_produce_va()
 *
 * See RD_KAFKA_V_..() macros below for which union field corresponds
 * to which RD_KAFKA_VTYPE_...
 */
typedef struct rd_kafka_vu_s {
        rd_kafka_vtype_t vtype; /**< RD_KAFKA_VTYPE_.. */
        /** Value union, see RD_KAFKA_V_.. macros for which field to use. */
        union {
                const char *cstr;
                rd_kafka_topic_t *rkt;
                int i;
                int32_t i32;
                int64_t i64;
                struct {
                        void *ptr;
                        size_t size;
                } mem;
                struct {
                        const char *name;
                        const void *val;
                        ssize_t size;
                } header;
                rd_kafka_headers_t *headers;
                void *ptr;
                char _pad[64]; /**< Padding size for future-proofness */
        } u;
} rd_kafka_vu_t;

/**
 * @brief Convenience macros for rd_kafka_vtype_t that takes the
 *        correct arguments for each vtype.
 */

/*!
 * va-arg end sentinel used to terminate the variable argument list
 */
#define RD_KAFKA_V_END RD_KAFKA_VTYPE_END

/*!
 * Topic name (const char *)
 *
 * rd_kafka_vu_t field: u.cstr
 */
#define RD_KAFKA_V_TOPIC(topic)                                                \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_TOPIC, const char *, topic),             \
            (const char *)topic
/*!
 * Topic object (rd_kafka_topic_t *)
 *
 * rd_kafka_vu_t field: u.rkt
 */
#define RD_KAFKA_V_RKT(rkt)                                                    \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_RKT, rd_kafka_topic_t *, rkt),           \
            (rd_kafka_topic_t *)rkt
/*!
 * Partition (int32_t)
 *
 * rd_kafka_vu_t field: u.i32
 */
#define RD_KAFKA_V_PARTITION(partition)                                        \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_PARTITION, int32_t, partition),          \
            (int32_t)partition
/*!
 * Message value/payload pointer and length (void *, size_t)
 *
 * rd_kafka_vu_t fields: u.mem.ptr, u.mem.size
 */
#define RD_KAFKA_V_VALUE(VALUE, LEN)                                           \
        _LRK_TYPECHECK2(RD_KAFKA_VTYPE_VALUE, void *, VALUE, size_t, LEN),     \
            (void *)VALUE, (size_t)LEN
/*!
 * Message key pointer and length (const void *, size_t)
 *
 * rd_kafka_vu_t field: u.mem.ptr, rd_kafka_vu.t.u.mem.size
 */
#define RD_KAFKA_V_KEY(KEY, LEN)                                               \
        _LRK_TYPECHECK2(RD_KAFKA_VTYPE_KEY, const void *, KEY, size_t, LEN),   \
            (void *)KEY, (size_t)LEN
/*!
 * Message opaque pointer (void *)
 * Same as \c msg_opaque, \c produce(.., msg_opaque),
 * and \c rkmessage->_private .
 *
 * rd_kafka_vu_t field: u.ptr
 */
#define RD_KAFKA_V_OPAQUE(msg_opaque)                                          \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_OPAQUE, void *, msg_opaque),             \
            (void *)msg_opaque
/*!
 * Message flags (int)
 * @sa RD_KAFKA_MSG_F_COPY, et.al.
 *
 * rd_kafka_vu_t field: u.i
 */
#define RD_KAFKA_V_MSGFLAGS(msgflags)                                          \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_MSGFLAGS, int, msgflags), (int)msgflags
/*!
 * Timestamp in milliseconds since epoch UTC (int64_t).
 * A value of 0 will use the current wall-clock time.
 *
 * rd_kafka_vu_t field: u.i64
 */
#define RD_KAFKA_V_TIMESTAMP(timestamp)                                        \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_TIMESTAMP, int64_t, timestamp),          \
            (int64_t)timestamp
/*!
 * Add Message Header (const char *NAME, const void *VALUE, ssize_t LEN).
 * @sa rd_kafka_header_add()
 * @remark RD_KAFKA_V_HEADER() and RD_KAFKA_V_HEADERS() MUST NOT be mixed
 *         in the same call to producev().
 *
 * rd_kafka_vu_t fields: u.header.name, u.header.val, u.header.size
 */
#define RD_KAFKA_V_HEADER(NAME, VALUE, LEN)                                    \
        _LRK_TYPECHECK3(RD_KAFKA_VTYPE_HEADER, const char *, NAME,             \
                        const void *, VALUE, ssize_t, LEN),                    \
            (const char *)NAME, (const void *)VALUE, (ssize_t)LEN

/*!
 * Message Headers list (rd_kafka_headers_t *).
 * The message object will assume ownership of the headers (unless producev()
 * fails).
 * Any existing headers will be replaced.
 * @sa rd_kafka_message_set_headers()
 * @remark RD_KAFKA_V_HEADER() and RD_KAFKA_V_HEADERS() MUST NOT be mixed
 *         in the same call to producev().
 *
 * rd_kafka_vu_t fields: u.headers
 */
#define RD_KAFKA_V_HEADERS(HDRS)                                               \
        _LRK_TYPECHECK(RD_KAFKA_VTYPE_HEADERS, rd_kafka_headers_t *, HDRS),    \
            (rd_kafka_headers_t *)HDRS


/**@}*/


/**
 * @name Message headers
 * @{
 *
 * @brief Message headers consist of a list of (string key, binary value) pairs.
 *        Duplicate keys are supported and the order in which keys were
 *        added are retained.
 *
 *        Header values are considered binary and may have three types of
 *        value:
 *          - proper value with size > 0 and a valid pointer
 *          - empty value with size = 0 and any non-NULL pointer
 *          - null value with size = 0 and a NULL pointer
 *
 *        Headers require Apache Kafka broker version v0.11.0.0 or later.
 *
 *        Header operations are O(n).
 */


/**
 * @brief Create a new headers list.
 *
 * @param initial_count Preallocate space for this number of headers.
 *                      Any number of headers may be added, updated and
 *                      removed regardless of the initial count.
 */
RD_EXPORT rd_kafka_headers_t *rd_kafka_headers_new(size_t initial_count);

/**
 * @brief Destroy the headers list. The object and any returned value pointers
 *        are not usable after this call.
 */
RD_EXPORT void rd_kafka_headers_destroy(rd_kafka_headers_t *hdrs);

/**
 * @brief Make a copy of headers list \p src.
 */
RD_EXPORT rd_kafka_headers_t *
rd_kafka_headers_copy(const rd_kafka_headers_t *src);

/**
 * @brief Add header with name \p name and value \p val (copied) of size
 *        \p size (not including null-terminator).
 *
 * @param hdrs       Headers list.
 * @param name       Header name.
 * @param name_size  Header name size (not including the null-terminator).
 *                   If -1 the \p name length is automatically acquired using
 *                   strlen().
 * @param value      Pointer to header value, or NULL (set size to 0 or -1).
 * @param value_size Size of header value. If -1 the \p value is assumed to be a
 *                   null-terminated string and the length is automatically
 *                   acquired using strlen().
 *
 * @returns RD_KAFKA_RESP_ERR__READ_ONLY if the headers are read-only,
 *          else RD_KAFKA_RESP_ERR_NO_ERROR.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_header_add(rd_kafka_headers_t *hdrs,
                                                  const char *name,
                                                  ssize_t name_size,
                                                  const void *value,
                                                  ssize_t value_size);

/**
 * @brief Remove all headers for the given key (if any).
 *
 * @returns RD_KAFKA_RESP_ERR__READ_ONLY if the headers are read-only,
 *          RD_KAFKA_RESP_ERR__NOENT if no matching headers were found,
 *          else RD_KAFKA_RESP_ERR_NO_ERROR if headers were removed.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_header_remove(rd_kafka_headers_t *hdrs,
                                                     const char *name);


/**
 * @brief Find last header in list \p hdrs matching \p name.
 *
 * @param hdrs   Headers list.
 * @param name   Header to find (last match).
 * @param valuep (out) Set to a (null-terminated) const pointer to the value
 *               (may be NULL).
 * @param sizep  (out) Set to the value's size (not including null-terminator).
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if an entry was found, else
 *          RD_KAFKA_RESP_ERR__NOENT.
 *
 * @remark The returned pointer in \p valuep includes a trailing null-terminator
 *         that is not accounted for in \p sizep.
 * @remark The returned pointer is only valid as long as the headers list and
 *         the header item is valid.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_header_get_last(const rd_kafka_headers_t *hdrs,
                         const char *name,
                         const void **valuep,
                         size_t *sizep);

/**
 * @brief Iterator for headers matching \p name.
 *
 *        Same semantics as rd_kafka_header_get_last()
 *
 * @param hdrs   Headers to iterate.
 * @param idx    Iterator index, start at 0 and increment by one for each call
 *               as long as RD_KAFKA_RESP_ERR_NO_ERROR is returned.
 * @param name   Header name to match.
 * @param valuep (out) Set to a (null-terminated) const pointer to the value
 *               (may be NULL).
 * @param sizep  (out) Set to the value's size (not including null-terminator).
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_header_get(const rd_kafka_headers_t *hdrs,
                    size_t idx,
                    const char *name,
                    const void **valuep,
                    size_t *sizep);


/**
 * @brief Iterator for all headers.
 *
 *        Same semantics as rd_kafka_header_get()
 *
 * @sa rd_kafka_header_get()
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_header_get_all(const rd_kafka_headers_t *hdrs,
                        size_t idx,
                        const char **namep,
                        const void **valuep,
                        size_t *sizep);



/**@}*/



/**
 * @name Kafka messages
 * @{
 *
 */



// FIXME: This doesn't show up in docs for some reason
// "Compound rd_kafka_message_t is not documented."

/**
 * @brief A Kafka message as returned by the \c rd_kafka_consume*() family
 *        of functions as well as provided to the Producer \c dr_msg_cb().
 *
 * For the consumer this object has two purposes:
 *  - provide the application with a consumed message. (\c err == 0)
 *  - report per-topic+partition consumer errors (\c err != 0)
 *
 * The application must check \c err to decide what action to take.
 *
 * When the application is finished with a message it must call
 * rd_kafka_message_destroy() unless otherwise noted.
 */
typedef struct rd_kafka_message_s {
        rd_kafka_resp_err_t err; /**< Non-zero for error signaling. */
        rd_kafka_topic_t *rkt;   /**< Topic */
        int32_t partition;       /**< Partition */
        void *payload;           /**< Producer: original message payload.
                                  * Consumer: Depends on the value of \c err :
                                  * - \c err==0: Message payload.
                                  * - \c err!=0: Error string */
        size_t len;              /**< Depends on the value of \c err :
                                  * - \c err==0: Message payload length
                                  * - \c err!=0: Error string length */
        void *key;               /**< Depends on the value of \c err :
                                  * - \c err==0: Optional message key */
        size_t key_len;          /**< Depends on the value of \c err :
                                  * - \c err==0: Optional message key length*/
        int64_t offset;          /**< Consumer:
                                  * - Message offset (or offset for error
                                  *   if \c err!=0 if applicable).
                                  *   Producer, dr_msg_cb:
                                  *   Message offset assigned by broker.
                                  *   May be RD_KAFKA_OFFSET_INVALID
                                  *   for retried messages when
                                  *   idempotence is enabled. */
        void *_private;          /**< Consumer:
                                  *  - rdkafka private pointer: DO NOT MODIFY
                                  *  Producer:
                                  *  - dr_msg_cb:
                                  *    msg_opaque from produce() call or
                                  *    RD_KAFKA_V_OPAQUE from producev(). */
} rd_kafka_message_t;


/**
 * @brief Frees resources for \p rkmessage and hands ownership back to rdkafka.
 */
RD_EXPORT
void rd_kafka_message_destroy(rd_kafka_message_t *rkmessage);



/**
 * @brief Returns the error string for an errored rd_kafka_message_t or NULL if
 *        there was no error.
 *
 * @remark This function MUST NOT be used with the producer.
 */
RD_EXPORT
const char *rd_kafka_message_errstr(const rd_kafka_message_t *rkmessage);


/**
 * @brief Returns the message timestamp for a consumed message.
 *
 * The timestamp is the number of milliseconds since the epoch (UTC).
 *
 * \p tstype (if not NULL) is updated to indicate the type of timestamp.
 *
 * @returns message timestamp, or -1 if not available.
 *
 * @remark Message timestamps require broker version 0.10.0 or later.
 */
RD_EXPORT
int64_t rd_kafka_message_timestamp(const rd_kafka_message_t *rkmessage,
                                   rd_kafka_timestamp_type_t *tstype);



/**
 * @brief Returns the latency for a produced message measured from
 *        the produce() call.
 *
 * @returns the latency in microseconds, or -1 if not available.
 */
RD_EXPORT
int64_t rd_kafka_message_latency(const rd_kafka_message_t *rkmessage);


/**
 * @brief Returns the broker id of the broker the message was produced to
 *        or fetched from.
 *
 * @returns a broker id if known, else -1.
 */
RD_EXPORT
int32_t rd_kafka_message_broker_id(const rd_kafka_message_t *rkmessage);


/**
 * @brief Get the message header list.
 *
 * The returned pointer in \p *hdrsp is associated with the \p rkmessage and
 * must not be used after destruction of the message object or the header
 * list is replaced with rd_kafka_message_set_headers().
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if headers were returned,
 *          RD_KAFKA_RESP_ERR__NOENT if the message has no headers,
 *          or another error code if the headers could not be parsed.
 *
 * @remark Headers require broker version 0.11.0.0 or later.
 *
 * @remark As an optimization the raw protocol headers are parsed on
 *         the first call to this function.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_message_headers(const rd_kafka_message_t *rkmessage,
                         rd_kafka_headers_t **hdrsp);

/**
 * @brief Get the message header list and detach the list from the message
 *        making the application the owner of the headers.
 *        The application must eventually destroy the headers using
 *        rd_kafka_headers_destroy().
 *        The message's headers will be set to NULL.
 *
 *        Otherwise same semantics as rd_kafka_message_headers()
 *
 * @sa rd_kafka_message_headers
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_message_detach_headers(rd_kafka_message_t *rkmessage,
                                rd_kafka_headers_t **hdrsp);


/**
 * @brief Replace the message's current headers with a new list.
 *
 * @param rkmessage The message to set headers.
 * @param hdrs New header list. The message object assumes ownership of
 *             the list, the list will be destroyed automatically with
 *             the message object.
 *             The new headers list may be updated until the message object
 *             is passed or returned to librdkafka.
 *
 * @remark The existing headers object, if any, will be destroyed.
 */
RD_EXPORT
void rd_kafka_message_set_headers(rd_kafka_message_t *rkmessage,
                                  rd_kafka_headers_t *hdrs);


/**
 * @brief Returns the number of header key/value pairs
 *
 * @param hdrs   Headers to count
 */
RD_EXPORT size_t rd_kafka_header_cnt(const rd_kafka_headers_t *hdrs);


/**
 * @enum rd_kafka_msg_status_t
 * @brief Message persistence status can be used by the application to
 *        find out if a produced message was persisted in the topic log.
 */
typedef enum {
        /** Message was never transmitted to the broker, or failed with
         *  an error indicating it was not written to the log.
         *  Application retry risks ordering, but not duplication. */
        RD_KAFKA_MSG_STATUS_NOT_PERSISTED = 0,

        /** Message was transmitted to broker, but no acknowledgement was
         *  received.
         *  Application retry risks ordering and duplication. */
        RD_KAFKA_MSG_STATUS_POSSIBLY_PERSISTED = 1,

        /** Message was written to the log and acknowledged by the broker.
         *  No reason for application to retry.
         *  Note: this value should only be trusted with \c acks=all. */
        RD_KAFKA_MSG_STATUS_PERSISTED = 2
} rd_kafka_msg_status_t;


/**
 * @brief Returns the message's persistence status in the topic log.
 *
 * @remark The message status is not available in on_acknowledgement
 *         interceptors.
 */
RD_EXPORT rd_kafka_msg_status_t
rd_kafka_message_status(const rd_kafka_message_t *rkmessage);

/**@}*/


/**
 * @name Configuration interface
 * @{
 *
 * @brief Main/global configuration property interface
 *
 */

/**
 * @enum rd_kafka_conf_res_t
 * @brief Configuration result type
 */
typedef enum {
        RD_KAFKA_CONF_UNKNOWN = -2, /**< Unknown configuration name. */
        RD_KAFKA_CONF_INVALID = -1, /**< Invalid configuration value or
                                     *   property or value not supported in
                                     *   this build. */
        RD_KAFKA_CONF_OK = 0        /**< Configuration okay */
} rd_kafka_conf_res_t;


/**
 * @brief Create configuration object.
 *
 * When providing your own configuration to the \c rd_kafka_*_new_*() calls
 * the rd_kafka_conf_t objects needs to be created with this function
 * which will set up the defaults.
 * I.e.:
 * @code
 *   rd_kafka_conf_t *myconf;
 *   rd_kafka_conf_res_t res;
 *
 *   myconf = rd_kafka_conf_new();
 *   res = rd_kafka_conf_set(myconf, "socket.timeout.ms", "600",
 *                           errstr, sizeof(errstr));
 *   if (res != RD_KAFKA_CONF_OK)
 *      die("%s\n", errstr);
 *
 *   rk = rd_kafka_new(..., myconf);
 * @endcode
 *
 * Please see CONFIGURATION.md for the default settings or use
 * rd_kafka_conf_properties_show() to provide the information at runtime.
 *
 * The properties are identical to the Apache Kafka configuration properties
 * whenever possible.
 *
 * @remark A successful call to rd_kafka_new() will assume ownership of
 * the conf object and rd_kafka_conf_destroy() must not be called.
 *
 * @returns A new rd_kafka_conf_t object with defaults set.
 *
 * @sa rd_kafka_new(), rd_kafka_conf_set(), rd_kafka_conf_destroy()
 */
RD_EXPORT
rd_kafka_conf_t *rd_kafka_conf_new(void);


/**
 * @brief Destroys a conf object.
 */
RD_EXPORT
void rd_kafka_conf_destroy(rd_kafka_conf_t *conf);


/**
 * @brief Creates a copy/duplicate of configuration object \p conf
 *
 * @remark Interceptors are NOT copied to the new configuration object.
 * @sa rd_kafka_interceptor_f_on_conf_dup
 */
RD_EXPORT
rd_kafka_conf_t *rd_kafka_conf_dup(const rd_kafka_conf_t *conf);


/**
 * @brief Same as rd_kafka_conf_dup() but with an array of property name
 *        prefixes to filter out (ignore) when copying.
 */
RD_EXPORT
rd_kafka_conf_t *rd_kafka_conf_dup_filter(const rd_kafka_conf_t *conf,
                                          size_t filter_cnt,
                                          const char **filter);



/**
 * @returns the configuration object used by an rd_kafka_t instance.
 *          For use with rd_kafka_conf_get(), et.al., to extract configuration
 *          properties from a running client.
 *
 * @remark the returned object is read-only and its lifetime is the same
 *         as the rd_kafka_t object.
 */
RD_EXPORT
const rd_kafka_conf_t *rd_kafka_conf(rd_kafka_t *rk);


/**
 * @brief Sets a configuration property.
 *
 * \p conf must have been previously created with rd_kafka_conf_new().
 *
 * Fallthrough:
 * Topic-level configuration properties may be set using this interface
 * in which case they are applied on the \c default_topic_conf.
 * If no \c default_topic_conf has been set one will be created.
 * Any subsequent rd_kafka_conf_set_default_topic_conf() calls will
 * replace the current default topic configuration.
 *
 * @returns \c rd_kafka_conf_res_t to indicate success or failure.
 * In case of failure \p errstr is updated to contain a human readable
 * error string.
 *
 * @remark Setting properties or values that were disabled at build time due to
 *         missing dependencies will return RD_KAFKA_CONF_INVALID.
 */
RD_EXPORT
rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t *conf,
                                      const char *name,
                                      const char *value,
                                      char *errstr,
                                      size_t errstr_size);


/**
 * @brief Enable event sourcing.
 * \p events is a bitmask of \c RD_KAFKA_EVENT_* of events to enable
 * for consumption by `rd_kafka_queue_poll()`.
 */
RD_EXPORT
void rd_kafka_conf_set_events(rd_kafka_conf_t *conf, int events);


/**
 * @brief Generic event callback to be used with the event API to trigger
 *        callbacks for \c rd_kafka_event_t objects from a background
 *        thread serving the background queue.
 *
 * How to use:
 *  1. First set the event callback on the configuration object with this
 *     function, followed by creating an rd_kafka_t instance
 *     with rd_kafka_new().
 *  2. Get the instance's background queue with rd_kafka_queue_get_background()
 *     and pass it as the reply/response queue to an API that takes an
 *     event queue, such as rd_kafka_CreateTopics().
 *  3. As the response event is ready and enqueued on the background queue the
 *     event callback will be triggered from the background thread.
 *  4. Prior to destroying the client instance, loose your reference to the
 *     background queue by calling rd_kafka_queue_destroy().
 *
 * The application must destroy the \c rkev passed to \p event cb using
 * rd_kafka_event_destroy().
 *
 * The \p event_cb \c opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark This callback is a specialized alternative to the poll-based
 *         event API described in the Event interface section.
 *
 * @remark The \p event_cb will be called spontaneously from a background
 *         thread completely managed by librdkafka.
 *         Take care to perform proper locking of application objects.
 *
 * @warning The application MUST NOT call rd_kafka_destroy() from the
 *          event callback.
 *
 * @sa rd_kafka_queue_get_background
 */
RD_EXPORT void rd_kafka_conf_set_background_event_cb(
    rd_kafka_conf_t *conf,
    void (*event_cb)(rd_kafka_t *rk, rd_kafka_event_t *rkev, void *opaque));


/**
 * @deprecated See rd_kafka_conf_set_dr_msg_cb()
 */
RD_EXPORT
void rd_kafka_conf_set_dr_cb(rd_kafka_conf_t *conf,
                             void (*dr_cb)(rd_kafka_t *rk,
                                           void *payload,
                                           size_t len,
                                           rd_kafka_resp_err_t err,
                                           void *opaque,
                                           void *msg_opaque));

/**
 * @brief \b Producer: Set delivery report callback in provided \p conf object.
 *
 * The delivery report callback will be called once for each message
 * accepted by rd_kafka_produce() (et.al) with \p err set to indicate
 * the result of the produce request.
 *
 * The callback is called when a message is succesfully produced or
 * if librdkafka encountered a permanent failure.
 * Delivery errors occur when the retry count is exceeded, when the
 * message.timeout.ms timeout is exceeded or there is a permanent error
 * like RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART.
 *
 * An application must call rd_kafka_poll() at regular intervals to
 * serve queued delivery report callbacks.
 *
 * The broker-assigned offset can be retrieved with \c rkmessage->offset
 * and the timestamp can be retrieved using rd_kafka_message_timestamp().
 *
 * The \p dr_msg_cb \c opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 * The per-message msg_opaque value is available in
 * \c rd_kafka_message_t._private.
 *
 * @remark The Idempotent Producer may return invalid timestamp
 *         (RD_KAFKA_TIMESTAMP_NOT_AVAILABLE), and
 *         and offset (RD_KAFKA_OFFSET_INVALID) for retried messages
 *         that were previously successfully delivered but not properly
 *         acknowledged.
 */
RD_EXPORT
void rd_kafka_conf_set_dr_msg_cb(
    rd_kafka_conf_t *conf,
    void (*dr_msg_cb)(rd_kafka_t *rk,
                      const rd_kafka_message_t *rkmessage,
                      void *opaque));


/**
 * @brief \b Consumer: Set consume callback for use with
 *        rd_kafka_consumer_poll()
 *
 * The \p consume_cb \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 */
RD_EXPORT
void rd_kafka_conf_set_consume_cb(
    rd_kafka_conf_t *conf,
    void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque));

/**
 * @brief \b Consumer: Set rebalance callback for use with
 *                     coordinated consumer group balancing.
 *
 * The \p err field is set to either RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS
 * or RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS and 'partitions'
 * contains the full partition set that was either assigned or revoked.
 *
 * Registering a \p rebalance_cb turns off librdkafka's automatic
 * partition assignment/revocation and instead delegates that responsibility
 * to the application's \p rebalance_cb.
 *
 * The rebalance callback is responsible for updating librdkafka's
 * assignment set based on the two events: RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS
 * and RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS but should also be able to handle
 * arbitrary rebalancing failures where \p err is neither of those.
 * @remark In this latter case (arbitrary error), the application must
 *         call rd_kafka_assign(rk, NULL) to synchronize state.
 *
 * For eager/non-cooperative `partition.assignment.strategy` assignors,
 * such as `range` and `roundrobin`, the application must use
 * rd_kafka_assign() to set or clear the entire assignment.
 * For the cooperative assignors, such as `cooperative-sticky`, the application
 * must use rd_kafka_incremental_assign() for
 * RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS and rd_kafka_incremental_unassign()
 * for RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS.
 *
 * Without a rebalance callback this is done automatically by librdkafka
 * but registering a rebalance callback gives the application flexibility
 * in performing other operations along with the assigning/revocation,
 * such as fetching offsets from an alternate location (on assign)
 * or manually committing offsets (on revoke).
 *
 * rebalance_cb is always triggered exactly once when a rebalance completes
 * with a new assignment, even if that assignment is empty. If an
 * eager/non-cooperative assignor is configured, there will eventually be
 * exactly one corresponding call to rebalance_cb to revoke these partitions
 * (even if empty), whether this is due to a group rebalance or lost
 * partitions. In the cooperative case, rebalance_cb will never be called if
 * the set of partitions being revoked is empty (whether or not lost).
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark The \p partitions list is destroyed by librdkafka on return
 *         return from the rebalance_cb and must not be freed or
 *         saved by the application.
 *
 * @remark Be careful when modifying the \p partitions list.
 *         Changing this list should only be done to change the initial
 *         offsets for each partition.
 *         But a function like `rd_kafka_position()` might have unexpected
 *         effects for instance when a consumer gets assigned a partition
 *         it used to consume at an earlier rebalance. In this case, the
 *         list of partitions will be updated with the old offset for that
 *         partition. In this case, it is generally better to pass a copy
 *         of the list (see `rd_kafka_topic_partition_list_copy()`).
 *         The result of `rd_kafka_position()` is typically outdated in
 *         RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS.
 *
 * @sa rd_kafka_assign()
 * @sa rd_kafka_incremental_assign()
 * @sa rd_kafka_incremental_unassign()
 * @sa rd_kafka_assignment_lost()
 * @sa rd_kafka_rebalance_protocol()
 *
 * The following example shows the application's responsibilities:
 * @code
 *    static void rebalance_cb (rd_kafka_t *rk, rd_kafka_resp_err_t err,
 *                              rd_kafka_topic_partition_list_t *partitions,
 *                              void *opaque) {
 *
 *        switch (err)
 *        {
 *          case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
 *             // application may load offets from arbitrary external
 *             // storage here and update \p partitions
 *             if (!strcmp(rd_kafka_rebalance_protocol(rk), "COOPERATIVE"))
 *                     rd_kafka_incremental_assign(rk, partitions);
 *             else // EAGER
 *                     rd_kafka_assign(rk, partitions);
 *             break;
 *
 *          case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
 *             if (manual_commits) // Optional explicit manual commit
 *                 rd_kafka_commit(rk, partitions, 0); // sync commit
 *
 *             if (!strcmp(rd_kafka_rebalance_protocol(rk), "COOPERATIVE"))
 *                     rd_kafka_incremental_unassign(rk, partitions);
 *             else // EAGER
 *                     rd_kafka_assign(rk, NULL);
 *             break;
 *
 *          default:
 *             handle_unlikely_error(err);
 *             rd_kafka_assign(rk, NULL); // sync state
 *             break;
 *         }
 *    }
 * @endcode
 *
 * @remark The above example lacks error handling for assign calls, see
 *         the examples/ directory.
 */
RD_EXPORT
void rd_kafka_conf_set_rebalance_cb(
    rd_kafka_conf_t *conf,
    void (*rebalance_cb)(rd_kafka_t *rk,
                         rd_kafka_resp_err_t err,
                         rd_kafka_topic_partition_list_t *partitions,
                         void *opaque));



/**
 * @brief \b Consumer: Set offset commit callback for use with consumer groups.
 *
 * The results of automatic or manual offset commits will be scheduled
 * for this callback and is served by rd_kafka_consumer_poll().
 *
 * If no partitions had valid offsets to commit this callback will be called
 * with \p err == RD_KAFKA_RESP_ERR__NO_OFFSET which is not to be considered
 * an error.
 *
 * The \p offsets list contains per-partition information:
 *   - \c offset: committed offset (attempted)
 *   - \c err:    commit error
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 */
RD_EXPORT
void rd_kafka_conf_set_offset_commit_cb(
    rd_kafka_conf_t *conf,
    void (*offset_commit_cb)(rd_kafka_t *rk,
                             rd_kafka_resp_err_t err,
                             rd_kafka_topic_partition_list_t *offsets,
                             void *opaque));


/**
 * @brief Set error callback in provided conf object.
 *
 * The error callback is used by librdkafka to signal warnings and errors
 * back to the application.
 *
 * These errors should generally be considered informational and non-permanent,
 * the client will try to recover automatically from all type of errors.
 * Given that the client and cluster configuration is correct the
 * application should treat these as temporary errors.
 *
 * \p error_cb will be triggered with \c err set to RD_KAFKA_RESP_ERR__FATAL
 * if a fatal error has been raised; in this case use rd_kafka_fatal_error() to
 * retrieve the fatal error code and error string, and then begin terminating
 * the client instance.
 *
 * If no \p error_cb is registered, or RD_KAFKA_EVENT_ERROR has not been set
 * with rd_kafka_conf_set_events, then the errors will be logged instead.
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 */
RD_EXPORT
void rd_kafka_conf_set_error_cb(rd_kafka_conf_t *conf,
                                void (*error_cb)(rd_kafka_t *rk,
                                                 int err,
                                                 const char *reason,
                                                 void *opaque));

/**
 * @brief Set throttle callback.
 *
 * The throttle callback is used to forward broker throttle times to the
 * application for Produce and Fetch (consume) requests.
 *
 * Callbacks are triggered whenever a non-zero throttle time is returned by
 * the broker, or when the throttle time drops back to zero.
 *
 * An application must call rd_kafka_poll() or rd_kafka_consumer_poll() at
 * regular intervals to serve queued callbacks.
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark Requires broker version 0.9.0 or later.
 */
RD_EXPORT
void rd_kafka_conf_set_throttle_cb(rd_kafka_conf_t *conf,
                                   void (*throttle_cb)(rd_kafka_t *rk,
                                                       const char *broker_name,
                                                       int32_t broker_id,
                                                       int throttle_time_ms,
                                                       void *opaque));


/**
 * @brief Set logger callback.
 *
 * The default is to print to stderr, but a syslog logger is also available,
 * see rd_kafka_log_print and rd_kafka_log_syslog for the builtin alternatives.
 * Alternatively the application may provide its own logger callback.
 * Or pass \p func as NULL to disable logging.
 *
 * This is the configuration alternative to the deprecated rd_kafka_set_logger()
 *
 * @remark The log_cb will be called spontaneously from librdkafka's internal
 *         threads unless logs have been forwarded to a poll queue through
 *         \c rd_kafka_set_log_queue().
 *         An application MUST NOT call any librdkafka APIs or do any prolonged
 *         work in a non-forwarded \c log_cb.
 */
RD_EXPORT
void rd_kafka_conf_set_log_cb(rd_kafka_conf_t *conf,
                              void (*log_cb)(const rd_kafka_t *rk,
                                             int level,
                                             const char *fac,
                                             const char *buf));


/**
 * @brief Set statistics callback in provided conf object.
 *
 * The statistics callback is triggered from rd_kafka_poll() every
 * \c statistics.interval.ms (needs to be configured separately).
 * Function arguments:
 *   - \p rk - Kafka handle
 *   - \p json - String containing the statistics data in JSON format
 *   - \p json_len - Length of \p json string.
 *   - \p opaque - Application-provided opaque as set by
 *                 rd_kafka_conf_set_opaque().
 *
 * For more information on the format of \p json, see
 * https://github.com/edenhill/librdkafka/wiki/Statistics
 *
 * If the application wishes to hold on to the \p json pointer and free
 * it at a later time it must return 1 from the \p stats_cb.
 * If the application returns 0 from the \p stats_cb then librdkafka
 * will immediately free the \p json pointer.
 *
 * See STATISTICS.md for a full definition of the JSON object.
 */
RD_EXPORT
void rd_kafka_conf_set_stats_cb(
    rd_kafka_conf_t *conf,
    int (*stats_cb)(rd_kafka_t *rk, char *json, size_t json_len, void *opaque));

/**
 * @brief Set SASL/OAUTHBEARER token refresh callback in provided conf object.
 *
 * @param conf the configuration to mutate.
 * @param oauthbearer_token_refresh_cb the callback to set; callback function
 *  arguments:<br>
 *   \p rk - Kafka handle<br>
 *   \p oauthbearer_config - Value of configuration property
 *                           sasl.oauthbearer.config.
 *   \p opaque - Application-provided opaque set via
 *               rd_kafka_conf_set_opaque()
 *
 * The SASL/OAUTHBEARER token refresh callback is triggered via rd_kafka_poll()
 * whenever OAUTHBEARER is the SASL mechanism and a token needs to be retrieved,
 * typically based on the configuration defined in \c sasl.oauthbearer.config.
 *
 * The callback should invoke rd_kafka_oauthbearer_set_token()
 * or rd_kafka_oauthbearer_set_token_failure() to indicate success
 * or failure, respectively.
 *
 * The refresh operation is eventable and may be received via
 * rd_kafka_queue_poll() with an event type of
 * \c RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH.
 *
 * Note that before any SASL/OAUTHBEARER broker connection can succeed the
 * application must call rd_kafka_oauthbearer_set_token() once -- either
 * directly or, more typically, by invoking either rd_kafka_poll(),
 * rd_kafka_consumer_poll(), rd_kafka_queue_poll(), etc, in order to cause
 * retrieval of an initial token to occur.
 *
 * Alternatively, the application can enable the SASL queue by calling
 * rd_kafka_conf_enable_sasl_queue() on the configuration object prior to
 * creating the client instance, get the SASL queue with
 * rd_kafka_queue_get_sasl(), and either serve the queue manually by calling
 * rd_kafka_queue_poll(), or redirecting the queue to the background thread to
 * have the queue served automatically. For the latter case the SASL queue
 * must be forwarded to the background queue with rd_kafka_queue_forward().
 * A convenience function is available to automatically forward the SASL queue
 * to librdkafka's background thread, see
 * rd_kafka_sasl_background_callbacks_enable().
 *
 * An unsecured JWT refresh handler is provided by librdkafka for development
 * and testing purposes, it is enabled by setting
 * the \c enable.sasl.oauthbearer.unsecure.jwt property to true and is
 * mutually exclusive to using a refresh callback.
 *
 * @sa rd_kafka_sasl_background_callbacks_enable()
 * @sa rd_kafka_queue_get_sasl()
 */
RD_EXPORT
void rd_kafka_conf_set_oauthbearer_token_refresh_cb(
    rd_kafka_conf_t *conf,
    void (*oauthbearer_token_refresh_cb)(rd_kafka_t *rk,
                                         const char *oauthbearer_config,
                                         void *opaque));

/**
 * @brief Enable/disable creation of a queue specific to SASL events
 *        and callbacks.
 *
 * For SASL mechanisms that trigger callbacks (currently OAUTHBEARER) this
 * configuration API allows an application to get a dedicated
 * queue for the SASL events/callbacks. After enabling the queue with this API
 * the application can retrieve the queue by calling
 * rd_kafka_queue_get_sasl() on the client instance.
 * This queue may then be served directly by the application
 * (with rd_kafka_queue_poll(), et.al)  or forwarded to another queue, such as
 * the background queue.
 *
 * A convenience function is available to automatically forward the SASL queue
 * to librdkafka's background thread, see
 * rd_kafka_sasl_background_callbacks_enable().
 *
 * By default (\p enable = 0) the main queue (as served by rd_kafka_poll(),
 * et.al.) is used for SASL callbacks.
 *
 * @remark The SASL queue is currently only used by the SASL OAUTHBEARER
 *         mechanism's token_refresh_cb().
 *
 * @sa rd_kafka_queue_get_sasl()
 * @sa rd_kafka_sasl_background_callbacks_enable()
 */

RD_EXPORT
void rd_kafka_conf_enable_sasl_queue(rd_kafka_conf_t *conf, int enable);


/**
 * @brief Set socket callback.
 *
 * The socket callback is responsible for opening a socket
 * according to the supplied \p domain, \p type and \p protocol.
 * The socket shall be created with \c CLOEXEC set in a racefree fashion, if
 * possible.
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * Default:
 *  - on linux: racefree CLOEXEC
 *  - others  : non-racefree CLOEXEC
 *
 * @remark The callback will be called from an internal librdkafka thread.
 */
RD_EXPORT
void rd_kafka_conf_set_socket_cb(
    rd_kafka_conf_t *conf,
    int (*socket_cb)(int domain, int type, int protocol, void *opaque));



/**
 * @brief Set connect callback.
 *
 * The connect callback is responsible for connecting socket \p sockfd
 * to peer address \p addr.
 * The \p id field contains the broker identifier.
 *
 * \p connect_cb shall return 0 on success (socket connected) or an error
 * number (errno) on error.
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark The callback will be called from an internal librdkafka thread.
 */
RD_EXPORT void
rd_kafka_conf_set_connect_cb(rd_kafka_conf_t *conf,
                             int (*connect_cb)(int sockfd,
                                               const struct sockaddr *addr,
                                               int addrlen,
                                               const char *id,
                                               void *opaque));

/**
 * @brief Set close socket callback.
 *
 * Close a socket (optionally opened with socket_cb()).
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark The callback will be called from an internal librdkafka thread.
 */
RD_EXPORT void rd_kafka_conf_set_closesocket_cb(
    rd_kafka_conf_t *conf,
    int (*closesocket_cb)(int sockfd, void *opaque));



#ifndef _WIN32
/**
 * @brief Set open callback.
 *
 * The open callback is responsible for opening the file specified by
 * pathname, flags and mode.
 * The file shall be opened with \c CLOEXEC set in a racefree fashion, if
 * possible.
 *
 * Default:
 *  - on linux: racefree CLOEXEC
 *  - others  : non-racefree CLOEXEC
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @remark The callback will be called from an internal librdkafka thread.
 */
RD_EXPORT
void rd_kafka_conf_set_open_cb(
    rd_kafka_conf_t *conf,
    int (*open_cb)(const char *pathname, int flags, mode_t mode, void *opaque));
#endif

/** Forward declaration to avoid netdb.h or winsock includes */
struct addrinfo;

/**
 * @brief Set address resolution callback.
 *
 * The callback is responsible for resolving the hostname \p node and the
 * service \p service into a list of socket addresses as \c getaddrinfo(3)
 * would. The \p hints and \p res parameters function as they do for
 * \c getaddrinfo(3). The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * If the callback is invoked with a NULL \p node, \p service, and \p hints, the
 * callback should instead free the addrinfo struct specified in \p res. In this
 * case the callback must succeed; the return value will not be checked by the
 * caller.
 *
 * The callback's return value is interpreted as the return value of \p
 * \c getaddrinfo(3).
 *
 * @remark The callback will be called from an internal librdkafka thread.
 */
RD_EXPORT void
rd_kafka_conf_set_resolve_cb(rd_kafka_conf_t *conf,
                             int (*resolve_cb)(const char *node,
                                               const char *service,
                                               const struct addrinfo *hints,
                                               struct addrinfo **res,
                                               void *opaque));

/**
 * @brief Sets the verification callback of the broker certificate
 *
 * The verification callback is triggered from internal librdkafka threads
 * upon connecting to a broker. On each connection attempt the callback
 * will be called for each certificate in the broker's certificate chain,
 * starting at the root certification, as long as the application callback
 * returns 1 (valid certificate).
 * \c broker_name and \c broker_id correspond to the broker the connection
 * is being made to.
 * The \c x509_error argument indicates if OpenSSL's verification of
 * the certificate succeed (0) or failed (an OpenSSL error code).
 * The application may set the SSL context error code by returning 0
 * from the verify callback and providing a non-zero SSL context error code
 * in \c x509_error.
 * If the verify callback sets \c x509_error to 0, returns 1, and the
 * original \c x509_error was non-zero, the error on the SSL context will
 * be cleared.
 * \c x509_error is always a valid pointer to an int.
 *
 * \c depth is the depth of the current certificate in the chain, starting
 * at the root certificate.
 *
 * The certificate itself is passed in binary DER format in \c buf of
 * size \c size.
 *
 * The callback must return 1 if verification succeeds, or
 * 0 if verification fails and then write a human-readable error message
 * to \c errstr (limited to \c errstr_size bytes, including nul-term).
 *
 * The callback's \p opaque argument is the opaque set with
 * rd_kafka_conf_set_opaque().
 *
 * @returns RD_KAFKA_CONF_OK if SSL is supported in this build, else
 *          RD_KAFKA_CONF_INVALID.
 *
 * @warning This callback will be called from internal librdkafka threads.
 *
 * @remark See <openssl/x509_vfy.h> in the OpenSSL source distribution
 *         for a list of \p x509_error codes.
 */
RD_EXPORT
rd_kafka_conf_res_t rd_kafka_conf_set_ssl_cert_verify_cb(
    rd_kafka_conf_t *conf,
    int (*ssl_cert_verify_cb)(rd_kafka_t *rk,
                              const char *broker_name,
                              int32_t broker_id,
                              int *x509_error,
                              int depth,
                              const char *buf,
                              size_t size,
                              char *errstr,
                              size_t errstr_size,
                              void *opaque));


/**
 * @enum rd_kafka_cert_type_t
 *
 * @brief SSL certificate type
 *
 * @sa rd_kafka_conf_set_ssl_cert
 */
typedef enum rd_kafka_cert_type_t {
        RD_KAFKA_CERT_PUBLIC_KEY,  /**< Client's public key */
        RD_KAFKA_CERT_PRIVATE_KEY, /**< Client's private key */
        RD_KAFKA_CERT_CA,          /**< CA certificate */
        RD_KAFKA_CERT__CNT,
} rd_kafka_cert_type_t;

/**
 * @enum rd_kafka_cert_enc_t
 *
 * @brief SSL certificate encoding
 *
 * @sa rd_kafka_conf_set_ssl_cert
 */
typedef enum rd_kafka_cert_enc_t {
        RD_KAFKA_CERT_ENC_PKCS12, /**< PKCS#12 */
        RD_KAFKA_CERT_ENC_DER,    /**< DER / binary X.509 ASN1 */
        RD_KAFKA_CERT_ENC_PEM,    /**< PEM */
        RD_KAFKA_CERT_ENC__CNT,
} rd_kafka_cert_enc_t;


/**
 * @brief Set certificate/key \p cert_type from the \p cert_enc encoded
 *        memory at \p buffer of \p size bytes.
 *
 * @param conf Configuration object.
 * @param cert_type Certificate or key type to configure.
 * @param cert_enc  Buffer \p encoding type.
 * @param buffer Memory pointer to encoded certificate or key.
 *               The memory is not referenced after this function returns.
 * @param size Size of memory at \p buffer.
 * @param errstr Memory were a human-readable error string will be written
 *               on failure.
 * @param errstr_size Size of \p errstr, including space for nul-terminator.
 *
 * @returns RD_KAFKA_CONF_OK on success or RD_KAFKA_CONF_INVALID if the
 *          memory in \p buffer is of incorrect encoding, or if librdkafka
 *          was not built with SSL support.
 *
 * @remark Calling this method multiple times with the same \p cert_type
 *         will replace the previous value.
 *
 * @remark Calling this method with \p buffer set to NULL will clear the
 *         configuration for \p cert_type.
 *
 * @remark The private key may require a password, which must be specified
 *         with the `ssl.key.password` configuration property prior to
 *         calling this function.
 *
 * @remark Private and public keys in PEM format may also be set with the
 *         `ssl.key.pem` and `ssl.certificate.pem` configuration properties.
 *
 * @remark CA certificate in PEM format may also be set with the
 *         `ssl.ca.pem` configuration property.
 *
 * @remark When librdkafka is linked to OpenSSL 3.0 and the certificate is
 *         encoded using an obsolete cipher, it might be necessary to set up
 *         an OpenSSL configuration file to load the "legacy" provider and
 *         set the OPENSSL_CONF environment variable.
 *         See
 * https://github.com/openssl/openssl/blob/master/README-PROVIDERS.md for more
 * information.
 */
RD_EXPORT rd_kafka_conf_res_t
rd_kafka_conf_set_ssl_cert(rd_kafka_conf_t *conf,
                           rd_kafka_cert_type_t cert_type,
                           rd_kafka_cert_enc_t cert_enc,
                           const void *buffer,
                           size_t size,
                           char *errstr,
                           size_t errstr_size);


/**
 * @brief Set callback_data for OpenSSL engine.
 *
 * @param conf Configuration object.
 * @param callback_data passed to engine callbacks,
 *                      e.g. \c ENGINE_load_ssl_client_cert.
 *
 * @remark The \c ssl.engine.location configuration must be set for this
 *         to have affect.
 *
 * @remark The memory pointed to by \p value must remain valid for the
 *         lifetime of the configuration object and any Kafka clients that
 *         use it.
 */
RD_EXPORT
void rd_kafka_conf_set_engine_callback_data(rd_kafka_conf_t *conf,
                                            void *callback_data);


/**
 * @brief Sets the application's opaque pointer that will be passed to callbacks
 *
 * @sa rd_kafka_opaque()
 */
RD_EXPORT
void rd_kafka_conf_set_opaque(rd_kafka_conf_t *conf, void *opaque);

/**
 * @brief Retrieves the opaque pointer previously set
 *        with rd_kafka_conf_set_opaque()
 */
RD_EXPORT
void *rd_kafka_opaque(const rd_kafka_t *rk);



/**
 * @brief Sets the default topic configuration to use for automatically
 *        subscribed topics (e.g., through pattern-matched topics).
 *        The topic config object is not usable after this call.
 *
 * @warning Any topic configuration settings that have been set on the
 *          global rd_kafka_conf_t object will be overwritten by this call
 *          since the implicitly created default topic config object is
 *          replaced by the user-supplied one.
 *
 * @deprecated Set default topic level configuration on the
 *             global rd_kafka_conf_t object instead.
 */
RD_EXPORT
void rd_kafka_conf_set_default_topic_conf(rd_kafka_conf_t *conf,
                                          rd_kafka_topic_conf_t *tconf);

/**
 * @brief Gets the default topic configuration as previously set with
 *        rd_kafka_conf_set_default_topic_conf() or that was implicitly created
 *        by configuring a topic-level property on the global \p conf object.
 *
 * @returns the \p conf's default topic configuration (if any), or NULL.
 *
 * @warning The returned topic configuration object is owned by the \p conf
 *          object. It may be modified but not destroyed and its lifetime is
 *          the same as the \p conf object or the next call to
 *          rd_kafka_conf_set_default_topic_conf().
 */
RD_EXPORT rd_kafka_topic_conf_t *
rd_kafka_conf_get_default_topic_conf(rd_kafka_conf_t *conf);


/**
 * @brief Retrieve configuration value for property \p name.
 *
 * If \p dest is non-NULL the value will be written to \p dest with at
 * most \p dest_size.
 *
 * \p *dest_size is updated to the full length of the value, thus if
 * \p *dest_size initially is smaller than the full length the application
 * may reallocate \p dest to fit the returned \p *dest_size and try again.
 *
 * If \p dest is NULL only the full length of the value is returned.
 *
 * Fallthrough:
 * Topic-level configuration properties from the \c default_topic_conf
 * may be retrieved using this interface.
 *
 * @returns \p RD_KAFKA_CONF_OK if the property name matched, else
 * \p RD_KAFKA_CONF_UNKNOWN.
 */
RD_EXPORT
rd_kafka_conf_res_t rd_kafka_conf_get(const rd_kafka_conf_t *conf,
                                      const char *name,
                                      char *dest,
                                      size_t *dest_size);


/**
 * @brief Retrieve topic configuration value for property \p name.
 *
 * @sa rd_kafka_conf_get()
 */
RD_EXPORT
rd_kafka_conf_res_t rd_kafka_topic_conf_get(const rd_kafka_topic_conf_t *conf,
                                            const char *name,
                                            char *dest,
                                            size_t *dest_size);


/**
 * @brief Dump the configuration properties and values of \p conf to an array
 *        with \"key\", \"value\" pairs.
 *
 * The number of entries in the array is returned in \p *cntp.
 *
 * The dump must be freed with `rd_kafka_conf_dump_free()`.
 */
RD_EXPORT
const char **rd_kafka_conf_dump(rd_kafka_conf_t *conf, size_t *cntp);


/**
 * @brief Dump the topic configuration properties and values of \p conf
 *        to an array with \"key\", \"value\" pairs.
 *
 * The number of entries in the array is returned in \p *cntp.
 *
 * The dump must be freed with `rd_kafka_conf_dump_free()`.
 */
RD_EXPORT
const char **rd_kafka_topic_conf_dump(rd_kafka_topic_conf_t *conf,
                                      size_t *cntp);

/**
 * @brief Frees a configuration dump returned from `rd_kafka_conf_dump()` or
 *        `rd_kafka_topic_conf_dump().
 */
RD_EXPORT
void rd_kafka_conf_dump_free(const char **arr, size_t cnt);

/**
 * @brief Prints a table to \p fp of all supported configuration properties,
 *        their default values as well as a description.
 *
 * @remark All properties and properties and values are shown, even those
 *         that have been disabled at build time due to missing dependencies.
 */
RD_EXPORT
void rd_kafka_conf_properties_show(FILE *fp);

/**@}*/


/**
 * @name Topic configuration
 * @brief Topic configuration property interface
 * @{
 *
 */


/**
 * @brief Create topic configuration object
 *
 * @sa Same semantics as for rd_kafka_conf_new().
 */
RD_EXPORT
rd_kafka_topic_conf_t *rd_kafka_topic_conf_new(void);


/**
 * @brief Creates a copy/duplicate of topic configuration object \p conf.
 */
RD_EXPORT
rd_kafka_topic_conf_t *
rd_kafka_topic_conf_dup(const rd_kafka_topic_conf_t *conf);

/**
 * @brief Creates a copy/duplicate of \p rk 's default topic configuration
 *        object.
 */
RD_EXPORT
rd_kafka_topic_conf_t *rd_kafka_default_topic_conf_dup(rd_kafka_t *rk);


/**
 * @brief Destroys a topic conf object.
 */
RD_EXPORT
void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *topic_conf);


/**
 * @brief Sets a single rd_kafka_topic_conf_t value by property name.
 *
 * \p topic_conf should have been previously set up
 * with `rd_kafka_topic_conf_new()`.
 *
 * @returns rd_kafka_conf_res_t to indicate success or failure.
 */
RD_EXPORT
rd_kafka_conf_res_t rd_kafka_topic_conf_set(rd_kafka_topic_conf_t *conf,
                                            const char *name,
                                            const char *value,
                                            char *errstr,
                                            size_t errstr_size);

/**
 * @brief Sets the application's opaque pointer that will be passed to all topic
 * callbacks as the \c rkt_opaque argument.
 *
 * @sa rd_kafka_topic_opaque()
 */
RD_EXPORT
void rd_kafka_topic_conf_set_opaque(rd_kafka_topic_conf_t *conf,
                                    void *rkt_opaque);


/**
 * @brief \b Producer: Set partitioner callback in provided topic conf object.
 *
 * The partitioner may be called in any thread at any time,
 * it may be called multiple times for the same message/key.
 *
 * The callback's \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The callback's \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * Partitioner function constraints:
 *   - MUST NOT call any rd_kafka_*() functions except:
 *       rd_kafka_topic_partition_available()
 *   - MUST NOT block or execute for prolonged periods of time.
 *   - MUST return a value between 0 and partition_cnt-1, or the
 *     special \c RD_KAFKA_PARTITION_UA value if partitioning
 *     could not be performed.
 */
RD_EXPORT
void rd_kafka_topic_conf_set_partitioner_cb(
    rd_kafka_topic_conf_t *topic_conf,
    int32_t (*partitioner)(const rd_kafka_topic_t *rkt,
                           const void *keydata,
                           size_t keylen,
                           int32_t partition_cnt,
                           void *rkt_opaque,
                           void *msg_opaque));


/**
 * @brief \b Producer: Set message queueing order comparator callback.
 *
 * The callback may be called in any thread at any time,
 * it may be called multiple times for the same message.
 *
 * Ordering comparator function constraints:
 *   - MUST be stable sort (same input gives same output).
 *   - MUST NOT call any rd_kafka_*() functions.
 *   - MUST NOT block or execute for prolonged periods of time.
 *
 * The comparator shall compare the two messages and return:
 *  - < 0 if message \p a should be inserted before message \p b.
 *  - >=0 if message \p a should be inserted after message \p b.
 *
 * @remark Insert sorting will be used to enqueue the message in the
 *         correct queue position, this comes at a cost of O(n).
 *
 * @remark If `queuing.strategy=fifo` new messages are enqueued to the
 *         tail of the queue regardless of msg_order_cmp, but retried messages
 *         are still affected by msg_order_cmp.
 *
 * @warning THIS IS AN EXPERIMENTAL API, SUBJECT TO CHANGE OR REMOVAL,
 *          DO NOT USE IN PRODUCTION.
 */
RD_EXPORT void rd_kafka_topic_conf_set_msg_order_cmp(
    rd_kafka_topic_conf_t *topic_conf,
    int (*msg_order_cmp)(const rd_kafka_message_t *a,
                         const rd_kafka_message_t *b));


/**
 * @brief Check if partition is available (has a leader broker).
 *
 * @returns 1 if the partition is available, else 0.
 *
 * @warning This function must only be called from inside a partitioner function
 */
RD_EXPORT
int rd_kafka_topic_partition_available(const rd_kafka_topic_t *rkt,
                                       int32_t partition);


/*******************************************************************
 *                                                                   *
 * Partitioners provided by rdkafka                                *
 *                                                                   *
 *******************************************************************/

/**
 * @brief Random partitioner.
 *
 * Will try not to return unavailable partitions.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a random partition between 0 and \p partition_cnt - 1.
 *
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_random(const rd_kafka_topic_t *rkt,
                                        const void *key,
                                        size_t keylen,
                                        int32_t partition_cnt,
                                        void *rkt_opaque,
                                        void *msg_opaque);

/**
 * @brief Consistent partitioner.
 *
 * Uses consistent hashing to map identical keys onto identical partitions.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a \"random\" partition between 0 and \p partition_cnt - 1 based on
 *          the CRC value of the key
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_consistent(const rd_kafka_topic_t *rkt,
                                            const void *key,
                                            size_t keylen,
                                            int32_t partition_cnt,
                                            void *rkt_opaque,
                                            void *msg_opaque);

/**
 * @brief Consistent-Random partitioner.
 *
 * This is the default partitioner.
 * Uses consistent hashing to map identical keys onto identical partitions, and
 * messages without keys will be assigned via the random partitioner.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a \"random\" partition between 0 and \p partition_cnt - 1 based on
 *          the CRC value of the key (if provided)
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_consistent_random(const rd_kafka_topic_t *rkt,
                                                   const void *key,
                                                   size_t keylen,
                                                   int32_t partition_cnt,
                                                   void *rkt_opaque,
                                                   void *msg_opaque);


/**
 * @brief Murmur2 partitioner (Java compatible).
 *
 * Uses consistent hashing to map identical keys onto identical partitions
 * using Java-compatible Murmur2 hashing.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a partition between 0 and \p partition_cnt - 1.
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_murmur2(const rd_kafka_topic_t *rkt,
                                         const void *key,
                                         size_t keylen,
                                         int32_t partition_cnt,
                                         void *rkt_opaque,
                                         void *msg_opaque);

/**
 * @brief Consistent-Random Murmur2 partitioner (Java compatible).
 *
 * Uses consistent hashing to map identical keys onto identical partitions
 * using Java-compatible Murmur2 hashing.
 * Messages without keys will be assigned via the random partitioner.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a partition between 0 and \p partition_cnt - 1.
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_murmur2_random(const rd_kafka_topic_t *rkt,
                                                const void *key,
                                                size_t keylen,
                                                int32_t partition_cnt,
                                                void *rkt_opaque,
                                                void *msg_opaque);


/**
 * @brief FNV-1a partitioner.
 *
 * Uses consistent hashing to map identical keys onto identical partitions
 * using FNV-1a hashing.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a partition between 0 and \p partition_cnt - 1.
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_fnv1a(const rd_kafka_topic_t *rkt,
                                       const void *key,
                                       size_t keylen,
                                       int32_t partition_cnt,
                                       void *rkt_opaque,
                                       void *msg_opaque);


/**
 * @brief Consistent-Random FNV-1a partitioner.
 *
 * Uses consistent hashing to map identical keys onto identical partitions
 * using FNV-1a hashing.
 * Messages without keys will be assigned via the random partitioner.
 *
 * The \p rkt_opaque argument is the opaque set by
 * rd_kafka_topic_conf_set_opaque().
 * The \p msg_opaque argument is the per-message opaque
 * passed to produce().
 *
 * @returns a partition between 0 and \p partition_cnt - 1.
 */
RD_EXPORT
int32_t rd_kafka_msg_partitioner_fnv1a_random(const rd_kafka_topic_t *rkt,
                                              const void *key,
                                              size_t keylen,
                                              int32_t partition_cnt,
                                              void *rkt_opaque,
                                              void *msg_opaque);


/**@}*/



/**
 * @name Main Kafka and Topic object handles
 * @{
 *
 *
 */



/**
 * @brief Creates a new Kafka handle and starts its operation according to the
 *        specified \p type (\p RD_KAFKA_CONSUMER or \p RD_KAFKA_PRODUCER).
 *
 * \p conf is an optional struct created with `rd_kafka_conf_new()` that will
 * be used instead of the default configuration.
 * The \p conf object is freed by this function on success and must not be used
 * or destroyed by the application subsequently.
 * See `rd_kafka_conf_set()` et.al for more information.
 *
 * \p errstr must be a pointer to memory of at least size \p errstr_size where
 * `rd_kafka_new()` may write a human readable error message in case the
 * creation of a new handle fails. In which case the function returns NULL.
 *
 * @remark \b RD_KAFKA_CONSUMER: When a new \p RD_KAFKA_CONSUMER
 *           rd_kafka_t handle is created it may either operate in the
 *           legacy simple consumer mode using the rd_kafka_consume_start()
 *           interface, or the High-level KafkaConsumer API.
 * @remark An application must only use one of these groups of APIs on a given
 *         rd_kafka_t RD_KAFKA_CONSUMER handle.

 *
 * @returns The Kafka handle on success or NULL on error (see \p errstr)
 *
 * @sa To destroy the Kafka handle, use rd_kafka_destroy().
 */
RD_EXPORT
rd_kafka_t *rd_kafka_new(rd_kafka_type_t type,
                         rd_kafka_conf_t *conf,
                         char *errstr,
                         size_t errstr_size);


/**
 * @brief Destroy Kafka handle.
 *
 * @remark This is a blocking operation.
 * @remark rd_kafka_consumer_close() will be called from this function
 *         if the instance type is RD_KAFKA_CONSUMER, a \c group.id was
 *         configured, and the rd_kafka_consumer_close() was not
 *         explicitly called by the application. This in turn may
 *         trigger consumer callbacks, such as rebalance_cb.
 *         Use rd_kafka_destroy_flags() with
 *         RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE to avoid this behaviour.
 *
 * @sa rd_kafka_destroy_flags()
 */
RD_EXPORT
void rd_kafka_destroy(rd_kafka_t *rk);


/**
 * @brief Destroy Kafka handle according to specified destroy flags
 *
 */
RD_EXPORT
void rd_kafka_destroy_flags(rd_kafka_t *rk, int flags);

/**
 * @brief Flags for rd_kafka_destroy_flags()
 */

/*!
 * Don't call consumer_close() to leave group and commit final offsets.
 *
 * This also disables consumer callbacks to be called from rd_kafka_destroy*(),
 * such as rebalance_cb.
 *
 * The consumer group handler is still closed internally, but from an
 * application perspective none of the functionality from consumer_close()
 * is performed.
 */
#define RD_KAFKA_DESTROY_F_NO_CONSUMER_CLOSE 0x8



/**
 * @brief Returns Kafka handle name.
 */
RD_EXPORT
const char *rd_kafka_name(const rd_kafka_t *rk);


/**
 * @brief Returns Kafka handle type.
 */
RD_EXPORT
rd_kafka_type_t rd_kafka_type(const rd_kafka_t *rk);


/**
 * @brief Returns this client's broker-assigned group member id.
 *
 * @remark This currently requires the high-level KafkaConsumer
 *
 * @returns An allocated string containing the current broker-assigned group
 *          member id, or NULL if not available.
 *          The application must free the string with \p free() or
 *          rd_kafka_mem_free()
 */
RD_EXPORT
char *rd_kafka_memberid(const rd_kafka_t *rk);



/**
 * @brief Returns the ClusterId as reported in broker metadata.
 *
 * @param rk         Client instance.
 * @param timeout_ms If there is no cached value from metadata retrieval
 *                   then this specifies the maximum amount of time
 *                   (in milliseconds) the call will block waiting
 *                   for metadata to be retrieved.
 *                   Use 0 for non-blocking calls.

 * @remark Requires broker version >=0.10.0 and api.version.request=true.
 *
 * @remark The application must free the returned pointer
 *         using rd_kafka_mem_free().
 *
 * @returns a newly allocated string containing the ClusterId, or NULL
 *          if no ClusterId could be retrieved in the allotted timespan.
 */
RD_EXPORT
char *rd_kafka_clusterid(rd_kafka_t *rk, int timeout_ms);


/**
 * @brief Returns the current ControllerId as reported in broker metadata.
 *
 * @param rk         Client instance.
 * @param timeout_ms If there is no cached value from metadata retrieval
 *                   then this specifies the maximum amount of time
 *                   (in milliseconds) the call will block waiting
 *                   for metadata to be retrieved.
 *                   Use 0 for non-blocking calls.

 * @remark Requires broker version >=0.10.0 and api.version.request=true.
 *
 * @returns the controller broker id (>= 0), or -1 if no ControllerId could be
 *          retrieved in the allotted timespan.
 */
RD_EXPORT
int32_t rd_kafka_controllerid(rd_kafka_t *rk, int timeout_ms);


/**
 * @brief Creates a new topic handle for topic named \p topic.
 *
 * \p conf is an optional configuration for the topic created with
 * `rd_kafka_topic_conf_new()` that will be used instead of the default
 * topic configuration.
 * The \p conf object is freed by this function and must not be used or
 * destroyed by the application subsequently.
 * See `rd_kafka_topic_conf_set()` et.al for more information.
 *
 * Topic handles are refcounted internally and calling rd_kafka_topic_new()
 * again with the same topic name will return the previous topic handle
 * without updating the original handle's configuration.
 * Applications must eventually call rd_kafka_topic_destroy() for each
 * succesfull call to rd_kafka_topic_new() to clear up resources.
 *
 * @returns the new topic handle or NULL on error (use rd_kafka_errno2err()
 *          to convert system \p errno to an rd_kafka_resp_err_t error code.
 *
 * @sa rd_kafka_topic_destroy()
 */
RD_EXPORT
rd_kafka_topic_t *rd_kafka_topic_new(rd_kafka_t *rk,
                                     const char *topic,
                                     rd_kafka_topic_conf_t *conf);



/**
 * @brief Loose application's topic handle refcount as previously created
 *        with `rd_kafka_topic_new()`.
 *
 * @remark Since topic objects are refcounted (both internally and for the app)
 *         the topic object might not actually be destroyed by this call,
 *         but the application must consider the object destroyed.
 */
RD_EXPORT
void rd_kafka_topic_destroy(rd_kafka_topic_t *rkt);


/**
 * @brief Returns the topic name.
 */
RD_EXPORT
const char *rd_kafka_topic_name(const rd_kafka_topic_t *rkt);


/**
 * @brief Get the \p rkt_opaque pointer that was set in the topic configuration
 *        with rd_kafka_topic_conf_set_opaque().
 */
RD_EXPORT
void *rd_kafka_topic_opaque(const rd_kafka_topic_t *rkt);


/**
 * @brief Unassigned partition.
 *
 * The unassigned partition is used by the producer API for messages
 * that should be partitioned using the configured or default partitioner.
 */
#define RD_KAFKA_PARTITION_UA ((int32_t)-1)


/**
 * @brief Polls the provided kafka handle for events.
 *
 * Events will cause application-provided callbacks to be called.
 *
 * The \p timeout_ms argument specifies the maximum amount of time
 * (in milliseconds) that the call will block waiting for events.
 * For non-blocking calls, provide 0 as \p timeout_ms.
 * To wait indefinitely for an event, provide -1.
 *
 * @remark  An application should make sure to call poll() at regular
 *          intervals to serve any queued callbacks waiting to be called.
 * @remark  If your producer doesn't have any callback set (in particular
 *          via rd_kafka_conf_set_dr_msg_cb or rd_kafka_conf_set_error_cb)
 *          you might choose not to call poll(), though this is not
 *          recommended.
 *
 * Events:
 *   - delivery report callbacks (if dr_cb/dr_msg_cb is configured) [producer]
 *   - error callbacks (rd_kafka_conf_set_error_cb()) [all]
 *   - stats callbacks (rd_kafka_conf_set_stats_cb()) [all]
 *   - throttle callbacks (rd_kafka_conf_set_throttle_cb()) [all]
 *   - OAUTHBEARER token refresh callbacks
 * (rd_kafka_conf_set_oauthbearer_token_refresh_cb()) [all]
 *
 * @returns the number of events served.
 */
RD_EXPORT
int rd_kafka_poll(rd_kafka_t *rk, int timeout_ms);


/**
 * @brief Cancels the current callback dispatcher (rd_kafka_poll(),
 *        rd_kafka_consume_callback(), etc).
 *
 * A callback may use this to force an immediate return to the calling
 * code (caller of e.g. rd_kafka_poll()) without processing any further
 * events.
 *
 * @remark This function MUST ONLY be called from within a librdkafka callback.
 */
RD_EXPORT
void rd_kafka_yield(rd_kafka_t *rk);



/**
 * @brief Pause producing or consumption for the provided list of partitions.
 *
 * Success or error is returned per-partition \p err in the \p partitions list.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_pause_partitions(rd_kafka_t *rk,
                          rd_kafka_topic_partition_list_t *partitions);



/**
 * @brief Resume producing consumption for the provided list of partitions.
 *
 * Success or error is returned per-partition \p err in the \p partitions list.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_resume_partitions(rd_kafka_t *rk,
                           rd_kafka_topic_partition_list_t *partitions);



/**
 * @brief Query broker for low (oldest/beginning) and high (newest/end) offsets
 *        for partition.
 *
 * Offsets are returned in \p *low and \p *high respectively.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code on failure.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_query_watermark_offsets(rd_kafka_t *rk,
                                 const char *topic,
                                 int32_t partition,
                                 int64_t *low,
                                 int64_t *high,
                                 int timeout_ms);


/**
 * @brief Get last known low (oldest/beginning) and high (newest/end) offsets
 *        for partition.
 *
 * The low offset is updated periodically (if statistics.interval.ms is set)
 * while the high offset is updated on each fetched message set from the broker.
 *
 * If there is no cached offset (either low or high, or both) then
 * RD_KAFKA_OFFSET_INVALID will be returned for the respective offset.
 *
 * Offsets are returned in \p *low and \p *high respectively.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code on failure.
 *
 * @remark Shall only be used with an active consumer instance.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_get_watermark_offsets(rd_kafka_t *rk,
                                                             const char *topic,
                                                             int32_t partition,
                                                             int64_t *low,
                                                             int64_t *high);



/**
 * @brief Look up the offsets for the given partitions by timestamp.
 *
 * The returned offset for each partition is the earliest offset whose
 * timestamp is greater than or equal to the given timestamp in the
 * corresponding partition.
 *
 * The timestamps to query are represented as \c offset in \p offsets
 * on input, and \c offset will contain the offset on output.
 *
 * The function will block for at most \p timeout_ms milliseconds.
 *
 * @remark Duplicate Topic+Partitions are not supported.
 * @remark Per-partition errors may be returned in \c
 * rd_kafka_topic_partition_t.err
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if offsets were be queried (do note
 *          that per-partition errors might be set),
 *          RD_KAFKA_RESP_ERR__TIMED_OUT if not all offsets could be fetched
 *          within \p timeout_ms,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if the \p offsets list is empty,
 *          RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION if all partitions are unknown,
 *          RD_KAFKA_RESP_ERR_LEADER_NOT_AVAILABLE if unable to query leaders
 *          for the given partitions.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_offsets_for_times(rd_kafka_t *rk,
                           rd_kafka_topic_partition_list_t *offsets,
                           int timeout_ms);



/**
 * @brief Allocate and zero memory using the same allocator librdkafka uses.
 *
 * This is typically an abstraction for the calloc(3) call and makes sure
 * the application can use the same memory allocator as librdkafka for
 * allocating pointers that are used by librdkafka.
 *
 * \p rk can be set to return memory allocated by a specific \c rk instance
 * otherwise pass NULL for \p rk.
 *
 * @remark Memory allocated by rd_kafka_mem_calloc() must be freed using
 *         rd_kafka_mem_free()
 */
RD_EXPORT
void *rd_kafka_mem_calloc(rd_kafka_t *rk, size_t num, size_t size);



/**
 * @brief Allocate memory using the same allocator librdkafka uses.
 *
 * This is typically an abstraction for the malloc(3) call and makes sure
 * the application can use the same memory allocator as librdkafka for
 * allocating pointers that are used by librdkafka.
 *
 * \p rk can be set to return memory allocated by a specific \c rk instance
 * otherwise pass NULL for \p rk.
 *
 * @remark Memory allocated by rd_kafka_mem_malloc() must be freed using
 *         rd_kafka_mem_free()
 */
RD_EXPORT
void *rd_kafka_mem_malloc(rd_kafka_t *rk, size_t size);



/**
 * @brief Free pointer returned by librdkafka
 *
 * This is typically an abstraction for the free(3) call and makes sure
 * the application can use the same memory allocator as librdkafka for
 * freeing pointers returned by librdkafka.
 *
 * In standard setups it is usually not necessary to use this interface
 * rather than the free(3) functione.
 *
 * \p rk must be set for memory returned by APIs that take an \c rk argument,
 * for other APIs pass NULL for \p rk.
 *
 * @remark rd_kafka_mem_free() must only be used for pointers returned by APIs
 *         that explicitly mention using this function for freeing.
 */
RD_EXPORT
void rd_kafka_mem_free(rd_kafka_t *rk, void *ptr);


/**@}*/



/**
 * @name Queue API
 * @{
 *
 * Message queues allows the application to re-route consumed messages
 * from multiple topic+partitions into one single queue point.
 * This queue point containing messages from a number of topic+partitions
 * may then be served by a single rd_kafka_consume*_queue() call,
 * rather than one call per topic+partition combination.
 */


/**
 * @brief Create a new message queue.
 *
 * See rd_kafka_consume_start_queue(), rd_kafka_consume_queue(), et.al.
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_new(rd_kafka_t *rk);

/**
 * Destroy a queue, purging all of its enqueued messages.
 */
RD_EXPORT
void rd_kafka_queue_destroy(rd_kafka_queue_t *rkqu);


/**
 * @returns a reference to the main librdkafka event queue.
 * This is the queue served by rd_kafka_poll().
 *
 * Use rd_kafka_queue_destroy() to loose the reference.
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_get_main(rd_kafka_t *rk);



/**
 * @returns a reference to the SASL callback queue, if a SASL mechanism
 *          with callbacks is configured (currently only OAUTHBEARER), else
 *          returns NULL.
 *
 * Use rd_kafka_queue_destroy() to loose the reference.
 *
 * @sa rd_kafka_sasl_background_callbacks_enable()
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_get_sasl(rd_kafka_t *rk);


/**
 * @brief Enable SASL OAUTHBEARER refresh callbacks on the librdkafka
 *        background thread.
 *
 * This serves as an alternative for applications that do not call
 * rd_kafka_poll() (et.al.) at regular intervals (or not at all), as a means
 * of automatically trigger the refresh callbacks, which are needed to
 * initiate connections to the brokers in the case a custom OAUTHBEARER
 * refresh callback is configured.
 *
 * @returns NULL on success or an error object on error.
 *
 * @sa rd_kafka_queue_get_sasl()
 * @sa rd_kafka_conf_set_oauthbearer_token_refresh_cb()
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_sasl_background_callbacks_enable(rd_kafka_t *rk);


/**
 * @brief Sets SASL credentials used for SASL PLAIN and SCRAM mechanisms by
 *        this Kafka client.
 *
 * This function sets or resets the SASL username and password credentials
 * used by this Kafka client. The new credentials will be used the next time
 * this client needs to authenticate to a broker. This function
 * will not disconnect existing connections that might have been made using
 * the old credentials.
 *
 * @remark This function only applies to the SASL PLAIN and SCRAM mechanisms.
 *
 * @returns NULL on success or an error object on error.
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_sasl_set_credentials(rd_kafka_t *rk,
                                                const char *username,
                                                const char *password);

/**
 * @returns a reference to the librdkafka consumer queue.
 * This is the queue served by rd_kafka_consumer_poll().
 *
 * Use rd_kafka_queue_destroy() to loose the reference.
 *
 * @remark rd_kafka_queue_destroy() MUST be called on this queue
 *         prior to calling rd_kafka_consumer_close().
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_get_consumer(rd_kafka_t *rk);

/**
 * @returns a reference to the partition's queue, or NULL if
 *          partition is invalid.
 *
 * Use rd_kafka_queue_destroy() to loose the reference.
 *
 * @remark rd_kafka_queue_destroy() MUST be called on this queue
 *
 * @remark This function only works on consumers.
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_get_partition(rd_kafka_t *rk,
                                               const char *topic,
                                               int32_t partition);

/**
 * @returns a reference to the background thread queue, or NULL if the
 *          background queue is not enabled.
 *
 * The background thread queue provides the application with an automatically
 * polled queue that triggers the event callback in a background thread,
 * this background thread is completely managed by librdkafka.
 *
 * The background thread queue is automatically created if a generic event
 * handler callback is configured with rd_kafka_conf_set_background_event_cb()
 * or if rd_kafka_queue_get_background() is called.
 *
 * The background queue is polled and served by librdkafka and MUST NOT be
 * polled, forwarded, or otherwise managed by the application, it may only
 * be used as the destination queue passed to queue-enabled APIs, such as
 * the Admin API.
 *
 * Use rd_kafka_queue_destroy() to loose the reference.
 *
 * @warning The background queue MUST NOT be read from (polled, consumed, etc),
 *          or forwarded from.
 */
RD_EXPORT
rd_kafka_queue_t *rd_kafka_queue_get_background(rd_kafka_t *rk);


/**
 * @brief Forward/re-route queue \p src to \p dst.
 * If \p dst is \c NULL the forwarding is removed.
 *
 * The internal refcounts for both queues are increased.
 *
 * @remark Regardless of whether \p dst is NULL or not, after calling this
 *         function, \p src will not forward it's fetch queue to the consumer
 *         queue.
 */
RD_EXPORT
void rd_kafka_queue_forward(rd_kafka_queue_t *src, rd_kafka_queue_t *dst);

/**
 * @brief Forward librdkafka logs (and debug) to the specified queue
 *        for serving with one of the ..poll() calls.
 *
 *        This allows an application to serve log callbacks (\c log_cb)
 *        in its thread of choice.
 *
 * @param rk   Client instance.
 * @param rkqu Queue to forward logs to. If the value is NULL the logs
 *        are forwarded to the main queue.
 *
 * @remark The configuration property \c log.queue MUST also be set to true.
 *
 * @remark librdkafka maintains its own reference to the provided queue.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code on error,
 * eg RD_KAFKA_RESP_ERR__NOT_CONFIGURED when log.queue is not set to true.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_set_log_queue(rd_kafka_t *rk,
                                           rd_kafka_queue_t *rkqu);


/**
 * @returns the current number of elements in queue.
 */
RD_EXPORT
size_t rd_kafka_queue_length(rd_kafka_queue_t *rkqu);


/**
 * @brief Enable IO event triggering for queue.
 *
 * To ease integration with IO based polling loops this API
 * allows an application to create a separate file-descriptor
 * that librdkafka will write \p payload (of size \p size) to
 * whenever a new element is enqueued on a previously empty queue.
 *
 * To remove event triggering call with \p fd = -1.
 *
 * librdkafka will maintain a copy of the \p payload.
 *
 * @remark IO and callback event triggering are mutually exclusive.
 * @remark When using forwarded queues the IO event must only be enabled
 *         on the final forwarded-to (destination) queue.
 * @remark The file-descriptor/socket must be set to non-blocking.
 */
RD_EXPORT
void rd_kafka_queue_io_event_enable(rd_kafka_queue_t *rkqu,
                                    int fd,
                                    const void *payload,
                                    size_t size);

/**
 * @brief Enable callback event triggering for queue.
 *
 * The callback will be called from an internal librdkafka thread
 * when a new element is enqueued on a previously empty queue.
 *
 * To remove event triggering call with \p event_cb = NULL.
 *
 * The \p qev_opaque is passed to the callback's \p qev_opaque argument.
 *
 * @remark IO and callback event triggering are mutually exclusive.
 * @remark Since the callback may be triggered from internal librdkafka
 *         threads, the application must not perform any pro-longed work in
 *         the callback, or call any librdkafka APIs (for the same rd_kafka_t
 *         handle).
 */
RD_EXPORT
void rd_kafka_queue_cb_event_enable(rd_kafka_queue_t *rkqu,
                                    void (*event_cb)(rd_kafka_t *rk,
                                                     void *qev_opaque),
                                    void *qev_opaque);


/**
 * @brief Cancels the current rd_kafka_queue_poll() on \p rkqu.
 *
 * An application may use this from another thread to force
 * an immediate return to the calling code (caller of rd_kafka_queue_poll()).
 * Must not be used from signal handlers since that may cause deadlocks.
 */
RD_EXPORT
void rd_kafka_queue_yield(rd_kafka_queue_t *rkqu);


/**@}*/

/**
 *
 * @name Simple Consumer API (legacy)
 * @{
 *
 */


#define RD_KAFKA_OFFSET_BEGINNING                                              \
        -2 /**< Start consuming from beginning of                              \
            *   kafka partition queue: oldest msg */
#define RD_KAFKA_OFFSET_END                                                    \
        -1 /**< Start consuming from end of kafka                              \
            *   partition queue: next msg */
#define RD_KAFKA_OFFSET_STORED                                                 \
        -1000 /**< Start consuming from offset retrieved                       \
               *   from offset store */
#define RD_KAFKA_OFFSET_INVALID -1001 /**< Invalid offset */


/** @cond NO_DOC */
#define RD_KAFKA_OFFSET_TAIL_BASE -2000 /* internal: do not use */
/** @endcond */

/**
 * @brief Start consuming \p CNT messages from topic's current end offset.
 *
 * That is, if current end offset is 12345 and \p CNT is 200, it will start
 * consuming from offset \c 12345-200 = \c 12145. */
#define RD_KAFKA_OFFSET_TAIL(CNT) (RD_KAFKA_OFFSET_TAIL_BASE - (CNT))

/**
 * @brief Start consuming messages for topic \p rkt and \p partition
 * at offset \p offset which may either be an absolute \c (0..N)
 * or one of the logical offsets:
 *  - RD_KAFKA_OFFSET_BEGINNING
 *  - RD_KAFKA_OFFSET_END
 *  - RD_KAFKA_OFFSET_STORED
 *  - RD_KAFKA_OFFSET_TAIL
 *
 * rdkafka will attempt to keep \c queued.min.messages (config property)
 * messages in the local queue by repeatedly fetching batches of messages
 * from the broker until the threshold is reached.
 *
 * The application shall use one of the `rd_kafka_consume*()` functions
 * to consume messages from the local queue, each kafka message being
 * represented as a `rd_kafka_message_t *` object.
 *
 * `rd_kafka_consume_start()` must not be called multiple times for the same
 * topic and partition without stopping consumption first with
 * `rd_kafka_consume_stop()`.
 *
 * @returns 0 on success or -1 on error in which case errno is set accordingly:
 *  - EBUSY    - Conflicts with an existing or previous subscription
 *               (RD_KAFKA_RESP_ERR__CONFLICT)
 *  - EINVAL   - Invalid offset, or incomplete configuration (lacking group.id)
 *               (RD_KAFKA_RESP_ERR__INVALID_ARG)
 *  - ESRCH    - requested \p partition is invalid.
 *               (RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION)
 *  - ENOENT   - topic is unknown in the Kafka cluster.
 *               (RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
 *
 * Use `rd_kafka_errno2err()` to convert sytem \c errno to `rd_kafka_resp_err_t`
 */
RD_EXPORT
int rd_kafka_consume_start(rd_kafka_topic_t *rkt,
                           int32_t partition,
                           int64_t offset);

/**
 * @brief Same as rd_kafka_consume_start() but re-routes incoming messages to
 * the provided queue \p rkqu (which must have been previously allocated
 * with `rd_kafka_queue_new()`.
 *
 * The application must use one of the `rd_kafka_consume_*_queue()` functions
 * to receive fetched messages.
 *
 * `rd_kafka_consume_start_queue()` must not be called multiple times for the
 * same topic and partition without stopping consumption first with
 * `rd_kafka_consume_stop()`.
 * `rd_kafka_consume_start()` and `rd_kafka_consume_start_queue()` must not
 * be combined for the same topic and partition.
 */
RD_EXPORT
int rd_kafka_consume_start_queue(rd_kafka_topic_t *rkt,
                                 int32_t partition,
                                 int64_t offset,
                                 rd_kafka_queue_t *rkqu);

/**
 * @brief Stop consuming messages for topic \p rkt and \p partition, purging
 * all messages currently in the local queue.
 *
 * NOTE: To enforce synchronisation this call will block until the internal
 *       fetcher has terminated and offsets are committed to configured
 *       storage method.
 *
 * The application needs to be stop all consumers before calling
 * `rd_kafka_destroy()` on the main object handle.
 *
 * @returns 0 on success or -1 on error (see `errno`).
 */
RD_EXPORT
int rd_kafka_consume_stop(rd_kafka_topic_t *rkt, int32_t partition);



/**
 * @brief Seek consumer for topic+partition to \p offset which is either an
 *        absolute or logical offset.
 *
 * If \p timeout_ms is specified (not 0) the seek call will wait this long
 * for the consumer to update its fetcher state for the given partition with
 * the new offset. This guarantees that no previously fetched messages for the
 * old offset (or fetch position) will be passed to the application.
 *
 * If the timeout is reached the internal state will be unknown to the caller
 * and this function returns `RD_KAFKA_RESP_ERR__TIMED_OUT`.
 *
 * If \p timeout_ms is 0 it will initiate the seek but return
 * immediately without any error reporting (e.g., async).
 *
 * This call will purge all pre-fetched messages for the given partition, which
 * may be up to \c queued.max.message.kbytes in size. Repeated use of seek
 * may thus lead to increased network usage as messages are re-fetched from
 * the broker.
 *
 * @remark Seek must only be performed for already assigned/consumed partitions,
 *         use rd_kafka_assign() (et.al) to set the initial starting offset
 *         for a new assignmenmt.
 *
 * @returns `RD_KAFKA_RESP_ERR__NO_ERROR` on success else an error code.
 *
 * @deprecated Use rd_kafka_seek_partitions().
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_seek(rd_kafka_topic_t *rkt,
                                  int32_t partition,
                                  int64_t offset,
                                  int timeout_ms);



/**
 * @brief Seek consumer for partitions in \p partitions to the per-partition
 *        offset in the \c .offset field of \p partitions.
 *
 * The offset may be either absolute (>= 0) or a logical offset.
 *
 * If \p timeout_ms is specified (not 0) the seek call will wait this long
 * for the consumer to update its fetcher state for the given partition with
 * the new offset. This guarantees that no previously fetched messages for the
 * old offset (or fetch position) will be passed to the application.
 *
 * If the timeout is reached the internal state will be unknown to the caller
 * and this function returns `RD_KAFKA_RESP_ERR__TIMED_OUT`.
 *
 * If \p timeout_ms is 0 it will initiate the seek but return
 * immediately without any error reporting (e.g., async).
 *
 * This call will purge all pre-fetched messages for the given partition, which
 * may be up to \c queued.max.message.kbytes in size. Repeated use of seek
 * may thus lead to increased network usage as messages are re-fetched from
 * the broker.
 *
 * Individual partition errors are reported in the per-partition \c .err field
 * of \p partitions.
 *
 * @remark Seek must only be performed for already assigned/consumed partitions,
 *         use rd_kafka_assign() (et.al) to set the initial starting offset
 *         for a new assignmenmt.
 *
 * @returns NULL on success or an error object on failure.
 */
RD_EXPORT rd_kafka_error_t *
rd_kafka_seek_partitions(rd_kafka_t *rk,
                         rd_kafka_topic_partition_list_t *partitions,
                         int timeout_ms);


/**
 * @brief Consume a single message from topic \p rkt and \p partition
 *
 * \p timeout_ms is maximum amount of time to wait for a message to be received.
 * Consumer must have been previously started with `rd_kafka_consume_start()`.
 *
 * @returns a message object on success or \c NULL on error.
 * The message object must be destroyed with `rd_kafka_message_destroy()`
 * when the application is done with it.
 *
 * Errors (when returning NULL):
 *  - ETIMEDOUT - \p timeout_ms was reached with no new messages fetched.
 *  - ENOENT    - \p rkt + \p partition is unknown.
 *                 (no prior `rd_kafka_consume_start()` call)
 *
 * NOTE: The returned message's \c ..->err must be checked for errors.
 * NOTE: \c ..->err \c == \c RD_KAFKA_RESP_ERR__PARTITION_EOF signals that the
 *       end of the partition has been reached, which should typically not be
 *       considered an error. The application should handle this case
 *       (e.g., ignore).
 *
 * @remark on_consume() interceptors may be called from this function prior to
 *         passing message to application.
 */
RD_EXPORT
rd_kafka_message_t *
rd_kafka_consume(rd_kafka_topic_t *rkt, int32_t partition, int timeout_ms);



/**
 * @brief Consume up to \p rkmessages_size from topic \p rkt and \p partition
 *        putting a pointer to each message in the application provided
 *        array \p rkmessages (of size \p rkmessages_size entries).
 *
 * `rd_kafka_consume_batch()` provides higher throughput performance
 * than `rd_kafka_consume()`.
 *
 * \p timeout_ms is the maximum amount of time to wait for all of
 * \p rkmessages_size messages to be put into \p rkmessages.
 * If no messages were available within the timeout period this function
 * returns 0 and \p rkmessages remains untouched.
 * This differs somewhat from `rd_kafka_consume()`.
 *
 * The message objects must be destroyed with `rd_kafka_message_destroy()`
 * when the application is done with it.
 *
 * @returns the number of rkmessages added in \p rkmessages,
 * or -1 on error (same error codes as for `rd_kafka_consume()`.
 *
 * @sa rd_kafka_consume()
 *
 * @remark on_consume() interceptors may be called from this function prior to
 *         passing message to application.
 */
RD_EXPORT
ssize_t rd_kafka_consume_batch(rd_kafka_topic_t *rkt,
                               int32_t partition,
                               int timeout_ms,
                               rd_kafka_message_t **rkmessages,
                               size_t rkmessages_size);



/**
 * @brief Consumes messages from topic \p rkt and \p partition, calling
 * the provided callback for each consumed messsage.
 *
 * `rd_kafka_consume_callback()` provides higher throughput performance
 * than both `rd_kafka_consume()` and `rd_kafka_consume_batch()`.
 *
 * \p timeout_ms is the maximum amount of time to wait for one or more messages
 * to arrive.
 *
 * The provided \p consume_cb function is called for each message,
 * the application \b MUST \b NOT call `rd_kafka_message_destroy()` on the
 * provided \p rkmessage.
 *
 * The \p commit_opaque argument is passed to the \p consume_cb
 * as \p commit_opaque.
 *
 * @returns the number of messages processed or -1 on error.
 *
 * @sa rd_kafka_consume()
 *
 * @remark on_consume() interceptors may be called from this function prior to
 *         passing message to application.
 *
 * @remark This function will return early if a transaction control message is
 *         received, these messages are not exposed to the application but
 *         still enqueued on the consumer queue to make sure their
 *         offsets are stored.
 *
 * @deprecated This API is deprecated and subject for future removal.
 *             There is no new callback-based consume interface, use the
 *             poll/queue based alternatives.
 */
RD_EXPORT
int rd_kafka_consume_callback(rd_kafka_topic_t *rkt,
                              int32_t partition,
                              int timeout_ms,
                              void (*consume_cb)(rd_kafka_message_t *rkmessage,
                                                 void *commit_opaque),
                              void *commit_opaque);


/**@}*/

/**
 * @name Simple Consumer API (legacy): Queue consumers
 * @{
 *
 * The following `..._queue()` functions are analogue to the functions above
 * but reads messages from the provided queue \p rkqu instead.
 * \p rkqu must have been previously created with `rd_kafka_queue_new()`
 * and the topic consumer must have been started with
 * `rd_kafka_consume_start_queue()` utilising the the same queue.
 */

/**
 * @brief Consume from queue
 *
 * @sa rd_kafka_consume()
 */
RD_EXPORT
rd_kafka_message_t *rd_kafka_consume_queue(rd_kafka_queue_t *rkqu,
                                           int timeout_ms);

/**
 * @brief Consume batch of messages from queue
 *
 * @sa rd_kafka_consume_batch()
 */
RD_EXPORT
ssize_t rd_kafka_consume_batch_queue(rd_kafka_queue_t *rkqu,
                                     int timeout_ms,
                                     rd_kafka_message_t **rkmessages,
                                     size_t rkmessages_size);

/**
 * @brief Consume multiple messages from queue with callback
 *
 * @sa rd_kafka_consume_callback()
 *
 * @deprecated This API is deprecated and subject for future removal.
 *             There is no new callback-based consume interface, use the
 *             poll/queue based alternatives.
 */
RD_EXPORT
int rd_kafka_consume_callback_queue(
    rd_kafka_queue_t *rkqu,
    int timeout_ms,
    void (*consume_cb)(rd_kafka_message_t *rkmessage, void *commit_opaque),
    void *commit_opaque);


/**@}*/



/**
 * @name Simple Consumer API (legacy): Topic+partition offset store.
 * @{
 *
 * If \c auto.commit.enable is true the offset is stored automatically prior to
 * returning of the message(s) in each of the rd_kafka_consume*() functions
 * above.
 */


/**
 * @brief Store offset \p offset + 1 for topic \p rkt partition \p partition.
 *
 * The \c offset + 1 will be committed (written) to broker (or file) according
 * to \c `auto.commit.interval.ms` or manual offset-less commit()
 *
 * @warning This method may only be called for partitions that are currently
 *          assigned.
 *          Non-assigned partitions will fail with RD_KAFKA_RESP_ERR__STATE.
 *          Since v1.9.0.
 *
 * @warning Avoid storing offsets after calling rd_kafka_seek() (et.al) as
 *          this may later interfere with resuming a paused partition, instead
 *          store offsets prior to calling seek.
 *
 * @remark \c `enable.auto.offset.store` must be set to "false" when using
 *         this API.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code on error.
 */
RD_EXPORT
rd_kafka_resp_err_t
rd_kafka_offset_store(rd_kafka_topic_t *rkt, int32_t partition, int64_t offset);


/**
 * @brief Store offsets for next auto-commit for one or more partitions.
 *
 * The offset will be committed (written) to the offset store according
 * to \c `auto.commit.interval.ms` or manual offset-less commit().
 *
 * Per-partition success/error status propagated through each partition's
 * \c .err for all return values (even NO_ERROR) except INVALID_ARG.
 *
 * @warning This method may only be called for partitions that are currently
 *          assigned.
 *          Non-assigned partitions will fail with RD_KAFKA_RESP_ERR__STATE.
 *          Since v1.9.0.
 *
 * @warning Avoid storing offsets after calling rd_kafka_seek() (et.al) as
 *          this may later interfere with resuming a paused partition, instead
 *          store offsets prior to calling seek.
 *
 * @remark The \c .offset field is stored as is, it will NOT be + 1.
 *
 * @remark \c `enable.auto.offset.store` must be set to "false" when using
 *         this API.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on (partial) success, or
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \c enable.auto.offset.store
 *          is true, or
 *          RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION or RD_KAFKA_RESP_ERR__STATE
 *          if none of the offsets could be stored.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_offsets_store(rd_kafka_t *rk,
                       rd_kafka_topic_partition_list_t *offsets);
/**@}*/



/**
 * @name KafkaConsumer (C)
 * @brief High-level KafkaConsumer C API
 * @{
 *
 *
 *
 */

/**
 * @brief Subscribe to topic set using balanced consumer groups.
 *
 * Wildcard (regex) topics are supported:
 * any topic name in the \p topics list that is prefixed with \c \"^\" will
 * be regex-matched to the full list of topics in the cluster and matching
 * topics will be added to the subscription list.
 *
 * The full topic list is retrieved every \c topic.metadata.refresh.interval.ms
 * to pick up new or delete topics that match the subscription.
 * If there is any change to the matched topics the consumer will
 * immediately rejoin the group with the updated set of subscribed topics.
 *
 * Regex and full topic names can be mixed in \p topics.
 *
 * @remark Only the \c .topic field is used in the supplied \p topics list,
 *         all other fields are ignored.
 *
 * @remark subscribe() is an asynchronous method which returns immediately:
 *         background threads will (re)join the group, wait for group rebalance,
 *         issue any registered rebalance_cb, assign() the assigned partitions,
 *         and then start fetching messages. This cycle may take up to
 *         \c session.timeout.ms * 2 or more to complete.
 *
 * @remark After this call returns a consumer error will be returned by
 *         rd_kafka_consumer_poll (et.al) for each unavailable topic in the
 *         \p topics. The error will be RD_KAFKA_RESP_ERR_UNKNOWN_TOPIC_OR_PART
 *         for non-existent topics, and
 *         RD_KAFKA_RESP_ERR_TOPIC_AUTHORIZATION_FAILED for unauthorized topics.
 *         The consumer error will be raised through rd_kafka_consumer_poll()
 *         (et.al.) with the \c rd_kafka_message_t.err field set to one of the
 *         error codes mentioned above.
 *         The subscribe function itself is asynchronous and will not return
 *         an error on unavailable topics.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if list is empty, contains invalid
 *          topics or regexes or duplicate entries,
 *          RD_KAFKA_RESP_ERR__FATAL if the consumer has raised a fatal error.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_subscribe(rd_kafka_t *rk,
                   const rd_kafka_topic_partition_list_t *topics);


/**
 * @brief Unsubscribe from the current subscription set.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_unsubscribe(rd_kafka_t *rk);


/**
 * @brief Returns the current topic subscription
 *
 * @returns An error code on failure, otherwise \p topic is updated
 *          to point to a newly allocated topic list (possibly empty).
 *
 * @remark The application is responsible for calling
 *         rd_kafka_topic_partition_list_destroy on the returned list.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_subscription(rd_kafka_t *rk, rd_kafka_topic_partition_list_t **topics);



/**
 * @brief Poll the consumer for messages or events.
 *
 * Will block for at most \p timeout_ms milliseconds.
 *
 * @remark  An application should make sure to call consumer_poll() at regular
 *          intervals, even if no messages are expected, to serve any
 *          queued callbacks waiting to be called. This is especially
 *          important when a rebalance_cb has been registered as it needs
 *          to be called and handled properly to synchronize internal
 *          consumer state.
 *
 * @returns A message object which is a proper message if \p ->err is
 *          RD_KAFKA_RESP_ERR_NO_ERROR, or an event or error for any other
 *          value.
 *
 * @remark on_consume() interceptors may be called from this function prior to
 *         passing message to application.
 *
 * @remark When subscribing to topics the application must call poll at
 *         least every \c max.poll.interval.ms to remain a member of the
 *         consumer group.
 *
 * Noteworthy errors returned in \c ->err:
 * - RD_KAFKA_RESP_ERR__MAX_POLL_EXCEEDED - application failed to call
 *   poll within `max.poll.interval.ms`.
 *
 * @sa rd_kafka_message_t
 */
RD_EXPORT
rd_kafka_message_t *rd_kafka_consumer_poll(rd_kafka_t *rk, int timeout_ms);

/**
 * @brief Close the consumer.
 *
 * This call will block until the consumer has revoked its assignment,
 * calling the \c rebalance_cb if it is configured, committed offsets
 * to broker, and left the consumer group (if applicable).
 * The maximum blocking time is roughly limited to session.timeout.ms.
 *
 * @returns An error code indicating if the consumer close was succesful
 *          or not.
 *          RD_KAFKA_RESP_ERR__FATAL is returned if the consumer has raised
 *          a fatal error.
 *
 * @remark The application still needs to call rd_kafka_destroy() after
 *         this call finishes to clean up the underlying handle resources.
 *
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_consumer_close(rd_kafka_t *rk);


/**
 * @brief Asynchronously close the consumer.
 *
 * Performs the same actions as rd_kafka_consumer_close() but in a
 * background thread.
 *
 * Rebalance events/callbacks (etc) will be forwarded to the
 * application-provided \p rkqu. The application must poll/serve this queue
 * until rd_kafka_consumer_closed() returns true.
 *
 * @remark Depending on consumer group join state there may or may not be
 *         rebalance events emitted on \p rkqu.
 *
 * @returns an error object if the consumer close failed, else NULL.
 *
 * @sa rd_kafka_consumer_closed()
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_consumer_close_queue(rd_kafka_t *rk,
                                                rd_kafka_queue_t *rkqu);


/**
 * @returns 1 if the consumer is closed, else 0.
 *
 * Should be used in conjunction with rd_kafka_consumer_close_queue() to know
 * when the consumer has been closed.
 *
 * @sa rd_kafka_consumer_close_queue()
 */
RD_EXPORT
int rd_kafka_consumer_closed(rd_kafka_t *rk);


/**
 * @brief Incrementally add \p partitions to the current assignment.
 *
 * If a COOPERATIVE assignor (i.e. incremental rebalancing) is being used,
 * this method should be used in a rebalance callback to adjust the current
 * assignment appropriately in the case where the rebalance type is
 * RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS. The application must pass the
 * partition list passed to the callback (or a copy of it), even if the
 * list is empty. \p partitions must not be NULL. This method may also be
 * used outside the context of a rebalance callback.
 *
 * @returns NULL on success, or an error object if the operation was
 *          unsuccessful.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT rd_kafka_error_t *
rd_kafka_incremental_assign(rd_kafka_t *rk,
                            const rd_kafka_topic_partition_list_t *partitions);


/**
 * @brief Incrementally remove \p partitions from the current assignment.
 *
 * If a COOPERATIVE assignor (i.e. incremental rebalancing) is being used,
 * this method should be used in a rebalance callback to adjust the current
 * assignment appropriately in the case where the rebalance type is
 * RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS. The application must pass the
 * partition list passed to the callback (or a copy of it), even if the
 * list is empty. \p partitions must not be NULL. This method may also be
 * used outside the context of a rebalance callback.
 *
 * @returns NULL on success, or an error object if the operation was
 *          unsuccessful.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT rd_kafka_error_t *rd_kafka_incremental_unassign(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *partitions);


/**
 * @brief The rebalance protocol currently in use. This will be
 *        "NONE" if the consumer has not (yet) joined a group, else it will
 *        match the rebalance protocol ("EAGER", "COOPERATIVE") of the
 *        configured and selected assignor(s). All configured
 *        assignors must have the same protocol type, meaning
 *        online migration of a consumer group from using one
 *        protocol to another (in particular upgading from EAGER
 *        to COOPERATIVE) without a restart is not currently
 *        supported.
 *
 * @returns NULL on error, or one of "NONE", "EAGER", "COOPERATIVE" on success.
 */
RD_EXPORT
const char *rd_kafka_rebalance_protocol(rd_kafka_t *rk);


/**
 * @brief Atomic assignment of partitions to consume.
 *
 * The new \p partitions will replace the existing assignment.
 *
 * A zero-length \p partitions will treat the partitions as a valid,
 * albeit empty assignment, and maintain internal state, while a \c NULL
 * value for \p partitions will reset and clear the internal state.
 *
 * When used from a rebalance callback, the application should pass the
 * partition list passed to the callback (or a copy of it) even if the list
 * is empty (i.e. should not pass NULL in this case) so as to maintain
 * internal join state. This is not strictly required - the application
 * may adjust the assignment provided by the group. However, this is rarely
 * useful in practice.
 *
 * @returns An error code indicating if the new assignment was applied or not.
 *          RD_KAFKA_RESP_ERR__FATAL is returned if the consumer has raised
 *          a fatal error.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_assign(rd_kafka_t *rk,
                const rd_kafka_topic_partition_list_t *partitions);

/**
 * @brief Returns the current partition assignment as set by rd_kafka_assign()
 *        or rd_kafka_incremental_assign().
 *
 * @returns An error code on failure, otherwise \p partitions is updated
 *          to point to a newly allocated partition list (possibly empty).
 *
 * @remark The application is responsible for calling
 *         rd_kafka_topic_partition_list_destroy on the returned list.
 *
 * @remark This assignment represents the partitions assigned through the
 *         assign functions and not the partitions assigned to this consumer
 *         instance by the consumer group leader.
 *         They are usually the same following a rebalance but not necessarily
 *         since an application is free to assign any partitions.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_assignment(rd_kafka_t *rk,
                    rd_kafka_topic_partition_list_t **partitions);


/**
 * @brief Check whether the consumer considers the current assignment to
 *        have been lost involuntarily. This method is only applicable for
 *        use with a high level subscribing consumer. Assignments are revoked
 *        immediately when determined to have been lost, so this method
 *        is only useful when reacting to a RD_KAFKA_EVENT_REBALANCE event
 *        or from within a rebalance_cb. Partitions that have been lost may
 *        already be owned by other members in the group and therefore
 *        commiting offsets, for example, may fail.
 *
 * @remark Calling rd_kafka_assign(), rd_kafka_incremental_assign() or
 *         rd_kafka_incremental_unassign() resets this flag.
 *
 * @returns Returns 1 if the current partition assignment is considered
 *          lost, 0 otherwise.
 */
RD_EXPORT int rd_kafka_assignment_lost(rd_kafka_t *rk);


/**
 * @brief Commit offsets on broker for the provided list of partitions.
 *
 * \p offsets should contain \c topic, \c partition, \c offset and possibly
 * \c metadata. The \c offset should be the offset where consumption will
 * resume, i.e., the last processed offset + 1.
 * If \p offsets is NULL the current partition assignment will be used instead.
 *
 * If \p async is false this operation will block until the broker offset commit
 * is done, returning the resulting success or error code.
 *
 * If a rd_kafka_conf_set_offset_commit_cb() offset commit callback has been
 * configured the callback will be enqueued for a future call to
 * rd_kafka_poll(), rd_kafka_consumer_poll() or similar.
 *
 * @returns An error code indiciating if the commit was successful,
 *          or successfully scheduled if asynchronous, or failed.
 *          RD_KAFKA_RESP_ERR__FATAL is returned if the consumer has raised
 *          a fatal error.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_commit(rd_kafka_t *rk,
                const rd_kafka_topic_partition_list_t *offsets,
                int async);


/**
 * @brief Commit message's offset on broker for the message's partition.
 *        The committed offset is the message's offset + 1.
 *
 * @sa rd_kafka_commit
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_commit_message(rd_kafka_t *rk,
                        const rd_kafka_message_t *rkmessage,
                        int async);


/**
 * @brief Commit offsets on broker for the provided list of partitions.
 *
 * See rd_kafka_commit for \p offsets semantics.
 *
 * The result of the offset commit will be posted on the provided \p rkqu queue.
 *
 * If the application uses one of the poll APIs (rd_kafka_poll(),
 * rd_kafka_consumer_poll(), rd_kafka_queue_poll(), ..) to serve the queue
 * the \p cb callback is required.
 *
 * The \p commit_opaque argument is passed to the callback as \p commit_opaque,
 * or if using the event API the callback is ignored and the offset commit
 * result will be returned as an RD_KAFKA_EVENT_COMMIT event and the
 * \p commit_opaque value will be available with rd_kafka_event_opaque().
 *
 * If \p rkqu is NULL a temporary queue will be created and the callback will
 * be served by this call.
 *
 * @sa rd_kafka_commit()
 * @sa rd_kafka_conf_set_offset_commit_cb()
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_commit_queue(rd_kafka_t *rk,
                      const rd_kafka_topic_partition_list_t *offsets,
                      rd_kafka_queue_t *rkqu,
                      void (*cb)(rd_kafka_t *rk,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 void *commit_opaque),
                      void *commit_opaque);


/**
 * @brief Retrieve committed offsets for topics+partitions.
 *
 * The \p offset field of each requested partition will either be set to
 * stored offset or to RD_KAFKA_OFFSET_INVALID in case there was no stored
 * offset for that partition.
 *
 * Committed offsets will be returned according to the `isolation.level`
 * configuration property, if set to `read_committed` (default) then only
 * stable offsets for fully committed transactions will be returned, while
 * `read_uncommitted` may return offsets for not yet committed transactions.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success in which case the
 *          \p offset or \p err field of each \p partitions' element is filled
 *          in with the stored offset, or a partition specific error.
 *          Else returns an error code.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_committed(rd_kafka_t *rk,
                   rd_kafka_topic_partition_list_t *partitions,
                   int timeout_ms);



/**
 * @brief Retrieve current positions (offsets) for topics+partitions.
 *
 * The \p offset field of each requested partition will be set to the offset
 * of the last consumed message + 1, or RD_KAFKA_OFFSET_INVALID in case there
 * was no previous message.
 *
 * @remark  In this context the last consumed message is the offset consumed
 *          by the current librdkafka instance and, in case of rebalancing, not
 *          necessarily the last message fetched from the partition.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success in which case the
 *          \p offset or \p err field of each \p partitions' element is filled
 *          in with the stored offset, or a partition specific error.
 *          Else returns an error code.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_position(rd_kafka_t *rk, rd_kafka_topic_partition_list_t *partitions);



/**
 * @returns the current consumer group metadata associated with this consumer,
 *          or NULL if \p rk is not a consumer configured with a \c group.id.
 *          This metadata object should be passed to the transactional
 *          producer's rd_kafka_send_offsets_to_transaction() API.
 *
 * @remark The returned pointer must be freed by the application using
 *         rd_kafka_consumer_group_metadata_destroy().
 *
 * @sa rd_kafka_send_offsets_to_transaction()
 */
RD_EXPORT rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata(rd_kafka_t *rk);


/**
 * @brief Create a new consumer group metadata object.
 *        This is typically only used for writing tests.
 *
 * @param group_id The group id.
 *
 * @remark The returned pointer must be freed by the application using
 *         rd_kafka_consumer_group_metadata_destroy().
 */
RD_EXPORT rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata_new(const char *group_id);


/**
 * @brief Create a new consumer group metadata object.
 *        This is typically only used for writing tests.
 *
 * @param group_id The group id.
 * @param generation_id The group generation id.
 * @param member_id The group member id.
 * @param group_instance_id The group instance id (may be NULL).
 *
 * @remark The returned pointer must be freed by the application using
 *         rd_kafka_consumer_group_metadata_destroy().
 */
RD_EXPORT rd_kafka_consumer_group_metadata_t *
rd_kafka_consumer_group_metadata_new_with_genid(const char *group_id,
                                                int32_t generation_id,
                                                const char *member_id,
                                                const char *group_instance_id);


/**
 * @brief Frees the consumer group metadata object as returned by
 *        rd_kafka_consumer_group_metadata().
 */
RD_EXPORT void
rd_kafka_consumer_group_metadata_destroy(rd_kafka_consumer_group_metadata_t *);


/**
 * @brief Serialize the consumer group metadata to a binary format.
 *        This is mainly for client binding use and not for application use.
 *
 * @remark The serialized metadata format is private and is not compatible
 *         across different versions or even builds of librdkafka.
 *         It should only be used in the same process runtime and must only
 *         be passed to rd_kafka_consumer_group_metadata_read().
 *
 * @param cgmd Metadata to be serialized.
 * @param bufferp On success this pointer will be updated to point to na
 *                allocated buffer containing the serialized metadata.
 *                The buffer must be freed with rd_kafka_mem_free().
 * @param sizep The pointed to size will be updated with the size of
 *              the serialized buffer.
 *
 * @returns NULL on success or an error object on failure.
 *
 * @sa rd_kafka_consumer_group_metadata_read()
 */
RD_EXPORT rd_kafka_error_t *rd_kafka_consumer_group_metadata_write(
    const rd_kafka_consumer_group_metadata_t *cgmd,
    void **bufferp,
    size_t *sizep);

/**
 * @brief Reads serialized consumer group metadata and returns a
 *        consumer group metadata object.
 *        This is mainly for client binding use and not for application use.
 *
 * @remark The serialized metadata format is private and is not compatible
 *         across different versions or even builds of librdkafka.
 *         It should only be used in the same process runtime and must only
 *         be passed to rd_kafka_consumer_group_metadata_read().
 *
 * @param cgmdp On success this pointer will be updated to point to a new
 *              consumer group metadata object which must be freed with
 *              rd_kafka_consumer_group_metadata_destroy().
 * @param buffer Pointer to the serialized data.
 * @param size Size of the serialized data.
 *
 * @returns NULL on success or an error object on failure.
 *
 * @sa rd_kafka_consumer_group_metadata_write()
 */
RD_EXPORT rd_kafka_error_t *rd_kafka_consumer_group_metadata_read(
    rd_kafka_consumer_group_metadata_t **cgmdp,
    const void *buffer,
    size_t size);

/**@}*/



/**
 * @name Producer API
 * @{
 *
 *
 */


/**
 * @brief Producer message flags
 */
#define RD_KAFKA_MSG_F_FREE                                                    \
        0x1 /**< Delegate freeing of payload to rdkafka.                       \
             */
#define RD_KAFKA_MSG_F_COPY                                                    \
        0x2 /**< rdkafka will make a copy of the payload.                      \
             */
#define RD_KAFKA_MSG_F_BLOCK                                                   \
        0x4 /**< Block produce*() on message queue full.                       \
             *   WARNING: If a delivery report callback                        \
             *            is used, the application MUST                        \
             *            call rd_kafka_poll() (or equiv.)                     \
             *            to make sure delivered messages                      \
             *            are drained from the internal                        \
             *            delivery report queue.                               \
             *            Failure to do so will result                         \
             *            in indefinitely blocking on                          \
             *            the produce() call when the                          \
             *            message queue is full. */
#define RD_KAFKA_MSG_F_PARTITION                                               \
        0x8 /**< produce_batch() will honor                                    \
             * per-message partition. */



/**
 * @brief Produce and send a single message to broker.
 *
 * \p rkt is the target topic which must have been previously created with
 * `rd_kafka_topic_new()`.
 *
 * `rd_kafka_produce()` is an asynchronous non-blocking API.
 * See `rd_kafka_conf_set_dr_msg_cb` on how to setup a callback to be called
 * once the delivery status (success or failure) is known. The delivery report
 * is triggered by the application calling `rd_kafka_poll()` (at regular
 * intervals) or `rd_kafka_flush()` (at termination).
 *
 * Since producing is asynchronous, you should call `rd_kafka_flush()` before
 * you destroy the producer. Otherwise, any outstanding messages will be
 * silently discarded.
 *
 * When temporary errors occur, librdkafka automatically retries to produce the
 * messages. Retries are triggered after retry.backoff.ms and when the
 * leader broker for the given partition is available. Otherwise, librdkafka
 * falls back to polling the topic metadata to monitor when a new leader is
 * elected (see the topic.metadata.refresh.fast.interval.ms and
 * topic.metadata.refresh.interval.ms configurations) and then performs a
 * retry. A delivery error will occur if the message could not be produced
 * within message.timeout.ms.
 *
 * See the "Message reliability" chapter in INTRODUCTION.md for more
 * information.
 *
 * \p partition is the target partition, either:
 *   - RD_KAFKA_PARTITION_UA (unassigned) for
 *     automatic partitioning using the topic's partitioner function, or
 *   - a fixed partition (0..N)
 *
 * \p msgflags is zero or more of the following flags OR:ed together:
 *    RD_KAFKA_MSG_F_BLOCK - block \p produce*() call if
 *                           \p queue.buffering.max.messages or
 *                           \p queue.buffering.max.kbytes are exceeded.
 *                           Messages are considered in-queue from the point
 * they are accepted by produce() until their corresponding delivery report
 * callback/event returns. It is thus a requirement to call rd_kafka_poll() (or
 * equiv.) from a separate thread when F_BLOCK is used. See WARNING on \c
 * RD_KAFKA_MSG_F_BLOCK above.
 *
 *    RD_KAFKA_MSG_F_FREE - rdkafka will free(3) \p payload when it is done
 *                          with it.
 *    RD_KAFKA_MSG_F_COPY - the \p payload data will be copied and the
 *                          \p payload pointer will not be used by rdkafka
 *                          after the call returns.
 *    RD_KAFKA_MSG_F_PARTITION - produce_batch() will honour per-message
 *                               partition, either set manually or by the
 *                               configured partitioner.
 *
 *    .._F_FREE and .._F_COPY are mutually exclusive. If neither of these are
 *    set, the caller must ensure that the memory backing \p payload remains
 *    valid and is not modified or reused until the delivery callback is
 *    invoked. Other buffers passed to `rd_kafka_produce()` don't have this
 *    restriction on reuse, i.e. the memory backing the key or the topic name
 *    may be reused as soon as `rd_kafka_produce()` returns.
 *
 *    If the function returns -1 and RD_KAFKA_MSG_F_FREE was specified, then
 *    the memory associated with the payload is still the caller's
 *    responsibility.
 *
 * \p payload is the message payload of size \p len bytes.
 *
 * \p key is an optional message key of size \p keylen bytes, if non-NULL it
 * will be passed to the topic partitioner as well as be sent with the
 * message to the broker and passed on to the consumer.
 *
 * \p msg_opaque is an optional application-provided per-message opaque
 * pointer that will provided in the message's delivery report callback
 * (\c dr_msg_cb or \c dr_cb) and the \c rd_kafka_message_t \c _private field.
 *
 * @remark on_send() and on_acknowledgement() interceptors may be called
 *         from this function. on_acknowledgement() will only be called if the
 *         message fails partitioning.
 *
 * @remark If the producer is transactional (\c transactional.id is configured)
 *         producing is only allowed during an on-going transaction, namely
 *         after rd_kafka_begin_transaction() has been called.
 *
 * @returns 0 on success or -1 on error in which case errno is set accordingly:
 *  - ENOBUFS  - maximum number of outstanding messages has been reached:
 *               "queue.buffering.max.messages"
 *               (RD_KAFKA_RESP_ERR__QUEUE_FULL)
 *  - EMSGSIZE - message is larger than configured max size:
 *               "messages.max.bytes".
 *               (RD_KAFKA_RESP_ERR_MSG_SIZE_TOO_LARGE)
 *  - ESRCH    - requested \p partition is unknown in the Kafka cluster.
 *               (RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION)
 *  - ENOENT   - topic is unknown in the Kafka cluster.
 *               (RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC)
 *  - ECANCELED - fatal error has been raised on producer, see
 *                rd_kafka_fatal_error(),
 *               (RD_KAFKA_RESP_ERR__FATAL).
 *  - ENOEXEC  - transactional state forbids producing
 *               (RD_KAFKA_RESP_ERR__STATE)
 *
 * @sa Use rd_kafka_errno2err() to convert `errno` to rdkafka error code.
 */
RD_EXPORT
int rd_kafka_produce(rd_kafka_topic_t *rkt,
                     int32_t partition,
                     int msgflags,
                     void *payload,
                     size_t len,
                     const void *key,
                     size_t keylen,
                     void *msg_opaque);


/**
 * @brief Produce and send a single message to broker.
 *
 * The message is defined by a va-arg list using \c rd_kafka_vtype_t
 * tag tuples which must be terminated with a single \c RD_KAFKA_V_END.
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, else an error code as
 *          described in rd_kafka_produce().
 *          \c RD_KAFKA_RESP_ERR__CONFLICT is returned if _V_HEADER and
 *          _V_HEADERS are mixed.
 *
 * @sa rd_kafka_produce, rd_kafka_produceva, RD_KAFKA_V_END
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_producev(rd_kafka_t *rk, ...);


/**
 * @brief Produce and send a single message to broker.
 *
 * The message is defined by an array of \c rd_kafka_vu_t of
 * count \p cnt.
 *
 * @returns an error object on failure or NULL on success.
 *          See rd_kafka_producev() for specific error codes.
 *
 * @sa rd_kafka_produce, rd_kafka_producev, RD_KAFKA_V_END
 */
RD_EXPORT
rd_kafka_error_t *
rd_kafka_produceva(rd_kafka_t *rk, const rd_kafka_vu_t *vus, size_t cnt);


/**
 * @brief Produce multiple messages.
 *
 * If partition is RD_KAFKA_PARTITION_UA the configured partitioner will
 * be run for each message (slower), otherwise the messages will be enqueued
 * to the specified partition directly (faster).
 *
 * The messages are provided in the array \p rkmessages of count \p message_cnt
 * elements.
 * The \p partition and \p msgflags are used for all provided messages.
 *
 * Honoured \p rkmessages[] fields are:
 *  - payload,len    Message payload and length
 *  - key,key_len    Optional message key
 *  - _private       Message opaque pointer (msg_opaque)
 *  - err            Will be set according to success or failure, see
 *                   rd_kafka_produce() for possible error codes.
 *                   Application only needs to check for errors if
 *                   return value != \p message_cnt.
 *
 * @remark If \c RD_KAFKA_MSG_F_PARTITION is set in \p msgflags, the
 *         \c .partition field of the \p rkmessages is used instead of
 *         \p partition.
 *
 * @returns the number of messages succesfully enqueued for producing.
 *
 * @remark This interface does NOT support setting message headers on
 *         the provided \p rkmessages.
 */
RD_EXPORT
int rd_kafka_produce_batch(rd_kafka_topic_t *rkt,
                           int32_t partition,
                           int msgflags,
                           rd_kafka_message_t *rkmessages,
                           int message_cnt);



/**
 * @brief Wait until all outstanding produce requests, et.al, are completed.
 *        This should typically be done prior to destroying a producer instance
 *        to make sure all queued and in-flight produce requests are completed
 *        before terminating.
 *
 * @remark This function will call rd_kafka_poll() and thus trigger callbacks.
 *
 * @remark The \c linger.ms time will be ignored for the duration of the call,
 *         queued messages will be sent to the broker as soon as possible.
 *
 * @remark If RD_KAFKA_EVENT_DR has been enabled
 *         (through rd_kafka_conf_set_events()) this function will not call
 *         rd_kafka_poll() but instead wait for the librdkafka-handled
 *         message count to reach zero. This requires the application to
 *         serve the event queue in a separate thread.
 *         In this mode only messages are counted, not other types of
 *         queued events.
 *
 * @returns RD_KAFKA_RESP_ERR__TIMED_OUT if \p timeout_ms was reached before all
 *          outstanding requests were completed, else RD_KAFKA_RESP_ERR_NO_ERROR
 *
 * @sa rd_kafka_outq_len()
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_flush(rd_kafka_t *rk, int timeout_ms);



/**
 * @brief Purge messages currently handled by the producer instance.
 *
 * @param rk          Client instance.
 * @param purge_flags Tells which messages to purge and how.
 *
 * The application will need to call rd_kafka_poll() or rd_kafka_flush()
 * afterwards to serve the delivery report callbacks of the purged messages.
 *
 * Messages purged from internal queues fail with the delivery report
 * error code set to RD_KAFKA_RESP_ERR__PURGE_QUEUE, while purged messages that
 * are in-flight to or from the broker will fail with the error code set to
 * RD_KAFKA_RESP_ERR__PURGE_INFLIGHT.
 *
 * @warning Purging messages that are in-flight to or from the broker
 *          will ignore any subsequent acknowledgement for these messages
 *          received from the broker, effectively making it impossible
 *          for the application to know if the messages were successfully
 *          produced or not. This may result in duplicate messages if the
 *          application retries these messages at a later time.
 *
 * @remark This call may block for a short time while background thread
 *         queues are purged.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if the \p purge flags are invalid
 *          or unknown,
 *          RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED if called on a non-producer
 *          client instance.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_purge(rd_kafka_t *rk, int purge_flags);


/**
 * @brief Flags for rd_kafka_purge()
 */

/*!
 * Purge messages in internal queues.
 */
#define RD_KAFKA_PURGE_F_QUEUE 0x1

/*!
 * Purge messages in-flight to or from the broker.
 * Purging these messages will void any future acknowledgements from the
 * broker, making it impossible for the application to know if these
 * messages were successfully delivered or not.
 * Retrying these messages may lead to duplicates.
 */
#define RD_KAFKA_PURGE_F_INFLIGHT 0x2


/*!
 * Don't wait for background thread queue purging to finish.
 */
#define RD_KAFKA_PURGE_F_NON_BLOCKING 0x4


/**@}*/


/**
 * @name Metadata API
 * @{
 *
 *
 */


/**
 * @brief Broker information
 */
typedef struct rd_kafka_metadata_broker {
        int32_t id; /**< Broker Id */
        char *host; /**< Broker hostname */
        int port;   /**< Broker listening port */
} rd_kafka_metadata_broker_t;

/**
 * @brief Partition information
 */
typedef struct rd_kafka_metadata_partition {
        int32_t id;              /**< Partition Id */
        rd_kafka_resp_err_t err; /**< Partition error reported by broker */
        int32_t leader;          /**< Leader broker */
        int replica_cnt;         /**< Number of brokers in \p replicas */
        int32_t *replicas;       /**< Replica brokers */
        int isr_cnt;             /**< Number of ISR brokers in \p isrs */
        int32_t *isrs;           /**< In-Sync-Replica brokers */
} rd_kafka_metadata_partition_t;

/**
 * @brief Topic information
 */
typedef struct rd_kafka_metadata_topic {
        char *topic;       /**< Topic name */
        int partition_cnt; /**< Number of partitions in \p partitions*/
        struct rd_kafka_metadata_partition *partitions; /**< Partitions */
        rd_kafka_resp_err_t err; /**< Topic error reported by broker */
} rd_kafka_metadata_topic_t;


/**
 * @brief Metadata container
 */
typedef struct rd_kafka_metadata {
        int broker_cnt; /**< Number of brokers in \p brokers */
        struct rd_kafka_metadata_broker *brokers; /**< Brokers */

        int topic_cnt; /**< Number of topics in \p topics */
        struct rd_kafka_metadata_topic *topics; /**< Topics */

        int32_t orig_broker_id; /**< Broker originating this metadata */
        char *orig_broker_name; /**< Name of originating broker */
} rd_kafka_metadata_t;

/**
 * @brief Request Metadata from broker.
 *
 * Parameters:
 *  - \p all_topics  if non-zero: request info about all topics in cluster,
 *                   if zero: only request info about locally known topics.
 *  - \p only_rkt    only request info about this topic
 *  - \p metadatap   pointer to hold metadata result.
 *                   The \p *metadatap pointer must be released
 *                   with rd_kafka_metadata_destroy().
 *  - \p timeout_ms  maximum response time before failing.
 *
 * @remark Consumer: If \p all_topics is non-zero the Metadata response
 *         information may trigger a re-join if any subscribed topics
 *         have changed partition count or existence state.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success (in which case *metadatap)
 *          will be set, else RD_KAFKA_RESP_ERR__TIMED_OUT on timeout or
 *          other error code on error.
 */
RD_EXPORT
rd_kafka_resp_err_t
rd_kafka_metadata(rd_kafka_t *rk,
                  int all_topics,
                  rd_kafka_topic_t *only_rkt,
                  const struct rd_kafka_metadata **metadatap,
                  int timeout_ms);

/**
 * @brief Release metadata memory.
 */
RD_EXPORT
void rd_kafka_metadata_destroy(const struct rd_kafka_metadata *metadata);

/**
 * @brief Node (broker) information.
 */
typedef struct rd_kafka_Node_s rd_kafka_Node_t;

/**
 * @brief Get the id of \p node.
 *
 * @param node The Node instance.
 *
 * @return The node id.
 */
RD_EXPORT
int rd_kafka_Node_id(const rd_kafka_Node_t *node);

/**
 * @brief Get the host of \p node.
 *
 * @param node The Node instance.
 *
 * @return The node host.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p node object.
 */
RD_EXPORT
const char *rd_kafka_Node_host(const rd_kafka_Node_t *node);

/**
 * @brief Get the port of \p node.
 *
 * @param node The Node instance.
 *
 * @return The node port.
 */
RD_EXPORT
uint16_t rd_kafka_Node_port(const rd_kafka_Node_t *node);

/**@}*/



/**
 * @name Client group information
 * @{
 *
 *
 */


/**
 * @brief Group member information
 *
 * For more information on \p member_metadata format, see
 * https://cwiki.apache.org/confluence/display/KAFKA/A+Guide+To+The+Kafka+Protocol#AGuideToTheKafkaProtocol-GroupMembershipAPI
 *
 */
struct rd_kafka_group_member_info {
        char *member_id;            /**< Member id (generated by broker) */
        char *client_id;            /**< Client's \p client.id */
        char *client_host;          /**< Client's hostname */
        void *member_metadata;      /**< Member metadata (binary),
                                     *   format depends on \p protocol_type. */
        int member_metadata_size;   /**< Member metadata size in bytes */
        void *member_assignment;    /**< Member assignment (binary),
                                     *    format depends on \p protocol_type. */
        int member_assignment_size; /**< Member assignment size in bytes */
};

/**
 * @enum rd_kafka_consumer_group_state_t
 *
 * @brief Consumer group state.
 */
typedef enum {
        RD_KAFKA_CONSUMER_GROUP_STATE_UNKNOWN              = 0,
        RD_KAFKA_CONSUMER_GROUP_STATE_PREPARING_REBALANCE  = 1,
        RD_KAFKA_CONSUMER_GROUP_STATE_COMPLETING_REBALANCE = 2,
        RD_KAFKA_CONSUMER_GROUP_STATE_STABLE               = 3,
        RD_KAFKA_CONSUMER_GROUP_STATE_DEAD                 = 4,
        RD_KAFKA_CONSUMER_GROUP_STATE_EMPTY                = 5,
        RD_KAFKA_CONSUMER_GROUP_STATE__CNT
} rd_kafka_consumer_group_state_t;

/**
 * @brief Group information
 */
struct rd_kafka_group_info {
        struct rd_kafka_metadata_broker broker; /**< Originating broker info */
        char *group;                            /**< Group name */
        rd_kafka_resp_err_t err;                /**< Broker-originated error */
        char *state;                            /**< Group state */
        char *protocol_type;                    /**< Group protocol type */
        char *protocol;                         /**< Group protocol */
        struct rd_kafka_group_member_info *members; /**< Group members */
        int member_cnt;                             /**< Group member count */
};

/**
 * @brief List of groups
 *
 * @sa rd_kafka_group_list_destroy() to release list memory.
 */
struct rd_kafka_group_list {
        struct rd_kafka_group_info *groups; /**< Groups */
        int group_cnt;                      /**< Group count */
};


/**
 * @brief List and describe client groups in cluster.
 *
 * \p group is an optional group name to describe, otherwise (\c NULL) all
 * groups are returned.
 *
 * \p timeout_ms is the (approximate) maximum time to wait for response
 * from brokers and must be a positive value.
 *
 * @returns \c RD_KAFKA_RESP_ERR__NO_ERROR on success and \p grplistp is
 *           updated to point to a newly allocated list of groups.
 *           \c RD_KAFKA_RESP_ERR__PARTIAL if not all brokers responded
 *           in time but at least one group is returned in  \p grplistlp.
 *           \c RD_KAFKA_RESP_ERR__TIMED_OUT if no groups were returned in the
 *           given timeframe but not all brokers have yet responded, or
 *           if the list of brokers in the cluster could not be obtained within
 *           the given timeframe.
 *           \c RD_KAFKA_RESP_ERR__TRANSPORT if no brokers were found.
 *           Other error codes may also be returned from the request layer.
 *
 *           The \p grplistp remains untouched if any error code is returned,
 *           with the exception of RD_KAFKA_RESP_ERR__PARTIAL which behaves
 *           as RD_KAFKA_RESP_ERR__NO_ERROR (success) but with an incomplete
 *           group list.
 *
 * @sa Use rd_kafka_group_list_destroy() to release list memory.
 *
 * @deprecated Use rd_kafka_ListConsumerGroups() and
 *             rd_kafka_DescribeConsumerGroups() instead.
 */
RD_EXPORT
rd_kafka_resp_err_t
rd_kafka_list_groups(rd_kafka_t *rk,
                     const char *group,
                     const struct rd_kafka_group_list **grplistp,
                     int timeout_ms);

/**
 * @brief Returns a name for a state code.
 *
 * @param state The state value.
 *
 * @return The group state name corresponding to the provided group state value.
 */
RD_EXPORT
const char *
rd_kafka_consumer_group_state_name(rd_kafka_consumer_group_state_t state);

/**
 * @brief Returns a code for a state name.
 *
 * @param name The state name.
 *
 * @return The group state value corresponding to the provided group state name.
 */
RD_EXPORT
rd_kafka_consumer_group_state_t
rd_kafka_consumer_group_state_code(const char *name);

/**
 * @brief Release list memory
 */
RD_EXPORT
void rd_kafka_group_list_destroy(const struct rd_kafka_group_list *grplist);


/**@}*/



/**
 * @name Miscellaneous APIs
 * @{
 *
 */


/**
 * @brief Adds one or more brokers to the kafka handle's list of initial
 *        bootstrap brokers.
 *
 * Additional brokers will be discovered automatically as soon as rdkafka
 * connects to a broker by querying the broker metadata.
 *
 * If a broker name resolves to multiple addresses (and possibly
 * address families) all will be used for connection attempts in
 * round-robin fashion.
 *
 * \p brokerlist is a ,-separated list of brokers in the format:
 *   \c \<broker1\>,\<broker2\>,..
 * Where each broker is in either the host or URL based format:
 *   \c \<host\>[:\<port\>]
 *   \c \<proto\>://\<host\>[:port]
 * \c \<proto\> is either \c PLAINTEXT, \c SSL, \c SASL, \c SASL_PLAINTEXT
 * The two formats can be mixed but ultimately the value of the
 * `security.protocol` config property decides what brokers are allowed.
 *
 * Example:
 *    brokerlist = "broker1:10000,broker2"
 *    brokerlist = "SSL://broker3:9000,ssl://broker2"
 *
 * @returns the number of brokers successfully added.
 *
 * @remark Brokers may also be defined with the \c metadata.broker.list or
 *         \c bootstrap.servers configuration property (preferred method).
 *
 * @deprecated Set bootstrap servers with the \c bootstrap.servers
 *             configuration property.
 */
RD_EXPORT
int rd_kafka_brokers_add(rd_kafka_t *rk, const char *brokerlist);



/**
 * @brief Set logger function.
 *
 * The default is to print to stderr, but a syslog logger is also available,
 * see rd_kafka_log_(print|syslog) for the builtin alternatives.
 * Alternatively the application may provide its own logger callback.
 * Or pass 'func' as NULL to disable logging.
 *
 * @deprecated Use rd_kafka_conf_set_log_cb()
 *
 * @remark \p rk may be passed as NULL in the callback.
 */
RD_EXPORT RD_DEPRECATED void
rd_kafka_set_logger(rd_kafka_t *rk,
                    void (*func)(const rd_kafka_t *rk,
                                 int level,
                                 const char *fac,
                                 const char *buf));


/**
 * @brief Specifies the maximum logging level emitted by
 *        internal kafka logging and debugging.
 *
 * @deprecated Set the \c "log_level" configuration property instead.
 *
 * @remark If the \p \"debug\" configuration property is set the log level is
 *         automatically adjusted to \c LOG_DEBUG (7).
 */
RD_EXPORT
void rd_kafka_set_log_level(rd_kafka_t *rk, int level);


/**
 * @brief Builtin (default) log sink: print to stderr
 */
RD_EXPORT
void rd_kafka_log_print(const rd_kafka_t *rk,
                        int level,
                        const char *fac,
                        const char *buf);


/**
 * @brief Builtin log sink: print to syslog.
 * @remark This logger is only available if librdkafka was built
 *         with syslog support.
 */
RD_EXPORT
void rd_kafka_log_syslog(const rd_kafka_t *rk,
                         int level,
                         const char *fac,
                         const char *buf);


/**
 * @brief Returns the current out queue length.
 *
 * The out queue length is the sum of:
 *  - number of messages waiting to be sent to, or acknowledged by,
 *    the broker.
 *  - number of delivery reports (e.g., dr_msg_cb) waiting to be served
 *    by rd_kafka_poll() or rd_kafka_flush().
 *  - number of callbacks (e.g., error_cb, stats_cb, etc) waiting to be
 *    served by rd_kafka_poll(), rd_kafka_consumer_poll() or rd_kafka_flush().
 *  - number of events waiting to be served by background_event_cb() in
 *    the background queue (see rd_kafka_conf_set_background_event_cb).
 *
 * An application should wait for the return value of this function to reach
 * zero before terminating to make sure outstanding messages,
 * requests (such as offset commits), callbacks and events are fully processed.
 * See rd_kafka_flush().
 *
 * @returns number of messages and events waiting in queues.
 *
 * @sa rd_kafka_flush()
 */
RD_EXPORT
int rd_kafka_outq_len(rd_kafka_t *rk);



/**
 * @brief Dumps rdkafka's internal state for handle \p rk to stream \p fp
 *
 * This is only useful for debugging rdkafka, showing state and statistics
 * for brokers, topics, partitions, etc.
 */
RD_EXPORT
void rd_kafka_dump(FILE *fp, rd_kafka_t *rk);



/**
 * @brief Retrieve the current number of threads in use by librdkafka.
 *
 * Used by regression tests.
 */
RD_EXPORT
int rd_kafka_thread_cnt(void);


/**
 * @enum rd_kafka_thread_type_t
 *
 * @brief librdkafka internal thread type.
 *
 * @sa rd_kafka_interceptor_add_on_thread_start()
 */
typedef enum rd_kafka_thread_type_t {
        RD_KAFKA_THREAD_MAIN,       /**< librdkafka's internal main thread */
        RD_KAFKA_THREAD_BACKGROUND, /**< Background thread (if enabled) */
        RD_KAFKA_THREAD_BROKER      /**< Per-broker thread */
} rd_kafka_thread_type_t;


/**
 * @brief Wait for all rd_kafka_t objects to be destroyed.
 *
 * Returns 0 if all kafka objects are now destroyed, or -1 if the
 * timeout was reached.
 *
 * @remark This function is deprecated.
 */
RD_EXPORT
int rd_kafka_wait_destroyed(int timeout_ms);


/**
 * @brief Run librdkafka's built-in unit-tests.
 *
 * @returns the number of failures, or 0 if all tests passed.
 */
RD_EXPORT
int rd_kafka_unittest(void);


/**@}*/



/**
 * @name Experimental APIs
 * @{
 */

/**
 * @brief Redirect the main (rd_kafka_poll()) queue to the KafkaConsumer's
 *        queue (rd_kafka_consumer_poll()).
 *
 * @warning It is not permitted to call rd_kafka_poll() after directing the
 *          main queue with rd_kafka_poll_set_consumer().
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_poll_set_consumer(rd_kafka_t *rk);


/**@}*/

/**
 * @name Event interface
 *
 * @brief The event API provides an alternative pollable non-callback interface
 *        to librdkafka's message and event queues.
 *
 * @{
 */


/**
 * @brief Event types
 */
typedef int rd_kafka_event_type_t;
#define RD_KAFKA_EVENT_NONE                0x0 /**< Unset value */
#define RD_KAFKA_EVENT_DR                  0x1 /**< Producer Delivery report batch */
#define RD_KAFKA_EVENT_FETCH               0x2 /**< Fetched message (consumer) */
#define RD_KAFKA_EVENT_LOG                 0x4 /**< Log message */
#define RD_KAFKA_EVENT_ERROR               0x8 /**< Error */
#define RD_KAFKA_EVENT_REBALANCE           0x10 /**< Group rebalance (consumer) */
#define RD_KAFKA_EVENT_OFFSET_COMMIT       0x20 /**< Offset commit result */
#define RD_KAFKA_EVENT_STATS               0x40 /**< Stats */
#define RD_KAFKA_EVENT_CREATETOPICS_RESULT 100  /**< CreateTopics_result_t */
#define RD_KAFKA_EVENT_DELETETOPICS_RESULT 101  /**< DeleteTopics_result_t */
#define RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT                                 \
        102                                    /**< CreatePartitions_result_t */
#define RD_KAFKA_EVENT_ALTERCONFIGS_RESULT 103 /**< AlterConfigs_result_t */
#define RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT                                  \
        104                                     /**< DescribeConfigs_result_t */
#define RD_KAFKA_EVENT_DELETERECORDS_RESULT 105 /**< DeleteRecords_result_t */
#define RD_KAFKA_EVENT_DELETEGROUPS_RESULT  106 /**< DeleteGroups_result_t */
/** DeleteConsumerGroupOffsets_result_t */
#define RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT 107
/** SASL/OAUTHBEARER token needs to be refreshed */
#define RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH 0x100
#define RD_KAFKA_EVENT_BACKGROUND                0x200 /**< Enable background thread. */
#define RD_KAFKA_EVENT_CREATEACLS_RESULT         0x400 /**< CreateAcls_result_t */
#define RD_KAFKA_EVENT_DESCRIBEACLS_RESULT       0x800 /**< DescribeAcls_result_t */
#define RD_KAFKA_EVENT_DELETEACLS_RESULT         0x1000 /**< DeleteAcls_result_t */
/** ListConsumerGroupsResult_t */
#define RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT 0x2000
/** DescribeConsumerGroups_result_t */
#define RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT 0x4000
/** ListConsumerGroupOffsets_result_t */
#define RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT 0x8000
/** AlterConsumerGroupOffsets_result_t */
#define RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT 0x10000


/**
 * @returns the event type for the given event.
 *
 * @remark As a convenience it is okay to pass \p rkev as NULL in which case
 *         RD_KAFKA_EVENT_NONE is returned.
 */
RD_EXPORT
rd_kafka_event_type_t rd_kafka_event_type(const rd_kafka_event_t *rkev);

/**
 * @returns the event type's name for the given event.
 *
 * @remark As a convenience it is okay to pass \p rkev as NULL in which case
 *         the name for RD_KAFKA_EVENT_NONE is returned.
 */
RD_EXPORT
const char *rd_kafka_event_name(const rd_kafka_event_t *rkev);


/**
 * @brief Destroy an event.
 *
 * @remark Any references to this event, such as extracted messages,
 *         will not be usable after this call.
 *
 * @remark As a convenience it is okay to pass \p rkev as NULL in which case
 *         no action is performed.
 */
RD_EXPORT
void rd_kafka_event_destroy(rd_kafka_event_t *rkev);


/**
 * @returns the next message from an event.
 *
 * Call repeatedly until it returns NULL.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_FETCH  (1 message)
 *  - RD_KAFKA_EVENT_DR     (>=1 message(s))
 *
 * @remark The returned message(s) MUST NOT be
 *         freed with rd_kafka_message_destroy().
 *
 * @remark on_consume() interceptor may be called
 *         from this function prior to passing message to application.
 */
RD_EXPORT
const rd_kafka_message_t *rd_kafka_event_message_next(rd_kafka_event_t *rkev);


/**
 * @brief Extacts \p size message(s) from the event into the
 *        pre-allocated array \p rkmessages.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_FETCH  (1 message)
 *  - RD_KAFKA_EVENT_DR     (>=1 message(s))
 *
 * @returns the number of messages extracted.
 *
 * @remark on_consume() interceptor may be called
 *         from this function prior to passing message to application.
 */
RD_EXPORT
size_t rd_kafka_event_message_array(rd_kafka_event_t *rkev,
                                    const rd_kafka_message_t **rkmessages,
                                    size_t size);


/**
 * @returns the number of remaining messages in the event.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_FETCH  (1 message)
 *  - RD_KAFKA_EVENT_DR     (>=1 message(s))
 */
RD_EXPORT
size_t rd_kafka_event_message_count(rd_kafka_event_t *rkev);


/**
 * @returns the associated configuration string for the event, or NULL
 *          if the configuration property is not set or if
 *          not applicable for the given event type.
 *
 * The returned memory is read-only and its lifetime is the same as the
 * event object.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_OAUTHBEARER_TOKEN_REFRESH: value of sasl.oauthbearer.config
 */
RD_EXPORT
const char *rd_kafka_event_config_string(rd_kafka_event_t *rkev);


/**
 * @returns the error code for the event.
 *
 * Use rd_kafka_event_error_is_fatal() to detect if this is a fatal error.
 *
 * Event types:
 *  - all
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_event_error(rd_kafka_event_t *rkev);


/**
 * @returns the error string (if any).
 *          An application should check that rd_kafka_event_error() returns
 *          non-zero before calling this function.
 *
 * Event types:
 *  - all
 */
RD_EXPORT
const char *rd_kafka_event_error_string(rd_kafka_event_t *rkev);


/**
 * @returns 1 if the error is a fatal error, else 0.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_ERROR
 *
 * @sa rd_kafka_fatal_error()
 */
RD_EXPORT
int rd_kafka_event_error_is_fatal(rd_kafka_event_t *rkev);


/**
 * @returns the event opaque (if any) as passed to rd_kafka_commit() (et.al) or
 *          rd_kafka_AdminOptions_set_opaque(), depending on event type.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_OFFSET_COMMIT
 *  - RD_KAFKA_EVENT_CREATETOPICS_RESULT
 *  - RD_KAFKA_EVENT_DELETETOPICS_RESULT
 *  - RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT
 *  - RD_KAFKA_EVENT_CREATEACLS_RESULT
 *  - RD_KAFKA_EVENT_DESCRIBEACLS_RESULT
 *  - RD_KAFKA_EVENT_DELETEACLS_RESULT
 *  - RD_KAFKA_EVENT_ALTERCONFIGS_RESULT
 *  - RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT
 *  - RD_KAFKA_EVENT_DELETEGROUPS_RESULT
 *  - RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT
 *  - RD_KAFKA_EVENT_DELETERECORDS_RESULT
 *  - RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT
 *  - RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT
 *  - RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT
 *  - RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT
 */
RD_EXPORT
void *rd_kafka_event_opaque(rd_kafka_event_t *rkev);


/**
 * @brief Extract log message from the event.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_LOG
 *
 * @returns 0 on success or -1 if unsupported event type.
 */
RD_EXPORT
int rd_kafka_event_log(rd_kafka_event_t *rkev,
                       const char **fac,
                       const char **str,
                       int *level);


/**
 * @brief Extract log debug context from event.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_LOG
 *
 *  @param rkev the event to extract data from.
 *  @param dst destination string for comma separated list.
 *  @param dstsize size of provided dst buffer.
 *  @returns 0 on success or -1 if unsupported event type.
 */
RD_EXPORT
int rd_kafka_event_debug_contexts(rd_kafka_event_t *rkev,
                                  char *dst,
                                  size_t dstsize);


/**
 * @brief Extract stats from the event.
 *
 * Event types:
 *  - RD_KAFKA_EVENT_STATS
 *
 * @returns stats json string.
 *
 * @remark the returned string will be freed automatically along with the event
 * object
 *
 */
RD_EXPORT
const char *rd_kafka_event_stats(rd_kafka_event_t *rkev);


/**
 * @returns the topic partition list from the event.
 *
 * @remark The list MUST NOT be freed with
 * rd_kafka_topic_partition_list_destroy()
 *
 * Event types:
 *  - RD_KAFKA_EVENT_REBALANCE
 *  - RD_KAFKA_EVENT_OFFSET_COMMIT
 */
RD_EXPORT rd_kafka_topic_partition_list_t *
rd_kafka_event_topic_partition_list(rd_kafka_event_t *rkev);


/**
 * @returns a newly allocated topic_partition container, if applicable for the
 * event type, else NULL.
 *
 * @remark The returned pointer MUST be freed with
 * rd_kafka_topic_partition_destroy().
 *
 * Event types:
 *   RD_KAFKA_EVENT_ERROR  (for partition level errors)
 */
RD_EXPORT rd_kafka_topic_partition_t *
rd_kafka_event_topic_partition(rd_kafka_event_t *rkev);


/*! CreateTopics result type */
typedef rd_kafka_event_t rd_kafka_CreateTopics_result_t;
/*! DeleteTopics result type */
typedef rd_kafka_event_t rd_kafka_DeleteTopics_result_t;
/*! CreateAcls result type */
typedef rd_kafka_event_t rd_kafka_CreateAcls_result_t;
/*! DescribeAcls result type */
typedef rd_kafka_event_t rd_kafka_DescribeAcls_result_t;
/*! DeleteAcls result type */
typedef rd_kafka_event_t rd_kafka_DeleteAcls_result_t;
/*! CreatePartitions result type */
typedef rd_kafka_event_t rd_kafka_CreatePartitions_result_t;
/*! AlterConfigs result type */
typedef rd_kafka_event_t rd_kafka_AlterConfigs_result_t;
/*! CreateTopics result type */
typedef rd_kafka_event_t rd_kafka_DescribeConfigs_result_t;
/*! DeleteRecords result type */
typedef rd_kafka_event_t rd_kafka_DeleteRecords_result_t;
/*! ListConsumerGroups result type */
typedef rd_kafka_event_t rd_kafka_ListConsumerGroups_result_t;
/*! DescribeConsumerGroups result type */
typedef rd_kafka_event_t rd_kafka_DescribeConsumerGroups_result_t;
/*! DeleteGroups result type */
typedef rd_kafka_event_t rd_kafka_DeleteGroups_result_t;
/*! DeleteConsumerGroupOffsets result type */
typedef rd_kafka_event_t rd_kafka_DeleteConsumerGroupOffsets_result_t;
/*! AlterConsumerGroupOffsets result type */
typedef rd_kafka_event_t rd_kafka_AlterConsumerGroupOffsets_result_t;
/*! ListConsumerGroupOffsets result type */
typedef rd_kafka_event_t rd_kafka_ListConsumerGroupOffsets_result_t;

/**
 * @brief Get CreateTopics result.
 *
 * @returns the result of a CreateTopics request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_CREATETOPICS_RESULT
 */
RD_EXPORT const rd_kafka_CreateTopics_result_t *
rd_kafka_event_CreateTopics_result(rd_kafka_event_t *rkev);

/**
 * @brief Get DeleteTopics result.
 *
 * @returns the result of a DeleteTopics request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DELETETOPICS_RESULT
 */
RD_EXPORT const rd_kafka_DeleteTopics_result_t *
rd_kafka_event_DeleteTopics_result(rd_kafka_event_t *rkev);

/**
 * @brief Get CreatePartitions result.
 *
 * @returns the result of a CreatePartitions request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT
 */
RD_EXPORT const rd_kafka_CreatePartitions_result_t *
rd_kafka_event_CreatePartitions_result(rd_kafka_event_t *rkev);

/**
 * @brief Get AlterConfigs result.
 *
 * @returns the result of a AlterConfigs request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_ALTERCONFIGS_RESULT
 */
RD_EXPORT const rd_kafka_AlterConfigs_result_t *
rd_kafka_event_AlterConfigs_result(rd_kafka_event_t *rkev);

/**
 * @brief Get DescribeConfigs result.
 *
 * @returns the result of a DescribeConfigs request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DESCRIBECONFIGS_RESULT
 */
RD_EXPORT const rd_kafka_DescribeConfigs_result_t *
rd_kafka_event_DescribeConfigs_result(rd_kafka_event_t *rkev);

/**
 * @returns the result of a DeleteRecords request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DELETERECORDS_RESULT
 */
RD_EXPORT const rd_kafka_DeleteRecords_result_t *
rd_kafka_event_DeleteRecords_result(rd_kafka_event_t *rkev);

/**
 * @brief Get ListConsumerGroups result.
 *
 * @returns the result of a ListConsumerGroups request, or NULL if event is of
 *          different type.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p rkev object.
 *
 * Event types:
 *   RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT
 */
RD_EXPORT const rd_kafka_ListConsumerGroups_result_t *
rd_kafka_event_ListConsumerGroups_result(rd_kafka_event_t *rkev);

/**
 * @brief Get DescribeConsumerGroups result.
 *
 * @returns the result of a DescribeConsumerGroups request, or NULL if event is
 * of different type.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p rkev object.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT
 */
RD_EXPORT const rd_kafka_DescribeConsumerGroups_result_t *
rd_kafka_event_DescribeConsumerGroups_result(rd_kafka_event_t *rkev);

/**
 * @brief Get DeleteGroups result.
 *
 * @returns the result of a DeleteGroups request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DELETEGROUPS_RESULT
 */
RD_EXPORT const rd_kafka_DeleteGroups_result_t *
rd_kafka_event_DeleteGroups_result(rd_kafka_event_t *rkev);

/**
 * @brief Get DeleteConsumerGroupOffsets result.
 *
 * @returns the result of a DeleteConsumerGroupOffsets request, or NULL if
 *          event is of different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT
 */
RD_EXPORT const rd_kafka_DeleteConsumerGroupOffsets_result_t *
rd_kafka_event_DeleteConsumerGroupOffsets_result(rd_kafka_event_t *rkev);

/**
 * @returns the result of a CreateAcls request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_CREATEACLS_RESULT
 */
RD_EXPORT const rd_kafka_CreateAcls_result_t *
rd_kafka_event_CreateAcls_result(rd_kafka_event_t *rkev);

/**
 * @returns the result of a DescribeAcls request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DESCRIBEACLS_RESULT
 */
RD_EXPORT const rd_kafka_DescribeAcls_result_t *
rd_kafka_event_DescribeAcls_result(rd_kafka_event_t *rkev);

/**
 * @returns the result of a DeleteAcls request, or NULL if event is of
 *          different type.
 *
 * Event types:
 *   RD_KAFKA_EVENT_DELETEACLS_RESULT
 */
RD_EXPORT const rd_kafka_DeleteAcls_result_t *
rd_kafka_event_DeleteAcls_result(rd_kafka_event_t *rkev);

/**
 * @brief Get AlterConsumerGroupOffsets result.
 *
 * @returns the result of a AlterConsumerGroupOffsets request, or NULL if
 *          event is of different type.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p rkev object.
 *
 * Event types:
 *   RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT
 */
RD_EXPORT const rd_kafka_AlterConsumerGroupOffsets_result_t *
rd_kafka_event_AlterConsumerGroupOffsets_result(rd_kafka_event_t *rkev);

/**
 * @brief Get ListConsumerGroupOffsets result.
 *
 * @returns the result of a ListConsumerGroupOffsets request, or NULL if
 *          event is of different type.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p rkev object.
 *
 * Event types:
 *   RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT
 */
RD_EXPORT const rd_kafka_ListConsumerGroupOffsets_result_t *
rd_kafka_event_ListConsumerGroupOffsets_result(rd_kafka_event_t *rkev);

/**
 * @brief Poll a queue for an event for max \p timeout_ms.
 *
 * @returns an event, or NULL.
 *
 * @remark Use rd_kafka_event_destroy() to free the event.
 *
 * @sa rd_kafka_conf_set_background_event_cb()
 */
RD_EXPORT
rd_kafka_event_t *rd_kafka_queue_poll(rd_kafka_queue_t *rkqu, int timeout_ms);

/**
 * @brief Poll a queue for events served through callbacks for max \p
 * timeout_ms.
 *
 * @returns the number of events served.
 *
 * @remark This API must only be used for queues with callbacks registered
 *         for all expected event types. E.g., not a message queue.
 *
 * @remark Also see rd_kafka_conf_set_background_event_cb() for triggering
 *         event callbacks from a librdkafka-managed background thread.
 *
 * @sa rd_kafka_conf_set_background_event_cb()
 */
RD_EXPORT
int rd_kafka_queue_poll_callback(rd_kafka_queue_t *rkqu, int timeout_ms);


/**@}*/


/**
 * @name Plugin interface
 *
 * @brief A plugin interface that allows external runtime-loaded libraries
 *        to integrate with a client instance without modifications to
 *        the application code.
 *
 *        Plugins are loaded when referenced through the `plugin.library.paths`
 *        configuration property and operates on the \c rd_kafka_conf_t
 *        object prior \c rd_kafka_t instance creation.
 *
 * @warning Plugins require the application to link librdkafka dynamically
 *          and not statically. Failure to do so will lead to missing symbols
 *          or finding symbols in another librdkafka library than the
 *          application was linked with.
 * @{
 */


/**
 * @brief Plugin's configuration initializer method called each time the
 *        library is referenced from configuration (even if previously loaded by
 *        another client instance).
 *
 * @remark This method MUST be implemented by plugins and have the symbol name
 *         \c conf_init
 *
 * @param conf Configuration set up to this point.
 * @param plug_opaquep Plugin can set this pointer to a per-configuration
 *                     opaque pointer.
 * @param errstr String buffer of size \p errstr_size where plugin must write
 *               a human readable error string in the case the initializer
 *               fails (returns non-zero).
 * @param errstr_size Maximum space (including \0) in \p errstr.
 *
 * @remark A plugin may add an on_conf_destroy() interceptor to clean up
 *         plugin-specific resources created in the plugin's conf_init() method.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code on error.
 */
typedef rd_kafka_resp_err_t(rd_kafka_plugin_f_conf_init_t)(
    rd_kafka_conf_t *conf,
    void **plug_opaquep,
    char *errstr,
    size_t errstr_size);

/**@}*/



/**
 * @name Interceptors
 *
 * @{
 *
 * @brief A callback interface that allows message interception for both
 *        producer and consumer data pipelines.
 *
 * Except for the on_new(), on_conf_set(), on_conf_dup() and on_conf_destroy()
 * interceptors, interceptors are added to the
 * newly created rd_kafka_t client instance. These interceptors MUST only
 * be added from on_new() and MUST NOT be added after rd_kafka_new() returns.
 *
 * The on_new(), on_conf_set(), on_conf_dup() and on_conf_destroy() interceptors
 * are added to the configuration object which is later passed to
 * rd_kafka_new() where on_new() is called to allow addition of
 * other interceptors.
 *
 * Each interceptor reference consists of a display name (ic_name),
 * a callback function, and an application-specified opaque value that is
 * passed as-is to the callback.
 * The ic_name must be unique for the interceptor implementation and is used
 * to reject duplicate interceptor methods.
 *
 * Any number of interceptors can be added and they are called in the order
 * they were added, unless otherwise noted.
 * The list of registered interceptor methods are referred to as
 * interceptor chains.
 *
 * @remark Contrary to the Java client the librdkafka interceptor interface
 *         does not support message key and value modification.
 *         Message mutability is discouraged in the Java client and the
 *         combination of serializers and headers cover most use-cases.
 *
 * @remark Interceptors are NOT copied to the new configuration on
 *         rd_kafka_conf_dup() since it would be hard for interceptors to
 *         track usage of the interceptor's opaque value.
 *         An interceptor should rely on the plugin, which will be copied
 *         in rd_kafka_conf_conf_dup(), to set up the initial interceptors.
 *         An interceptor should implement the on_conf_dup() method
 *         to manually set up its internal configuration on the newly created
 *         configuration object that is being copied-to based on the
 *         interceptor-specific configuration properties.
 *         conf_dup() should thus be treated the same as conf_init().
 *
 * @remark Interceptors are keyed by the interceptor type (on_..()), the
 *         interceptor name (ic_name) and the interceptor method function.
 *         Duplicates are not allowed and the .._add_on_..() method will
 *         return RD_KAFKA_RESP_ERR__CONFLICT if attempting to add a duplicate
 *         method.
 *         The only exception is on_conf_destroy() which may be added multiple
 *         times by the same interceptor to allow proper cleanup of
 *         interceptor configuration state.
 */


/**
 * @brief on_conf_set() is called from rd_kafka_*_conf_set() in the order
 *        the interceptors were added.
 *
 * @param conf Configuration object.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 * @param name The configuration property to set.
 * @param val The configuration value to set, or NULL for reverting to default
 *            in which case the previous value should be freed.
 * @param errstr A human readable error string in case the interceptor fails.
 * @param errstr_size Maximum space (including \0) in \p errstr.
 *
 * @returns RD_KAFKA_CONF_OK if the property was known and successfully
 *          handled by the interceptor, RD_KAFKA_CONF_INVALID if the
 *          property was handled by the interceptor but the value was invalid,
 *          or RD_KAFKA_CONF_UNKNOWN if the interceptor did not handle
 *          this property, in which case the property is passed on on the
 *          interceptor in the chain, finally ending up at the built-in
 *          configuration handler.
 */
typedef rd_kafka_conf_res_t(rd_kafka_interceptor_f_on_conf_set_t)(
    rd_kafka_conf_t *conf,
    const char *name,
    const char *val,
    char *errstr,
    size_t errstr_size,
    void *ic_opaque);


/**
 * @brief on_conf_dup() is called from rd_kafka_conf_dup() in the
 *        order the interceptors were added and is used to let
 *        an interceptor re-register its conf interecptors with a new
 *        opaque value.
 *        The on_conf_dup() method is called prior to the configuration from
 *        \p old_conf being copied to \p new_conf.
 *
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 * @param new_conf New configuration object.
 * @param old_conf Old configuration object to copy properties from.
 * @param filter_cnt Number of property names to filter in \p filter.
 * @param filter Property names to filter out (ignore) when setting up
 *               \p new_conf.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an error code
 *          on failure (which is logged but otherwise ignored).
 *
 * @remark No on_conf_* interceptors are copied to the new configuration
 *         object on rd_kafka_conf_dup().
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_conf_dup_t)(
    rd_kafka_conf_t *new_conf,
    const rd_kafka_conf_t *old_conf,
    size_t filter_cnt,
    const char **filter,
    void *ic_opaque);


/**
 * @brief on_conf_destroy() is called from rd_kafka_*_conf_destroy() in the
 *        order the interceptors were added.
 *
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_conf_destroy_t)(
    void *ic_opaque);


/**
 * @brief on_new() is called from rd_kafka_new() prior toreturning
 *        the newly created client instance to the application.
 *
 * @param rk The client instance.
 * @param conf The client instance's final configuration.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 * @param errstr A human readable error string in case the interceptor fails.
 * @param errstr_size Maximum space (including \0) in \p errstr.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 *
 * @warning The \p rk client instance will not be fully set up when this
 *          interceptor is called and the interceptor MUST NOT call any
 *          other rk-specific APIs than rd_kafka_interceptor_add..().
 *
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_new_t)(
    rd_kafka_t *rk,
    const rd_kafka_conf_t *conf,
    void *ic_opaque,
    char *errstr,
    size_t errstr_size);


/**
 * @brief on_destroy() is called from rd_kafka_destroy() or (rd_kafka_new()
 *        if rd_kafka_new() fails during initialization).
 *
 * @param rk The client instance.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 */
typedef rd_kafka_resp_err_t(
    rd_kafka_interceptor_f_on_destroy_t)(rd_kafka_t *rk, void *ic_opaque);



/**
 * @brief on_send() is called from rd_kafka_produce*() (et.al) prior to
 *        the partitioner being called.
 *
 * @param rk The client instance.
 * @param rkmessage The message being produced. Immutable.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @remark This interceptor is only used by producer instances.
 *
 * @remark The \p rkmessage object is NOT mutable and MUST NOT be modified
 *         by the interceptor.
 *
 * @remark If the partitioner fails or an unknown partition was specified,
 *         the on_acknowledgement() interceptor chain will be called from
 *         within the rd_kafka_produce*() call to maintain send-acknowledgement
 *         symmetry.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_send_t)(
    rd_kafka_t *rk,
    rd_kafka_message_t *rkmessage,
    void *ic_opaque);

/**
 * @brief on_acknowledgement() is called to inform interceptors that a message
 *        was succesfully delivered or permanently failed delivery.
 *        The interceptor chain is called from internal librdkafka background
 *        threads, or rd_kafka_produce*() if the partitioner failed.
 *
 * @param rk The client instance.
 * @param rkmessage The message being produced. Immutable.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @remark This interceptor is only used by producer instances.
 *
 * @remark The \p rkmessage object is NOT mutable and MUST NOT be modified
 *         by the interceptor.
 *
 * @warning The on_acknowledgement() method may be called from internal
 *         librdkafka threads. An on_acknowledgement() interceptor MUST NOT
 *         call any librdkafka API's associated with the \p rk, or perform
 *         any blocking or prolonged work.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_acknowledgement_t)(
    rd_kafka_t *rk,
    rd_kafka_message_t *rkmessage,
    void *ic_opaque);


/**
 * @brief on_consume() is called just prior to passing the message to the
 *        application in rd_kafka_consumer_poll(), rd_kafka_consume*(),
 *        the event interface, etc.
 *
 * @param rk The client instance.
 * @param rkmessage The message being consumed. Immutable.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @remark This interceptor is only used by consumer instances.
 *
 * @remark The \p rkmessage object is NOT mutable and MUST NOT be modified
 *         by the interceptor.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_consume_t)(
    rd_kafka_t *rk,
    rd_kafka_message_t *rkmessage,
    void *ic_opaque);

/**
 * @brief on_commit() is called on completed or failed offset commit.
 *        It is called from internal librdkafka threads.
 *
 * @param rk The client instance.
 * @param offsets List of topic+partition+offset+error that were committed.
 *                The error message of each partition should be checked for
 *                error.
 * @param err The commit error, if any.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @remark This interceptor is only used by consumer instances.
 *
 * @warning The on_commit() interceptor is called from internal
 *          librdkafka threads. An on_commit() interceptor MUST NOT
 *          call any librdkafka API's associated with the \p rk, or perform
 *          any blocking or prolonged work.
 *
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_commit_t)(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *offsets,
    rd_kafka_resp_err_t err,
    void *ic_opaque);


/**
 * @brief on_request_sent() is called when a request has been fully written
 *        to a broker TCP connections socket.
 *
 * @param rk The client instance.
 * @param sockfd Socket file descriptor.
 * @param brokername Broker request is being sent to.
 * @param brokerid Broker request is being sent to.
 * @param ApiKey Kafka protocol request type.
 * @param ApiVersion Kafka protocol request type version.
 * @param CorrId Kafka protocol request correlation id.
 * @param size Size of request.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @warning The on_request_sent() interceptor is called from internal
 *          librdkafka broker threads. An on_request_sent() interceptor MUST NOT
 *          call any librdkafka API's associated with the \p rk, or perform
 *          any blocking or prolonged work.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_request_sent_t)(
    rd_kafka_t *rk,
    int sockfd,
    const char *brokername,
    int32_t brokerid,
    int16_t ApiKey,
    int16_t ApiVersion,
    int32_t CorrId,
    size_t size,
    void *ic_opaque);


/**
 * @brief on_response_received() is called when a protocol response has been
 *        fully received from a broker TCP connection socket but before the
 *        response payload is parsed.
 *
 * @param rk The client instance.
 * @param sockfd Socket file descriptor (always -1).
 * @param brokername Broker response was received from, possibly empty string
 *                   on error.
 * @param brokerid Broker response was received from.
 * @param ApiKey Kafka protocol request type or -1 on error.
 * @param ApiVersion Kafka protocol request type version or -1 on error.
 * @param CorrId Kafka protocol request correlation id, possibly -1 on error.
 * @param size Size of response, possibly 0 on error.
 * @param rtt Request round-trip-time in microseconds, possibly -1 on error.
 * @param err Receive error.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @warning The on_response_received() interceptor is called from internal
 *          librdkafka broker threads. An on_response_received() interceptor
 *          MUST NOT call any librdkafka API's associated with the \p rk, or
 *          perform any blocking or prolonged work.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_response_received_t)(
    rd_kafka_t *rk,
    int sockfd,
    const char *brokername,
    int32_t brokerid,
    int16_t ApiKey,
    int16_t ApiVersion,
    int32_t CorrId,
    size_t size,
    int64_t rtt,
    rd_kafka_resp_err_t err,
    void *ic_opaque);


/**
 * @brief on_thread_start() is called from a newly created librdkafka-managed
 *        thread.

 * @param rk The client instance.
 * @param thread_type Thread type.
 * @param thread_name Human-readable thread name, may not be unique.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @warning The on_thread_start() interceptor is called from internal
 *          librdkafka threads. An on_thread_start() interceptor MUST NOT
 *          call any librdkafka API's associated with the \p rk, or perform
 *          any blocking or prolonged work.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_thread_start_t)(
    rd_kafka_t *rk,
    rd_kafka_thread_type_t thread_type,
    const char *thread_name,
    void *ic_opaque);


/**
 * @brief on_thread_exit() is called just prior to a librdkafka-managed
 *        thread exiting from the exiting thread itself.
 *
 * @param rk The client instance.
 * @param thread_type Thread type.n
 * @param thread_name Human-readable thread name, may not be unique.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @remark Depending on the thread type, librdkafka may execute additional
 *         code on the thread after on_thread_exit() returns.
 *
 * @warning The on_thread_exit() interceptor is called from internal
 *          librdkafka threads. An on_thread_exit() interceptor MUST NOT
 *          call any librdkafka API's associated with the \p rk, or perform
 *          any blocking or prolonged work.
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_thread_exit_t)(
    rd_kafka_t *rk,
    rd_kafka_thread_type_t thread_type,
    const char *thread_name,
    void *ic_opaque);


/**
 * @brief on_broker_state_change() is called just after a broker
 *        has been created or its state has been changed.
 *
 * @param rk The client instance.
 * @param broker_id The broker id (-1 is used for bootstrap brokers).
 * @param secproto The security protocol.
 * @param name The original name of the broker.
 * @param port The port of the broker.
 * @param ic_opaque The interceptor's opaque pointer specified in ..add..().
 *
 * @returns an error code on failure, the error is logged but otherwise ignored.
 */
typedef rd_kafka_resp_err_t(rd_kafka_interceptor_f_on_broker_state_change_t)(
    rd_kafka_t *rk,
    int32_t broker_id,
    const char *secproto,
    const char *name,
    int port,
    const char *state,
    void *ic_opaque);


/**
 * @brief Append an on_conf_set() interceptor.
 *
 * @param conf Configuration object.
 * @param ic_name Interceptor name, used in logging.
 * @param on_conf_set Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_set(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_set_t *on_conf_set,
    void *ic_opaque);


/**
 * @brief Append an on_conf_dup() interceptor.
 *
 * @param conf Configuration object.
 * @param ic_name Interceptor name, used in logging.
 * @param on_conf_dup Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_dup(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_dup_t *on_conf_dup,
    void *ic_opaque);

/**
 * @brief Append an on_conf_destroy() interceptor.
 *
 * @param conf Configuration object.
 * @param ic_name Interceptor name, used in logging.
 * @param on_conf_destroy Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR
 *
 * @remark Multiple on_conf_destroy() interceptors are allowed to be added
 *         to the same configuration object.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_conf_interceptor_add_on_conf_destroy(
    rd_kafka_conf_t *conf,
    const char *ic_name,
    rd_kafka_interceptor_f_on_conf_destroy_t *on_conf_destroy,
    void *ic_opaque);


/**
 * @brief Append an on_new() interceptor.
 *
 * @param conf Configuration object.
 * @param ic_name Interceptor name, used in logging.
 * @param on_new Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @remark Since the on_new() interceptor is added to the configuration object
 *         it may be copied by rd_kafka_conf_dup().
 *         An interceptor implementation must thus be able to handle
 *         the same interceptor,ic_opaque tuple to be used by multiple
 *         client instances.
 *
 * @remark An interceptor plugin should check the return value to make sure it
 *         has not already been added.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_conf_interceptor_add_on_new(rd_kafka_conf_t *conf,
                                     const char *ic_name,
                                     rd_kafka_interceptor_f_on_new_t *on_new,
                                     void *ic_opaque);



/**
 * @brief Append an on_destroy() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_destroy Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_destroy(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_destroy_t *on_destroy,
    void *ic_opaque);


/**
 * @brief Append an on_send() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_send Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing intercepted with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_interceptor_add_on_send(rd_kafka_t *rk,
                                 const char *ic_name,
                                 rd_kafka_interceptor_f_on_send_t *on_send,
                                 void *ic_opaque);

/**
 * @brief Append an on_acknowledgement() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_acknowledgement Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_acknowledgement(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_acknowledgement_t *on_acknowledgement,
    void *ic_opaque);


/**
 * @brief Append an on_consume() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_consume Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_consume(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_consume_t *on_consume,
    void *ic_opaque);


/**
 * @brief Append an on_commit() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_commit() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_commit(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_commit_t *on_commit,
    void *ic_opaque);


/**
 * @brief Append an on_request_sent() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_request_sent() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_request_sent(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_request_sent_t *on_request_sent,
    void *ic_opaque);


/**
 * @brief Append an on_response_received() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_response_received() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_response_received(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_response_received_t *on_response_received,
    void *ic_opaque);


/**
 * @brief Append an on_thread_start() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_thread_start() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_thread_start(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_thread_start_t *on_thread_start,
    void *ic_opaque);


/**
 * @brief Append an on_thread_exit() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_thread_exit() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_interceptor_add_on_thread_exit(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_thread_exit_t *on_thread_exit,
    void *ic_opaque);


/**
 * @brief Append an on_broker_state_change() interceptor.
 *
 * @param rk Client instance.
 * @param ic_name Interceptor name, used in logging.
 * @param on_broker_state_change() Function pointer.
 * @param ic_opaque Opaque value that will be passed to the function.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or RD_KAFKA_RESP_ERR__CONFLICT
 *          if an existing interceptor with the same \p ic_name and function
 *          has already been added to \p conf.
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_interceptor_add_on_broker_state_change(
    rd_kafka_t *rk,
    const char *ic_name,
    rd_kafka_interceptor_f_on_broker_state_change_t *on_broker_state_change,
    void *ic_opaque);



/**@}*/



/**
 * @name Auxiliary types
 *
 * @{
 */



/**
 * @brief Topic result provides per-topic operation result information.
 *
 */

/**
 * @returns the error code for the given topic result.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_topic_result_error(const rd_kafka_topic_result_t *topicres);

/**
 * @returns the human readable error string for the given topic result,
 *          or NULL if there was no error.
 *
 * @remark lifetime of the returned string is the same as the \p topicres.
 */
RD_EXPORT const char *
rd_kafka_topic_result_error_string(const rd_kafka_topic_result_t *topicres);

/**
 * @returns the name of the topic for the given topic result.
 * @remark lifetime of the returned string is the same as the \p topicres.
 *
 */
RD_EXPORT const char *
rd_kafka_topic_result_name(const rd_kafka_topic_result_t *topicres);

/**
 * @brief Group result provides per-group operation result information.
 *
 */

/**
 * @returns the error for the given group result, or NULL on success.
 * @remark lifetime of the returned error is the same as the \p groupres.
 */
RD_EXPORT const rd_kafka_error_t *
rd_kafka_group_result_error(const rd_kafka_group_result_t *groupres);

/**
 * @returns the name of the group for the given group result.
 * @remark lifetime of the returned string is the same as the \p groupres.
 *
 */
RD_EXPORT const char *
rd_kafka_group_result_name(const rd_kafka_group_result_t *groupres);

/**
 * @returns the partitions/offsets for the given group result, if applicable
 *          to the request type, else NULL.
 * @remark lifetime of the returned list is the same as the \p groupres.
 */
RD_EXPORT const rd_kafka_topic_partition_list_t *
rd_kafka_group_result_partitions(const rd_kafka_group_result_t *groupres);


/**@}*/


/**
 * @name Admin API
 * @{
 *
 * @brief The Admin API enables applications to perform administrative
 *        Apache Kafka tasks, such as creating and deleting topics,
 *        altering and reading broker configuration, etc.
 *
 * The Admin API is asynchronous and makes use of librdkafka's standard
 * \c rd_kafka_queue_t queues to propagate the result of an admin operation
 * back to the application.
 * The supplied queue may be any queue, such as a temporary single-call queue,
 * a shared queue used for multiple requests, or even the main queue or
 * consumer queues.
 *
 * Use \c rd_kafka_queue_poll() to collect the result of an admin operation
 * from the queue of your choice, then extract the admin API-specific result
 * type by using the corresponding \c rd_kafka_event_CreateTopics_result,
 * \c rd_kafka_event_DescribeConfigs_result, etc, methods.
 * Use the getter methods on the \c .._result_t type to extract response
 * information and finally destroy the result and event by calling
 * \c rd_kafka_event_destroy().
 *
 * Use rd_kafka_event_error() and rd_kafka_event_error_string() to acquire
 * the request-level error/success for an Admin API request.
 * Even if the returned value is \c RD_KAFKA_RESP_ERR_NO_ERROR there
 * may be individual objects (topics, resources, etc) that have failed.
 * Extract per-object error information with the corresponding
 * \c rd_kafka_..._result_topics|resources|..() to check per-object errors.
 *
 * Locally triggered errors:
 *  - \c RD_KAFKA_RESP_ERR__TIMED_OUT - (Controller) broker connection did not
 *    become available in the time allowed by AdminOption_set_request_timeout.
 */


/**
 * @enum rd_kafka_admin_op_t
 *
 * @brief Admin operation enum name for use with rd_kafka_AdminOptions_new()
 *
 * @sa rd_kafka_AdminOptions_new()
 */
typedef enum rd_kafka_admin_op_t {
        RD_KAFKA_ADMIN_OP_ANY = 0,          /**< Default value */
        RD_KAFKA_ADMIN_OP_CREATETOPICS,     /**< CreateTopics */
        RD_KAFKA_ADMIN_OP_DELETETOPICS,     /**< DeleteTopics */
        RD_KAFKA_ADMIN_OP_CREATEPARTITIONS, /**< CreatePartitions */
        RD_KAFKA_ADMIN_OP_ALTERCONFIGS,     /**< AlterConfigs */
        RD_KAFKA_ADMIN_OP_DESCRIBECONFIGS,  /**< DescribeConfigs */
        RD_KAFKA_ADMIN_OP_DELETERECORDS,    /**< DeleteRecords */
        RD_KAFKA_ADMIN_OP_DELETEGROUPS,     /**< DeleteGroups */
        /** DeleteConsumerGroupOffsets */
        RD_KAFKA_ADMIN_OP_DELETECONSUMERGROUPOFFSETS,
        RD_KAFKA_ADMIN_OP_CREATEACLS,             /**< CreateAcls */
        RD_KAFKA_ADMIN_OP_DESCRIBEACLS,           /**< DescribeAcls */
        RD_KAFKA_ADMIN_OP_DELETEACLS,             /**< DeleteAcls */
        RD_KAFKA_ADMIN_OP_LISTCONSUMERGROUPS,     /**< ListConsumerGroups */
        RD_KAFKA_ADMIN_OP_DESCRIBECONSUMERGROUPS, /**< DescribeConsumerGroups */
        /** ListConsumerGroupOffsets */
        RD_KAFKA_ADMIN_OP_LISTCONSUMERGROUPOFFSETS,
        /** AlterConsumerGroupOffsets */
        RD_KAFKA_ADMIN_OP_ALTERCONSUMERGROUPOFFSETS,
        RD_KAFKA_ADMIN_OP__CNT /**< Number of ops defined */
} rd_kafka_admin_op_t;

/**
 * @brief AdminOptions provides a generic mechanism for setting optional
 *        parameters for the Admin API requests.
 *
 * @remark Since AdminOptions is decoupled from the actual request type
 *         there is no enforcement to prevent setting unrelated properties,
 *         e.g. setting validate_only on a DescribeConfigs request is allowed
 *         but is silently ignored by DescribeConfigs.
 *         Future versions may introduce such enforcement.
 */


typedef struct rd_kafka_AdminOptions_s rd_kafka_AdminOptions_t;

/**
 * @brief Create a new AdminOptions object.
 *
 *        The options object is not modified by the Admin API request APIs,
 *        (e.g. CreateTopics) and may be reused for multiple calls.
 *
 * @param rk Client instance.
 * @param for_api Specifies what Admin API this AdminOptions object will be used
 *                for, which will enforce what AdminOptions_set_..() calls may
 *                be used based on the API, causing unsupported set..() calls
 *                to fail.
 *                Specifying RD_KAFKA_ADMIN_OP_ANY disables the enforcement
 *                allowing any option to be set, even if the option
 *                is not used in a future call to an Admin API method.
 *
 * @returns a new AdminOptions object (which must be freed with
 *          rd_kafka_AdminOptions_destroy()), or NULL if \p for_api was set to
 *          an unknown API op type.
 */
RD_EXPORT rd_kafka_AdminOptions_t *
rd_kafka_AdminOptions_new(rd_kafka_t *rk, rd_kafka_admin_op_t for_api);


/**
 * @brief Destroy a AdminOptions object.
 */
RD_EXPORT void rd_kafka_AdminOptions_destroy(rd_kafka_AdminOptions_t *options);


/**
 * @brief Sets the overall request timeout, including broker lookup,
 *        request transmission, operation time on broker, and response.
 *
 * @param options Admin options.
 * @param timeout_ms Timeout in milliseconds, use -1 for indefinite timeout.
 *                   Defaults to `socket.timeout.ms`.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success, or
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if timeout was out of range in which
 *          case an error string will be written \p errstr.
 *
 * @remark This option is valid for all Admin API requests.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_request_timeout(rd_kafka_AdminOptions_t *options,
                                          int timeout_ms,
                                          char *errstr,
                                          size_t errstr_size);


/**
 * @brief Sets the broker's operation timeout, such as the timeout for
 *        CreateTopics to complete the creation of topics on the controller
 *        before returning a result to the application.
 *
 * CreateTopics: values <= 0 will return immediately after triggering topic
 * creation, while > 0 will wait this long for topic creation to propagate
 * in cluster. Default: 60 seconds.
 *
 * DeleteTopics: same semantics as CreateTopics.
 * CreatePartitions: same semantics as CreateTopics.
 *
 * @param options Admin options.
 * @param timeout_ms Timeout in milliseconds.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success, or
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if timeout was out of range in which
 *          case an error string will be written \p errstr.
 *
 * @remark This option is valid for CreateTopics, DeleteTopics,
 *         CreatePartitions, and DeleteRecords.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_operation_timeout(rd_kafka_AdminOptions_t *options,
                                            int timeout_ms,
                                            char *errstr,
                                            size_t errstr_size);


/**
 * @brief Tell broker to only validate the request, without performing
 *        the requested operation (create topics, etc).
 *
 * @param options Admin options.
 * @param true_or_false Defaults to false.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an
 *          error code on failure in which case an error string will
 *          be written \p errstr.
 *
 * @remark This option is valid for CreateTopics,
 *         CreatePartitions, AlterConfigs.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_validate_only(rd_kafka_AdminOptions_t *options,
                                        int true_or_false,
                                        char *errstr,
                                        size_t errstr_size);


/**
 * @brief Override what broker the Admin request will be sent to.
 *
 * By default, Admin requests are sent to the controller broker, with
 * the following exceptions:
 *   - AlterConfigs with a BROKER resource are sent to the broker id set
 *     as the resource name.
 *   - DescribeConfigs with a BROKER resource are sent to the broker id set
 *     as the resource name.
 *
 * @param options Admin Options.
 * @param broker_id The broker to send the request to.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success or an
 *          error code on failure in which case an error string will
 *          be written \p errstr.
 *
 * @remark This API should typically not be used, but serves as a workaround
 *         if new resource types are to the broker that the client
 *         does not know where to send.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_AdminOptions_set_broker(rd_kafka_AdminOptions_t *options,
                                 int32_t broker_id,
                                 char *errstr,
                                 size_t errstr_size);


/**
 * @brief Whether broker should return stable offsets
 *        (transaction-committed).
 *
 * @param options Admin options.
 * @param true_or_false Defaults to false.
 *
 * @return NULL on success, a new error instance that must be
 *         released with rd_kafka_error_destroy() in case of error.
 *
 * @remark This option is valid for ListConsumerGroupOffsets.
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_AdminOptions_set_require_stable_offsets(
    rd_kafka_AdminOptions_t *options,
    int true_or_false);

/**
 * @brief Set consumer groups states to query for.
 *
 * @param options Admin options.
 * @param consumer_group_states Array of consumer group states.
 * @param consumer_group_states_cnt Size of the \p consumer_group_states array.
 *
 * @return NULL on success, a new error instance that must be
 *         released with rd_kafka_error_destroy() in case of error.
 *
 * @remark This option is valid for ListConsumerGroups.
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_AdminOptions_set_match_consumer_group_states(
    rd_kafka_AdminOptions_t *options,
    const rd_kafka_consumer_group_state_t *consumer_group_states,
    size_t consumer_group_states_cnt);

/**
 * @brief Set application opaque value that can be extracted from the
 *        result event using rd_kafka_event_opaque()
 */
RD_EXPORT void
rd_kafka_AdminOptions_set_opaque(rd_kafka_AdminOptions_t *options,
                                 void *ev_opaque);

/**@}*/

/**
 * @name Admin API - Topics
 * @brief Topic related operations.
 * @{
 *
 */


/*! Defines a new topic to be created. */
typedef struct rd_kafka_NewTopic_s rd_kafka_NewTopic_t;

/**
 * @brief Create a new NewTopic object. This object is later passed to
 *        rd_kafka_CreateTopics().
 *
 * @param topic Topic name to create.
 * @param num_partitions Number of partitions in topic, or -1 to use the
 *                       broker's default partition count (>= 2.4.0).
 * @param replication_factor Default replication factor for the topic's
 *                           partitions, or -1 to use the broker's default
 *                           replication factor (>= 2.4.0) or if
 *                           set_replica_assignment() will be used.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 *
 * @returns a new allocated NewTopic object, or NULL if the input parameters
 *          are invalid.
 *          Use rd_kafka_NewTopic_destroy() to free object when done.
 */
RD_EXPORT rd_kafka_NewTopic_t *rd_kafka_NewTopic_new(const char *topic,
                                                     int num_partitions,
                                                     int replication_factor,
                                                     char *errstr,
                                                     size_t errstr_size);

/**
 * @brief Destroy and free a NewTopic object previously created with
 *        rd_kafka_NewTopic_new()
 */
RD_EXPORT void rd_kafka_NewTopic_destroy(rd_kafka_NewTopic_t *new_topic);


/**
 * @brief Helper function to destroy all NewTopic objects in the \p new_topics
 *        array (of \p new_topic_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void rd_kafka_NewTopic_destroy_array(rd_kafka_NewTopic_t **new_topics,
                                               size_t new_topic_cnt);


/**
 * @brief Set the replica (broker) assignment for \p partition to the
 *        replica set in \p broker_ids (of \p broker_id_cnt elements).
 *
 * @remark When this method is used, rd_kafka_NewTopic_new() must have
 *         been called with a \c replication_factor of -1.
 *
 * @remark An application must either set the replica assignment for
 *         all new partitions, or none.
 *
 * @remark If called, this function must be called consecutively for each
 *         partition, starting at 0.
 *
 * @remark Use rd_kafka_metadata() to retrieve the list of brokers
 *         in the cluster.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success, or an error code
 *          if the arguments were invalid.
 *
 * @sa rd_kafka_AdminOptions_set_validate_only()
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_NewTopic_set_replica_assignment(rd_kafka_NewTopic_t *new_topic,
                                         int32_t partition,
                                         int32_t *broker_ids,
                                         size_t broker_id_cnt,
                                         char *errstr,
                                         size_t errstr_size);

/**
 * @brief Set (broker-side) topic configuration name/value pair.
 *
 * @remark The name and value are not validated by the client, the validation
 *         takes place on the broker.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success, or an error code
 *          if the arguments were invalid.
 *
 * @sa rd_kafka_AdminOptions_set_validate_only()
 * @sa http://kafka.apache.org/documentation.html#topicconfigs
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_NewTopic_set_config(rd_kafka_NewTopic_t *new_topic,
                             const char *name,
                             const char *value);


/**
 * @brief Create topics in cluster as specified by the \p new_topics
 *        array of size \p new_topic_cnt elements.
 *
 * @param rk Client instance.
 * @param new_topics Array of new topics to create.
 * @param new_topic_cnt Number of elements in \p new_topics array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_validate_only() - default false
 *  - rd_kafka_AdminOptions_set_operation_timeout() - default 60 seconds
 *  - rd_kafka_AdminOptions_set_request_timeout() - default socket.timeout.ms
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_CREATETOPICS_RESULT
 */
RD_EXPORT void rd_kafka_CreateTopics(rd_kafka_t *rk,
                                     rd_kafka_NewTopic_t **new_topics,
                                     size_t new_topic_cnt,
                                     const rd_kafka_AdminOptions_t *options,
                                     rd_kafka_queue_t *rkqu);


/*
 * CreateTopics result type and methods
 */

/**
 * @brief Get an array of topic results from a CreateTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 *
 * @param result Result to get topics from.
 * @param cntp Updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_topic_result_t **rd_kafka_CreateTopics_result_topics(
    const rd_kafka_CreateTopics_result_t *result,
    size_t *cntp);



/*
 * DeleteTopics - delete topics from cluster
 *
 */

/*! Represents a topic to be deleted. */
typedef struct rd_kafka_DeleteTopic_s rd_kafka_DeleteTopic_t;

/**
 * @brief Create a new DeleteTopic object. This object is later passed to
 *        rd_kafka_DeleteTopics().
 *
 * @param topic Topic name to delete.
 *
 * @returns a new allocated DeleteTopic object.
 *          Use rd_kafka_DeleteTopic_destroy() to free object when done.
 */
RD_EXPORT rd_kafka_DeleteTopic_t *rd_kafka_DeleteTopic_new(const char *topic);

/**
 * @brief Destroy and free a DeleteTopic object previously created with
 *        rd_kafka_DeleteTopic_new()
 */
RD_EXPORT void rd_kafka_DeleteTopic_destroy(rd_kafka_DeleteTopic_t *del_topic);

/**
 * @brief Helper function to destroy all DeleteTopic objects in
 *        the \p del_topics array (of \p del_topic_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_DeleteTopic_destroy_array(rd_kafka_DeleteTopic_t **del_topics,
                                   size_t del_topic_cnt);

/**
 * @brief Delete topics from cluster as specified by the \p topics
 *        array of size \p topic_cnt elements.
 *
 * @param rk Client instance.
 * @param del_topics Array of topics to delete.
 * @param del_topic_cnt Number of elements in \p topics array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DELETETOPICS_RESULT
 */
RD_EXPORT
void rd_kafka_DeleteTopics(rd_kafka_t *rk,
                           rd_kafka_DeleteTopic_t **del_topics,
                           size_t del_topic_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu);



/*
 * DeleteTopics result type and methods
 */

/**
 * @brief Get an array of topic results from a DeleteTopics result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 *
 * @param result Result to get topic results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_topic_result_t **rd_kafka_DeleteTopics_result_topics(
    const rd_kafka_DeleteTopics_result_t *result,
    size_t *cntp);


/**@}*/

/**
 * @name Admin API - Partitions
 * @brief Partition related operations.
 * @{
 *
 */

/*! Defines a new partition to be created. */
typedef struct rd_kafka_NewPartitions_s rd_kafka_NewPartitions_t;

/**
 * @brief Create a new NewPartitions. This object is later passed to
 *        rd_kafka_CreatePartitions() to increase the number of partitions
 *        to \p new_total_cnt for an existing topic.
 *
 * @param topic Topic name to create more partitions for.
 * @param new_total_cnt Increase the topic's partition count to this value.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * @returns a new allocated NewPartitions object, or NULL if the
 *          input parameters are invalid.
 *          Use rd_kafka_NewPartitions_destroy() to free object when done.
 */
RD_EXPORT rd_kafka_NewPartitions_t *
rd_kafka_NewPartitions_new(const char *topic,
                           size_t new_total_cnt,
                           char *errstr,
                           size_t errstr_size);

/**
 * @brief Destroy and free a NewPartitions object previously created with
 *        rd_kafka_NewPartitions_new()
 */
RD_EXPORT void
rd_kafka_NewPartitions_destroy(rd_kafka_NewPartitions_t *new_parts);

/**
 * @brief Helper function to destroy all NewPartitions objects in the
 *        \p new_parts array (of \p new_parts_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_NewPartitions_destroy_array(rd_kafka_NewPartitions_t **new_parts,
                                     size_t new_parts_cnt);

/**
 * @brief Set the replica (broker id) assignment for \p new_partition_idx to the
 *        replica set in \p broker_ids (of \p broker_id_cnt elements).
 *
 * @remark An application must either set the replica assignment for
 *         all new partitions, or none.
 *
 * @remark If called, this function must be called consecutively for each
 *         new partition being created,
 *         where \p new_partition_idx 0 is the first new partition,
 *         1 is the second, and so on.
 *
 * @remark \p broker_id_cnt should match the topic's replication factor.
 *
 * @remark Use rd_kafka_metadata() to retrieve the list of brokers
 *         in the cluster.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR on success, or an error code
 *          if the arguments were invalid.
 *
 * @sa rd_kafka_AdminOptions_set_validate_only()
 */
RD_EXPORT rd_kafka_resp_err_t rd_kafka_NewPartitions_set_replica_assignment(
    rd_kafka_NewPartitions_t *new_parts,
    int32_t new_partition_idx,
    int32_t *broker_ids,
    size_t broker_id_cnt,
    char *errstr,
    size_t errstr_size);


/**
 * @brief Create additional partitions for the given topics, as specified
 *        by the \p new_parts array of size \p new_parts_cnt elements.
 *
 * @param rk Client instance.
 * @param new_parts Array of topics for which new partitions are to be created.
 * @param new_parts_cnt Number of elements in \p new_parts array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_validate_only() - default false
 *  - rd_kafka_AdminOptions_set_operation_timeout() - default 60 seconds
 *  - rd_kafka_AdminOptions_set_request_timeout() - default socket.timeout.ms
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_CREATEPARTITIONS_RESULT
 */
RD_EXPORT void rd_kafka_CreatePartitions(rd_kafka_t *rk,
                                         rd_kafka_NewPartitions_t **new_parts,
                                         size_t new_parts_cnt,
                                         const rd_kafka_AdminOptions_t *options,
                                         rd_kafka_queue_t *rkqu);



/*
 * CreatePartitions result type and methods
 */

/**
 * @brief Get an array of topic results from a CreatePartitions result.
 *
 * The returned \p topics life-time is the same as the \p result object.
 *
 * @param result Result o get topic results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_topic_result_t **
rd_kafka_CreatePartitions_result_topics(
    const rd_kafka_CreatePartitions_result_t *result,
    size_t *cntp);

/**@}*/

/**
 * @name Admin API - Configuration
 * @brief Cluster, broker, topic configuration entries, sources, etc.
 * @{
 *
 */

/**
 * @enum rd_kafka_ConfigSource_t
 *
 * @brief Apache Kafka config sources.
 *
 * @remark These entities relate to the cluster, not the local client.
 *
 * @sa rd_kafka_conf_set(), et.al. for local client configuration.
 */
typedef enum rd_kafka_ConfigSource_t {
        /** Source unknown, e.g., in the ConfigEntry used for alter requests
         *  where source is not set */
        RD_KAFKA_CONFIG_SOURCE_UNKNOWN_CONFIG = 0,
        /** Dynamic topic config that is configured for a specific topic */
        RD_KAFKA_CONFIG_SOURCE_DYNAMIC_TOPIC_CONFIG = 1,
        /** Dynamic broker config that is configured for a specific broker */
        RD_KAFKA_CONFIG_SOURCE_DYNAMIC_BROKER_CONFIG = 2,
        /** Dynamic broker config that is configured as default for all
         *  brokers in the cluster */
        RD_KAFKA_CONFIG_SOURCE_DYNAMIC_DEFAULT_BROKER_CONFIG = 3,
        /** Static broker config provided as broker properties at startup
         *  (e.g. from server.properties file) */
        RD_KAFKA_CONFIG_SOURCE_STATIC_BROKER_CONFIG = 4,
        /** Built-in default configuration for configs that have a
         *  default value */
        RD_KAFKA_CONFIG_SOURCE_DEFAULT_CONFIG = 5,

        /** Number of source types defined */
        RD_KAFKA_CONFIG_SOURCE__CNT,
} rd_kafka_ConfigSource_t;


/**
 * @returns a string representation of the \p confsource.
 */
RD_EXPORT const char *
rd_kafka_ConfigSource_name(rd_kafka_ConfigSource_t confsource);


/*! Apache Kafka configuration entry. */
typedef struct rd_kafka_ConfigEntry_s rd_kafka_ConfigEntry_t;

/**
 * @returns the configuration property name
 */
RD_EXPORT const char *
rd_kafka_ConfigEntry_name(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns the configuration value, may be NULL for sensitive or unset
 *          properties.
 */
RD_EXPORT const char *
rd_kafka_ConfigEntry_value(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns the config source.
 */
RD_EXPORT rd_kafka_ConfigSource_t
rd_kafka_ConfigEntry_source(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns 1 if the config property is read-only on the broker, else 0.
 * @remark Shall only be used on a DescribeConfigs result, otherwise returns -1.
 */
RD_EXPORT int
rd_kafka_ConfigEntry_is_read_only(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns 1 if the config property is set to its default value on the broker,
 *          else 0.
 * @remark Shall only be used on a DescribeConfigs result, otherwise returns -1.
 */
RD_EXPORT int
rd_kafka_ConfigEntry_is_default(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns 1 if the config property contains sensitive information (such as
 *          security configuration), else 0.
 * @remark An application should take care not to include the value of
 *         sensitive configuration entries in its output.
 * @remark Shall only be used on a DescribeConfigs result, otherwise returns -1.
 */
RD_EXPORT int
rd_kafka_ConfigEntry_is_sensitive(const rd_kafka_ConfigEntry_t *entry);

/**
 * @returns 1 if this entry is a synonym, else 0.
 */
RD_EXPORT int
rd_kafka_ConfigEntry_is_synonym(const rd_kafka_ConfigEntry_t *entry);


/**
 * @returns the synonym config entry array.
 *
 * @param entry Entry to get synonyms for.
 * @param cntp is updated to the number of elements in the array.
 *
 * @remark The lifetime of the returned entry is the same as \p conf .
 * @remark Shall only be used on a DescribeConfigs result,
 *         otherwise returns NULL.
 */
RD_EXPORT const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigEntry_synonyms(const rd_kafka_ConfigEntry_t *entry,
                              size_t *cntp);



/**
 * @enum rd_kafka_ResourceType_t
 * @brief Apache Kafka resource types
 */
typedef enum rd_kafka_ResourceType_t {
        RD_KAFKA_RESOURCE_UNKNOWN = 0, /**< Unknown */
        RD_KAFKA_RESOURCE_ANY     = 1, /**< Any (used for lookups) */
        RD_KAFKA_RESOURCE_TOPIC   = 2, /**< Topic */
        RD_KAFKA_RESOURCE_GROUP   = 3, /**< Group */
        RD_KAFKA_RESOURCE_BROKER  = 4, /**< Broker */
        RD_KAFKA_RESOURCE__CNT,        /**< Number of resource types defined */
} rd_kafka_ResourceType_t;

/**
 * @enum rd_kafka_ResourcePatternType_t
 * @brief Apache Kafka pattern types
 */
typedef enum rd_kafka_ResourcePatternType_t {
        /** Unknown */
        RD_KAFKA_RESOURCE_PATTERN_UNKNOWN = 0,
        /** Any (used for lookups) */
        RD_KAFKA_RESOURCE_PATTERN_ANY = 1,
        /** Match: will perform pattern matching */
        RD_KAFKA_RESOURCE_PATTERN_MATCH = 2,
        /** Literal: A literal resource name */
        RD_KAFKA_RESOURCE_PATTERN_LITERAL = 3,
        /** Prefixed: A prefixed resource name */
        RD_KAFKA_RESOURCE_PATTERN_PREFIXED = 4,
        RD_KAFKA_RESOURCE_PATTERN_TYPE__CNT,
} rd_kafka_ResourcePatternType_t;

/**
 * @returns a string representation of the \p resource_pattern_type
 */
RD_EXPORT const char *rd_kafka_ResourcePatternType_name(
    rd_kafka_ResourcePatternType_t resource_pattern_type);

/**
 * @returns a string representation of the \p restype
 */
RD_EXPORT const char *
rd_kafka_ResourceType_name(rd_kafka_ResourceType_t restype);

/*! Apache Kafka configuration resource. */
typedef struct rd_kafka_ConfigResource_s rd_kafka_ConfigResource_t;


/**
 * @brief Create new ConfigResource object.
 *
 * @param restype The resource type (e.g., RD_KAFKA_RESOURCE_TOPIC)
 * @param resname The resource name (e.g., the topic name)
 *
 * @returns a newly allocated object
 */
RD_EXPORT rd_kafka_ConfigResource_t *
rd_kafka_ConfigResource_new(rd_kafka_ResourceType_t restype,
                            const char *resname);

/**
 * @brief Destroy and free a ConfigResource object previously created with
 *        rd_kafka_ConfigResource_new()
 */
RD_EXPORT void
rd_kafka_ConfigResource_destroy(rd_kafka_ConfigResource_t *config);


/**
 * @brief Helper function to destroy all ConfigResource objects in
 *        the \p configs array (of \p config_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_ConfigResource_destroy_array(rd_kafka_ConfigResource_t **config,
                                      size_t config_cnt);


/**
 * @brief Set configuration name value pair.
 *
 * @param config ConfigResource to set config property on.
 * @param name Configuration name, depends on resource type.
 * @param value Configuration value, depends on resource type and \p name.
 *              Set to \c NULL to revert configuration value to default.
 *
 * This will overwrite the current value.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if config was added to resource,
 *          or RD_KAFKA_RESP_ERR__INVALID_ARG on invalid input.
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_ConfigResource_set_config(rd_kafka_ConfigResource_t *config,
                                   const char *name,
                                   const char *value);


/**
 * @brief Get an array of config entries from a ConfigResource object.
 *
 * The returned object life-times are the same as the \p config object.
 *
 * @param config ConfigResource to get configs from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_ConfigEntry_t **
rd_kafka_ConfigResource_configs(const rd_kafka_ConfigResource_t *config,
                                size_t *cntp);



/**
 * @returns the ResourceType for \p config
 */
RD_EXPORT rd_kafka_ResourceType_t
rd_kafka_ConfigResource_type(const rd_kafka_ConfigResource_t *config);

/**
 * @returns the name for \p config
 */
RD_EXPORT const char *
rd_kafka_ConfigResource_name(const rd_kafka_ConfigResource_t *config);

/**
 * @returns the error for this resource from an AlterConfigs request
 */
RD_EXPORT rd_kafka_resp_err_t
rd_kafka_ConfigResource_error(const rd_kafka_ConfigResource_t *config);

/**
 * @returns the error string for this resource from an AlterConfigs
 *          request, or NULL if no error.
 */
RD_EXPORT const char *
rd_kafka_ConfigResource_error_string(const rd_kafka_ConfigResource_t *config);


/*
 * AlterConfigs - alter cluster configuration.
 *
 */


/**
 * @brief Update the configuration for the specified resources.
 *        Updates are not transactional so they may succeed for a subset
 *        of the provided resources while the others fail.
 *        The configuration for a particular resource is updated atomically,
 *        replacing values using the provided ConfigEntrys and reverting
 *        unspecified ConfigEntrys to their default values.
 *
 * @remark Requires broker version >=0.11.0.0
 *
 * @warning AlterConfigs will replace all existing configuration for
 *          the provided resources with the new configuration given,
 *          reverting all other configuration to their default values.
 *
 * @remark Multiple resources and resource types may be set, but at most one
 *         resource of type \c RD_KAFKA_RESOURCE_BROKER is allowed per call
 *         since these resource requests must be sent to the broker specified
 *         in the resource.
 *
 */
RD_EXPORT
void rd_kafka_AlterConfigs(rd_kafka_t *rk,
                           rd_kafka_ConfigResource_t **configs,
                           size_t config_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu);


/*
 * AlterConfigs result type and methods
 */

/**
 * @brief Get an array of resource results from a AlterConfigs result.
 *
 * Use \c rd_kafka_ConfigResource_error() and
 * \c rd_kafka_ConfigResource_error_string() to extract per-resource error
 * results on the returned array elements.
 *
 * The returned object life-times are the same as the \p result object.
 *
 * @param result Result object to get resource results from.
 * @param cntp is updated to the number of elements in the array.
 *
 * @returns an array of ConfigResource elements, or NULL if not available.
 */
RD_EXPORT const rd_kafka_ConfigResource_t **
rd_kafka_AlterConfigs_result_resources(
    const rd_kafka_AlterConfigs_result_t *result,
    size_t *cntp);



/*
 * DescribeConfigs - retrieve cluster configuration.
 *
 */


/**
 * @brief Get configuration for the specified resources in \p configs.
 *
 * The returned configuration includes default values and the
 * rd_kafka_ConfigEntry_is_default() or rd_kafka_ConfigEntry_source()
 * methods may be used to distinguish them from user supplied values.
 *
 * The value of config entries where rd_kafka_ConfigEntry_is_sensitive()
 * is true will always be NULL to avoid disclosing sensitive
 * information, such as security settings.
 *
 * Configuration entries where rd_kafka_ConfigEntry_is_read_only()
 * is true can't be updated (with rd_kafka_AlterConfigs()).
 *
 * Synonym configuration entries are returned if the broker supports
 * it (broker version >= 1.1.0). See rd_kafka_ConfigEntry_synonyms().
 *
 * @remark Requires broker version >=0.11.0.0
 *
 * @remark Multiple resources and resource types may be requested, but at most
 *         one resource of type \c RD_KAFKA_RESOURCE_BROKER is allowed per call
 *         since these resource requests must be sent to the broker specified
 *         in the resource.
 */
RD_EXPORT
void rd_kafka_DescribeConfigs(rd_kafka_t *rk,
                              rd_kafka_ConfigResource_t **configs,
                              size_t config_cnt,
                              const rd_kafka_AdminOptions_t *options,
                              rd_kafka_queue_t *rkqu);



/*
 * DescribeConfigs result type and methods
 */

/**
 * @brief Get an array of resource results from a DescribeConfigs result.
 *
 * The returned \p resources life-time is the same as the \p result object.
 *
 * @param result Result object to get resource results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_ConfigResource_t **
rd_kafka_DescribeConfigs_result_resources(
    const rd_kafka_DescribeConfigs_result_t *result,
    size_t *cntp);


/**@}*/

/**
 * @name Admin API - DeleteRecords
 * @brief delete records (messages) from partitions.
 * @{
 *
 */

/**! Represents records to be deleted */
typedef struct rd_kafka_DeleteRecords_s rd_kafka_DeleteRecords_t;

/**
 * @brief Create a new DeleteRecords object. This object is later passed to
 *        rd_kafka_DeleteRecords().
 *
 * \p before_offsets must contain \c topic, \c partition, and
 * \c offset is the offset before which the messages will
 * be deleted (exclusive).
 * Set \c offset to RD_KAFKA_OFFSET_END (high-watermark) in order to
 * delete all data in the partition.
 *
 * @param before_offsets For each partition delete all messages up to but not
 *                       including the specified offset.
 *
 * @returns a new allocated DeleteRecords object.
 *          Use rd_kafka_DeleteRecords_destroy() to free object when done.
 */
RD_EXPORT rd_kafka_DeleteRecords_t *rd_kafka_DeleteRecords_new(
    const rd_kafka_topic_partition_list_t *before_offsets);

/**
 * @brief Destroy and free a DeleteRecords object previously created with
 *        rd_kafka_DeleteRecords_new()
 */
RD_EXPORT void
rd_kafka_DeleteRecords_destroy(rd_kafka_DeleteRecords_t *del_records);

/**
 * @brief Helper function to destroy all DeleteRecords objects in
 *        the \p del_groups array (of \p del_group_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_DeleteRecords_destroy_array(rd_kafka_DeleteRecords_t **del_records,
                                     size_t del_record_cnt);

/**
 * @brief Delete records (messages) in topic partitions older than the
 *        offsets provided.
 *
 * @param rk Client instance.
 * @param del_records The offsets to delete (up to).
 *                    Currently only one DeleteRecords_t (but containing
 *                    multiple offsets) is supported.
 * @param del_record_cnt The number of elements in del_records, must be 1.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_operation_timeout() - default 60 seconds.
 *    Controls how long the brokers will wait for records to be deleted.
 *  - rd_kafka_AdminOptions_set_request_timeout() - default socket.timeout.ms.
 *    Controls how long \c rdkafka will wait for the request to complete.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DELETERECORDS_RESULT
 */
RD_EXPORT void rd_kafka_DeleteRecords(rd_kafka_t *rk,
                                      rd_kafka_DeleteRecords_t **del_records,
                                      size_t del_record_cnt,
                                      const rd_kafka_AdminOptions_t *options,
                                      rd_kafka_queue_t *rkqu);


/*
 * DeleteRecords result type and methods
 */

/**
 * @brief Get a list of topic and partition results from a DeleteRecords result.
 *        The returned objects will contain \c topic, \c partition, \c offset
 *        and \c err. \c offset will be set to the post-deletion low-watermark
 *        (smallest available offset of all live replicas). \c err will be set
 *        per-partition if deletion failed.
 *
 * The returned object's life-time is the same as the \p result object.
 */
RD_EXPORT const rd_kafka_topic_partition_list_t *
rd_kafka_DeleteRecords_result_offsets(
    const rd_kafka_DeleteRecords_result_t *result);

/**@}*/

/**
 * @name Admin API - ListConsumerGroups
 * @{
 */


/**
 * @brief ListConsumerGroups result for a single group
 */

/**! ListConsumerGroups result for a single group */
typedef struct rd_kafka_ConsumerGroupListing_s rd_kafka_ConsumerGroupListing_t;

/**! ListConsumerGroups results and errors */
typedef struct rd_kafka_ListConsumerGroupsResult_s
    rd_kafka_ListConsumerGroupsResult_t;

/**
 * @brief List the consumer groups available in the cluster.
 *
 * @param rk Client instance.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_LISTCONSUMERGROUPS_RESULT
 */
RD_EXPORT
void rd_kafka_ListConsumerGroups(rd_kafka_t *rk,
                                 const rd_kafka_AdminOptions_t *options,
                                 rd_kafka_queue_t *rkqu);

/**
 * @brief Gets the group id for the \p grplist group.
 *
 * @param grplist The group listing.
 *
 * @return The group id.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grplist object.
 */
RD_EXPORT
const char *rd_kafka_ConsumerGroupListing_group_id(
    const rd_kafka_ConsumerGroupListing_t *grplist);

/**
 * @brief Is the \p grplist group a simple consumer group.
 *
 * @param grplist The group listing.
 *
 * @return 1 if the group is a simple consumer group,
 *         else 0.
 */
RD_EXPORT
int rd_kafka_ConsumerGroupListing_is_simple_consumer_group(
    const rd_kafka_ConsumerGroupListing_t *grplist);

/**
 * @brief Gets state for the \p grplist group.
 *
 * @param grplist The group listing.
 *
 * @return A group state.
 */
RD_EXPORT
rd_kafka_consumer_group_state_t rd_kafka_ConsumerGroupListing_state(
    const rd_kafka_ConsumerGroupListing_t *grplist);

/**
 * @brief Get an array of valid list groups from a ListConsumerGroups result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p result object.
 */
RD_EXPORT
const rd_kafka_ConsumerGroupListing_t **
rd_kafka_ListConsumerGroups_result_valid(
    const rd_kafka_ListConsumerGroups_result_t *result,
    size_t *cntp);

/**
 * @brief Get an array of errors from a ListConsumerGroups call result.
 *
 * The returned errors life-time is the same as the \p result object.
 *
 * @param result ListConsumerGroups result.
 * @param cntp Is updated to the number of elements in the array.
 *
 * @return Array of errors in \p result.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p result object.
 */
RD_EXPORT
const rd_kafka_error_t **rd_kafka_ListConsumerGroups_result_errors(
    const rd_kafka_ListConsumerGroups_result_t *result,
    size_t *cntp);

/**@}*/

/**
 * @name Admin API - DescribeConsumerGroups
 * @{
 */

/**
 * @brief DescribeConsumerGroups result type.
 *
 */
typedef struct rd_kafka_ConsumerGroupDescription_s
    rd_kafka_ConsumerGroupDescription_t;

/**
 * @brief Member description included in ConsumerGroupDescription.
 *
 */
typedef struct rd_kafka_MemberDescription_s rd_kafka_MemberDescription_t;

/**
 * @brief Member assignment included in MemberDescription.
 *
 */
typedef struct rd_kafka_MemberAssignment_s rd_kafka_MemberAssignment_t;

/**
 * @brief Describe groups from cluster as specified by the \p groups
 *        array of size \p groups_cnt elements.
 *
 * @param rk Client instance.
 * @param groups Array of groups to describe.
 * @param groups_cnt Number of elements in \p groups array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DESCRIBECONSUMERGROUPS_RESULT
 */
RD_EXPORT
void rd_kafka_DescribeConsumerGroups(rd_kafka_t *rk,
                                     const char **groups,
                                     size_t groups_cnt,
                                     const rd_kafka_AdminOptions_t *options,
                                     rd_kafka_queue_t *rkqu);

/**
 * @brief Get an array of group results from a DescribeConsumerGroups result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p result object.
 */
RD_EXPORT
const rd_kafka_ConsumerGroupDescription_t **
rd_kafka_DescribeConsumerGroups_result_groups(
    const rd_kafka_DescribeConsumerGroups_result_t *result,
    size_t *cntp);


/**
 * @brief Gets the group id for the \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return The group id.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grpdesc object.
 */
RD_EXPORT
const char *rd_kafka_ConsumerGroupDescription_group_id(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);

/**
 * @brief Gets the error for the \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return The group description error.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grpdesc object.
 */
RD_EXPORT
const rd_kafka_error_t *rd_kafka_ConsumerGroupDescription_error(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);

/**
 * @brief Is the \p grpdesc group a simple consumer group.
 *
 * @param grpdesc The group description.
 * @return 1 if the group is a simple consumer group,
 *         else 0.
 */
RD_EXPORT
int rd_kafka_ConsumerGroupDescription_is_simple_consumer_group(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);


/**
 * @brief Gets the partition assignor for the \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return The partition assignor.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grpdesc object.
 */
RD_EXPORT
const char *rd_kafka_ConsumerGroupDescription_partition_assignor(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);


/**
 * @brief Gets state for the \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return A group state.
 */
RD_EXPORT
rd_kafka_consumer_group_state_t rd_kafka_ConsumerGroupDescription_state(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);

/**
 * @brief Gets the coordinator for the \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return The group coordinator.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grpdesc object.
 */
RD_EXPORT
const rd_kafka_Node_t *rd_kafka_ConsumerGroupDescription_coordinator(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);

/**
 * @brief Gets the members count of \p grpdesc group.
 *
 * @param grpdesc The group description.
 *
 * @return The member count.
 */
RD_EXPORT
size_t rd_kafka_ConsumerGroupDescription_member_count(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc);

/**
 * @brief Gets a member of \p grpdesc group.
 *
 * @param grpdesc The group description.
 * @param idx The member idx.
 *
 * @return A member at index \p idx, or NULL if
 *         \p idx is out of range.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p grpdesc object.
 */
RD_EXPORT
const rd_kafka_MemberDescription_t *rd_kafka_ConsumerGroupDescription_member(
    const rd_kafka_ConsumerGroupDescription_t *grpdesc,
    size_t idx);

/**
 * @brief Gets client id of \p member.
 *
 * @param member The group member.
 *
 * @return The client id.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p member object.
 */
RD_EXPORT
const char *rd_kafka_MemberDescription_client_id(
    const rd_kafka_MemberDescription_t *member);

/**
 * @brief Gets group instance id of \p member.
 *
 * @param member The group member.
 *
 * @return The group instance id, or NULL if not available.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p member object.
 */
RD_EXPORT
const char *rd_kafka_MemberDescription_group_instance_id(
    const rd_kafka_MemberDescription_t *member);

/**
 * @brief Gets consumer id of \p member.
 *
 * @param member The group member.
 *
 * @return The consumer id.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p member object.
 */
RD_EXPORT
const char *rd_kafka_MemberDescription_consumer_id(
    const rd_kafka_MemberDescription_t *member);

/**
 * @brief Gets host of \p member.
 *
 * @param member The group member.
 *
 * @return The host.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p member object.
 */
RD_EXPORT
const char *
rd_kafka_MemberDescription_host(const rd_kafka_MemberDescription_t *member);

/**
 * @brief Gets assignment of \p member.
 *
 * @param member The group member.
 *
 * @return The member assignment.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p member object.
 */
RD_EXPORT
const rd_kafka_MemberAssignment_t *rd_kafka_MemberDescription_assignment(
    const rd_kafka_MemberDescription_t *member);

/**
 * @brief Gets assigned partitions of a member \p assignment.
 *
 * @param assignment The group member assignment.
 *
 * @return The assigned partitions.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p assignment object.
 */
RD_EXPORT
const rd_kafka_topic_partition_list_t *rd_kafka_MemberAssignment_partitions(
    const rd_kafka_MemberAssignment_t *assignment);

/**@}*/

/**
 * @name Admin API - DeleteGroups
 * @brief Delete groups from cluster
 * @{
 *
 *
 */

/*! Represents a group to be deleted. */
typedef struct rd_kafka_DeleteGroup_s rd_kafka_DeleteGroup_t;

/**
 * @brief Create a new DeleteGroup object. This object is later passed to
 *        rd_kafka_DeleteGroups().
 *
 * @param group Name of group to delete.
 *
 * @returns a new allocated DeleteGroup object.
 *          Use rd_kafka_DeleteGroup_destroy() to free object when done.
 */
RD_EXPORT
rd_kafka_DeleteGroup_t *rd_kafka_DeleteGroup_new(const char *group);

/**
 * @brief Destroy and free a DeleteGroup object previously created with
 *        rd_kafka_DeleteGroup_new()
 */
RD_EXPORT
void rd_kafka_DeleteGroup_destroy(rd_kafka_DeleteGroup_t *del_group);

/**
 * @brief Helper function to destroy all DeleteGroup objects in
 *        the \p del_groups array (of \p del_group_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_DeleteGroup_destroy_array(rd_kafka_DeleteGroup_t **del_groups,
                                   size_t del_group_cnt);

/**
 * @brief Delete groups from cluster as specified by the \p del_groups
 *        array of size \p del_group_cnt elements.
 *
 * @param rk Client instance.
 * @param del_groups Array of groups to delete.
 * @param del_group_cnt Number of elements in \p del_groups array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DELETEGROUPS_RESULT
 *
 * @remark This function in called deleteConsumerGroups in the Java client.
 */
RD_EXPORT
void rd_kafka_DeleteGroups(rd_kafka_t *rk,
                           rd_kafka_DeleteGroup_t **del_groups,
                           size_t del_group_cnt,
                           const rd_kafka_AdminOptions_t *options,
                           rd_kafka_queue_t *rkqu);



/*
 * DeleteGroups result type and methods
 */

/**
 * @brief Get an array of group results from a DeleteGroups result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_group_result_t **rd_kafka_DeleteGroups_result_groups(
    const rd_kafka_DeleteGroups_result_t *result,
    size_t *cntp);

/**@}*/

/**
 * @name Admin API - ListConsumerGroupOffsets
 * @{
 *
 *
 */

/*! Represents consumer group committed offsets to be listed. */
typedef struct rd_kafka_ListConsumerGroupOffsets_s
    rd_kafka_ListConsumerGroupOffsets_t;

/**
 * @brief Create a new ListConsumerGroupOffsets object.
 *        This object is later passed to rd_kafka_ListConsumerGroupOffsets().
 *
 * @param group_id Consumer group id.
 * @param partitions Partitions to list committed offsets for.
 *                   Only the topic and partition fields are used.
 *
 * @returns a new allocated ListConsumerGroupOffsets object.
 *          Use rd_kafka_ListConsumerGroupOffsets_destroy() to free
 *          object when done.
 */
RD_EXPORT rd_kafka_ListConsumerGroupOffsets_t *
rd_kafka_ListConsumerGroupOffsets_new(
    const char *group_id,
    const rd_kafka_topic_partition_list_t *partitions);

/**
 * @brief Destroy and free a ListConsumerGroupOffsets object previously
 *        created with rd_kafka_ListConsumerGroupOffsets_new()
 */
RD_EXPORT void rd_kafka_ListConsumerGroupOffsets_destroy(
    rd_kafka_ListConsumerGroupOffsets_t *list_grpoffsets);

/**
 * @brief Helper function to destroy all ListConsumerGroupOffsets objects in
 *        the \p list_grpoffsets array (of \p list_grpoffsets_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void rd_kafka_ListConsumerGroupOffsets_destroy_array(
    rd_kafka_ListConsumerGroupOffsets_t **list_grpoffsets,
    size_t list_grpoffset_cnt);

/**
 * @brief List committed offsets for a set of partitions in a consumer
 *        group.
 *
 * @param rk Client instance.
 * @param list_grpoffsets Array of group committed offsets to list.
 *                       MUST only be one single element.
 * @param list_grpoffsets_cnt Number of elements in \p list_grpoffsets array.
 *                           MUST always be 1.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_LISTCONSUMERGROUPOFFSETS_RESULT
 *
 * @remark The current implementation only supports one group per invocation.
 */
RD_EXPORT
void rd_kafka_ListConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_ListConsumerGroupOffsets_t **list_grpoffsets,
    size_t list_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu);



/*
 * ListConsumerGroupOffsets result type and methods
 */

/**
 * @brief Get an array of results from a ListConsumerGroupOffsets result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p result object.
 */
RD_EXPORT const rd_kafka_group_result_t **
rd_kafka_ListConsumerGroupOffsets_result_groups(
    const rd_kafka_ListConsumerGroupOffsets_result_t *result,
    size_t *cntp);



/**@}*/

/**
 * @name Admin API - AlterConsumerGroupOffsets
 * @{
 *
 *
 */

/*! Represents consumer group committed offsets to be altered. */
typedef struct rd_kafka_AlterConsumerGroupOffsets_s
    rd_kafka_AlterConsumerGroupOffsets_t;

/**
 * @brief Create a new AlterConsumerGroupOffsets object.
 *        This object is later passed to rd_kafka_AlterConsumerGroupOffsets().
 *
 * @param group_id Consumer group id.
 * @param partitions Partitions to alter committed offsets for.
 *                   Only the topic and partition fields are used.
 *
 * @returns a new allocated AlterConsumerGroupOffsets object.
 *          Use rd_kafka_AlterConsumerGroupOffsets_destroy() to free
 *          object when done.
 */
RD_EXPORT rd_kafka_AlterConsumerGroupOffsets_t *
rd_kafka_AlterConsumerGroupOffsets_new(
    const char *group_id,
    const rd_kafka_topic_partition_list_t *partitions);

/**
 * @brief Destroy and free a AlterConsumerGroupOffsets object previously
 *        created with rd_kafka_AlterConsumerGroupOffsets_new()
 */
RD_EXPORT void rd_kafka_AlterConsumerGroupOffsets_destroy(
    rd_kafka_AlterConsumerGroupOffsets_t *alter_grpoffsets);

/**
 * @brief Helper function to destroy all AlterConsumerGroupOffsets objects in
 *        the \p alter_grpoffsets array (of \p alter_grpoffsets_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void rd_kafka_AlterConsumerGroupOffsets_destroy_array(
    rd_kafka_AlterConsumerGroupOffsets_t **alter_grpoffsets,
    size_t alter_grpoffset_cnt);

/**
 * @brief Alter committed offsets for a set of partitions in a consumer
 *        group. This will succeed at the partition level only if the group
 *        is not actively subscribed to the corresponding topic.
 *
 * @param rk Client instance.
 * @param alter_grpoffsets Array of group committed offsets to alter.
 *                       MUST only be one single element.
 * @param alter_grpoffsets_cnt Number of elements in \p alter_grpoffsets array.
 *                           MUST always be 1.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_ALTERCONSUMERGROUPOFFSETS_RESULT
 *
 * @remark The current implementation only supports one group per invocation.
 */
RD_EXPORT
void rd_kafka_AlterConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_AlterConsumerGroupOffsets_t **alter_grpoffsets,
    size_t alter_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu);



/*
 * AlterConsumerGroupOffsets result type and methods
 */

/**
 * @brief Get an array of results from a AlterConsumerGroupOffsets result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 *
 * @remark The lifetime of the returned memory is the same
 *         as the lifetime of the \p result object.
 */
RD_EXPORT const rd_kafka_group_result_t **
rd_kafka_AlterConsumerGroupOffsets_result_groups(
    const rd_kafka_AlterConsumerGroupOffsets_result_t *result,
    size_t *cntp);



/**@}*/

/**
 * @name Admin API - DeleteConsumerGroupOffsets
 * @{
 *
 *
 */

/*! Represents consumer group committed offsets to be deleted. */
typedef struct rd_kafka_DeleteConsumerGroupOffsets_s
    rd_kafka_DeleteConsumerGroupOffsets_t;

/**
 * @brief Create a new DeleteConsumerGroupOffsets object.
 *        This object is later passed to rd_kafka_DeleteConsumerGroupOffsets().
 *
 * @param group Consumer group id.
 * @param partitions Partitions to delete committed offsets for.
 *                   Only the topic and partition fields are used.
 *
 * @returns a new allocated DeleteConsumerGroupOffsets object.
 *          Use rd_kafka_DeleteConsumerGroupOffsets_destroy() to free
 *          object when done.
 */
RD_EXPORT rd_kafka_DeleteConsumerGroupOffsets_t *
rd_kafka_DeleteConsumerGroupOffsets_new(
    const char *group,
    const rd_kafka_topic_partition_list_t *partitions);

/**
 * @brief Destroy and free a DeleteConsumerGroupOffsets object previously
 *        created with rd_kafka_DeleteConsumerGroupOffsets_new()
 */
RD_EXPORT void rd_kafka_DeleteConsumerGroupOffsets_destroy(
    rd_kafka_DeleteConsumerGroupOffsets_t *del_grpoffsets);

/**
 * @brief Helper function to destroy all DeleteConsumerGroupOffsets objects in
 *        the \p del_grpoffsets array (of \p del_grpoffsets_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void rd_kafka_DeleteConsumerGroupOffsets_destroy_array(
    rd_kafka_DeleteConsumerGroupOffsets_t **del_grpoffsets,
    size_t del_grpoffset_cnt);

/**
 * @brief Delete committed offsets for a set of partitions in a consumer
 *        group. This will succeed at the partition level only if the group
 *        is not actively subscribed to the corresponding topic.
 *
 * @param rk Client instance.
 * @param del_grpoffsets Array of group committed offsets to delete.
 *                       MUST only be one single element.
 * @param del_grpoffsets_cnt Number of elements in \p del_grpoffsets array.
 *                           MUST always be 1.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DELETECONSUMERGROUPOFFSETS_RESULT
 *
 * @remark The current implementation only supports one group per invocation.
 */
RD_EXPORT
void rd_kafka_DeleteConsumerGroupOffsets(
    rd_kafka_t *rk,
    rd_kafka_DeleteConsumerGroupOffsets_t **del_grpoffsets,
    size_t del_grpoffsets_cnt,
    const rd_kafka_AdminOptions_t *options,
    rd_kafka_queue_t *rkqu);



/*
 * DeleteConsumerGroupOffsets result type and methods
 */

/**
 * @brief Get an array of results from a DeleteConsumerGroupOffsets result.
 *
 * The returned groups life-time is the same as the \p result object.
 *
 * @param result Result to get group results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_group_result_t **
rd_kafka_DeleteConsumerGroupOffsets_result_groups(
    const rd_kafka_DeleteConsumerGroupOffsets_result_t *result,
    size_t *cntp);

/**@}*/

/**
 * @name Admin API - ACL operations
 * @{
 */

/**
 * @brief ACL Binding is used to create access control lists.
 *
 *
 */
typedef struct rd_kafka_AclBinding_s rd_kafka_AclBinding_t;

/**
 * @brief ACL Binding filter is used to filter access control lists.
 *
 */
typedef rd_kafka_AclBinding_t rd_kafka_AclBindingFilter_t;

/**
 * @returns the error object for the given acl result, or NULL on success.
 */
RD_EXPORT const rd_kafka_error_t *
rd_kafka_acl_result_error(const rd_kafka_acl_result_t *aclres);


/**
 * @enum rd_kafka_AclOperation_t
 * @brief Apache Kafka ACL operation types.
 */
typedef enum rd_kafka_AclOperation_t {
        RD_KAFKA_ACL_OPERATION_UNKNOWN = 0, /**< Unknown */
        RD_KAFKA_ACL_OPERATION_ANY =
            1, /**< In a filter, matches any AclOperation */
        RD_KAFKA_ACL_OPERATION_ALL      = 2, /**< ALL operation */
        RD_KAFKA_ACL_OPERATION_READ     = 3, /**< READ operation */
        RD_KAFKA_ACL_OPERATION_WRITE    = 4, /**< WRITE operation */
        RD_KAFKA_ACL_OPERATION_CREATE   = 5, /**< CREATE operation */
        RD_KAFKA_ACL_OPERATION_DELETE   = 6, /**< DELETE operation */
        RD_KAFKA_ACL_OPERATION_ALTER    = 7, /**< ALTER operation */
        RD_KAFKA_ACL_OPERATION_DESCRIBE = 8, /**< DESCRIBE operation */
        RD_KAFKA_ACL_OPERATION_CLUSTER_ACTION =
            9, /**< CLUSTER_ACTION operation */
        RD_KAFKA_ACL_OPERATION_DESCRIBE_CONFIGS =
            10, /**< DESCRIBE_CONFIGS operation */
        RD_KAFKA_ACL_OPERATION_ALTER_CONFIGS =
            11, /**< ALTER_CONFIGS  operation */
        RD_KAFKA_ACL_OPERATION_IDEMPOTENT_WRITE =
            12, /**< IDEMPOTENT_WRITE operation */
        RD_KAFKA_ACL_OPERATION__CNT
} rd_kafka_AclOperation_t;

/**
 * @returns a string representation of the \p acl_operation
 */
RD_EXPORT const char *
rd_kafka_AclOperation_name(rd_kafka_AclOperation_t acl_operation);

/**
 * @enum rd_kafka_AclPermissionType_t
 * @brief Apache Kafka ACL permission types.
 */
typedef enum rd_kafka_AclPermissionType_t {
        RD_KAFKA_ACL_PERMISSION_TYPE_UNKNOWN = 0, /**< Unknown */
        RD_KAFKA_ACL_PERMISSION_TYPE_ANY =
            1, /**< In a filter, matches any AclPermissionType */
        RD_KAFKA_ACL_PERMISSION_TYPE_DENY  = 2, /**< Disallows access */
        RD_KAFKA_ACL_PERMISSION_TYPE_ALLOW = 3, /**< Grants access. */
        RD_KAFKA_ACL_PERMISSION_TYPE__CNT
} rd_kafka_AclPermissionType_t;

/**
 * @returns a string representation of the \p acl_permission_type
 */
RD_EXPORT const char *rd_kafka_AclPermissionType_name(
    rd_kafka_AclPermissionType_t acl_permission_type);

/**
 * @brief Create a new AclBinding object. This object is later passed to
 *        rd_kafka_CreateAcls().
 *
 * @param restype The ResourceType.
 * @param name The resource name.
 * @param resource_pattern_type The pattern type.
 * @param principal A principal, following the kafka specification.
 * @param host An hostname or ip.
 * @param operation A Kafka operation.
 * @param permission_type A Kafka permission type.
 * @param errstr An error string for returning errors or NULL to not use it.
 * @param errstr_size The \p errstr size or 0 to not use it.
 *
 * @returns a new allocated AclBinding object, or NULL if the input parameters
 *          are invalid.
 *          Use rd_kafka_AclBinding_destroy() to free object when done.
 */
RD_EXPORT rd_kafka_AclBinding_t *
rd_kafka_AclBinding_new(rd_kafka_ResourceType_t restype,
                        const char *name,
                        rd_kafka_ResourcePatternType_t resource_pattern_type,
                        const char *principal,
                        const char *host,
                        rd_kafka_AclOperation_t operation,
                        rd_kafka_AclPermissionType_t permission_type,
                        char *errstr,
                        size_t errstr_size);

/**
 * @brief Create a new AclBindingFilter object. This object is later passed to
 *        rd_kafka_DescribeAcls() or
 *        rd_kafka_DeletesAcls() in order to filter
 *        the acls to retrieve or to delete.
 *        Use the same rd_kafka_AclBinding functions to query or destroy it.
 *
 * @param restype The ResourceType or \c RD_KAFKA_RESOURCE_ANY if
 *                not filtering by this field.
 * @param name The resource name or NULL if not filtering by this field.
 * @param resource_pattern_type The pattern type or \c
 * RD_KAFKA_RESOURCE_PATTERN_ANY if not filtering by this field.
 * @param principal A principal or NULL if not filtering by this field.
 * @param host An hostname or ip or NULL if not filtering by this field.
 * @param operation A Kafka operation or \c RD_KAFKA_ACL_OPERATION_ANY if not
 * filtering by this field.
 * @param permission_type A Kafka permission type or \c
 * RD_KAFKA_ACL_PERMISSION_TYPE_ANY if not filtering by this field.
 * @param errstr An error string for returning errors or NULL to not use it.
 * @param errstr_size The \p errstr size or 0 to not use it.
 *
 * @returns a new allocated AclBindingFilter object, or NULL if the input
 * parameters are invalid. Use rd_kafka_AclBinding_destroy() to free object when
 * done.
 */
RD_EXPORT rd_kafka_AclBindingFilter_t *rd_kafka_AclBindingFilter_new(
    rd_kafka_ResourceType_t restype,
    const char *name,
    rd_kafka_ResourcePatternType_t resource_pattern_type,
    const char *principal,
    const char *host,
    rd_kafka_AclOperation_t operation,
    rd_kafka_AclPermissionType_t permission_type,
    char *errstr,
    size_t errstr_size);

/**
 * @returns the resource type for the given acl binding.
 */
RD_EXPORT rd_kafka_ResourceType_t
rd_kafka_AclBinding_restype(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the resource name for the given acl binding.
 *
 * @remark lifetime of the returned string is the same as the \p acl.
 */
RD_EXPORT const char *
rd_kafka_AclBinding_name(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the principal for the given acl binding.
 *
 * @remark lifetime of the returned string is the same as the \p acl.
 */
RD_EXPORT const char *
rd_kafka_AclBinding_principal(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the host for the given acl binding.
 *
 * @remark lifetime of the returned string is the same as the \p acl.
 */
RD_EXPORT const char *
rd_kafka_AclBinding_host(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the acl operation for the given acl binding.
 */
RD_EXPORT rd_kafka_AclOperation_t
rd_kafka_AclBinding_operation(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the permission type for the given acl binding.
 */
RD_EXPORT rd_kafka_AclPermissionType_t
rd_kafka_AclBinding_permission_type(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the resource pattern type for the given acl binding.
 */
RD_EXPORT rd_kafka_ResourcePatternType_t
rd_kafka_AclBinding_resource_pattern_type(const rd_kafka_AclBinding_t *acl);

/**
 * @returns the error object for the given acl binding, or NULL on success.
 */
RD_EXPORT const rd_kafka_error_t *
rd_kafka_AclBinding_error(const rd_kafka_AclBinding_t *acl);


/**
 * @brief Destroy and free an AclBinding object previously created with
 *        rd_kafka_AclBinding_new()
 */
RD_EXPORT void rd_kafka_AclBinding_destroy(rd_kafka_AclBinding_t *acl_binding);


/**
 * @brief Helper function to destroy all AclBinding objects in
 *        the \p acl_bindings array (of \p acl_bindings_cnt elements).
 *        The array itself is not freed.
 */
RD_EXPORT void
rd_kafka_AclBinding_destroy_array(rd_kafka_AclBinding_t **acl_bindings,
                                  size_t acl_bindings_cnt);

/**
 * @brief Get an array of acl results from a CreateAcls result.
 *
 * The returned \p acl result life-time is the same as the \p result object.
 * @param result CreateAcls result to get acl results from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_acl_result_t **
rd_kafka_CreateAcls_result_acls(const rd_kafka_CreateAcls_result_t *result,
                                size_t *cntp);

/**
 * @brief Create acls as specified by the \p new_acls
 *        array of size \p new_topic_cnt elements.
 *
 * @param rk Client instance.
 * @param new_acls Array of new acls to create.
 * @param new_acls_cnt Number of elements in \p new_acls array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_request_timeout() - default socket.timeout.ms
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_CREATEACLS_RESULT
 */
RD_EXPORT void rd_kafka_CreateAcls(rd_kafka_t *rk,
                                   rd_kafka_AclBinding_t **new_acls,
                                   size_t new_acls_cnt,
                                   const rd_kafka_AdminOptions_t *options,
                                   rd_kafka_queue_t *rkqu);

/**
 * DescribeAcls - describe access control lists.
 *
 *
 */

/**
 * @brief Get an array of resource results from a DescribeAcls result.
 *
 * The returned \p resources life-time is the same as the \p result object.
 * @param result DescribeAcls result to get acls from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_AclBinding_t **
rd_kafka_DescribeAcls_result_acls(const rd_kafka_DescribeAcls_result_t *result,
                                  size_t *cntp);

/**
 * @brief Describe acls matching the filter provided in \p acl_filter
 *
 * @param rk Client instance.
 * @param acl_filter Filter for the returned acls.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_operation_timeout() - default 0
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DESCRIBEACLS_RESULT
 */
RD_EXPORT void rd_kafka_DescribeAcls(rd_kafka_t *rk,
                                     rd_kafka_AclBindingFilter_t *acl_filter,
                                     const rd_kafka_AdminOptions_t *options,
                                     rd_kafka_queue_t *rkqu);

/**
 * DeleteAcls - delete access control lists.
 *
 *
 */

typedef struct rd_kafka_DeleteAcls_result_response_s
    rd_kafka_DeleteAcls_result_response_t;

/**
 * @brief Get an array of DeleteAcls result responses from a DeleteAcls result.
 *
 * The returned \p responses life-time is the same as the \p result object.
 * @param result DeleteAcls result to get responses from.
 * @param cntp is updated to the number of elements in the array.
 */
RD_EXPORT const rd_kafka_DeleteAcls_result_response_t **
rd_kafka_DeleteAcls_result_responses(const rd_kafka_DeleteAcls_result_t *result,
                                     size_t *cntp);

/**
 * @returns the error object for the given DeleteAcls result response,
 *          or NULL on success.
 */
RD_EXPORT const rd_kafka_error_t *rd_kafka_DeleteAcls_result_response_error(
    const rd_kafka_DeleteAcls_result_response_t *result_response);


/**
 * @returns the matching acls array for the given DeleteAcls result response.
 *
 * @remark lifetime of the returned acl bindings is the same as the \p
 * result_response.
 */
RD_EXPORT const rd_kafka_AclBinding_t **
rd_kafka_DeleteAcls_result_response_matching_acls(
    const rd_kafka_DeleteAcls_result_response_t *result_response,
    size_t *matching_acls_cntp);

/**
 * @brief Delete acls matching the filteres provided in \p del_acls
 * array of size \p del_acls_cnt.
 *
 * @param rk Client instance.
 * @param del_acls Filters for the acls to delete.
 * @param del_acls_cnt Number of elements in \p del_acls array.
 * @param options Optional admin options, or NULL for defaults.
 * @param rkqu Queue to emit result on.
 *
 * Supported admin options:
 *  - rd_kafka_AdminOptions_set_operation_timeout() - default 0
 *
 * @remark The result event type emitted on the supplied queue is of type
 *         \c RD_KAFKA_EVENT_DELETEACLS_RESULT
 */
RD_EXPORT void rd_kafka_DeleteAcls(rd_kafka_t *rk,
                                   rd_kafka_AclBindingFilter_t **del_acls,
                                   size_t del_acls_cnt,
                                   const rd_kafka_AdminOptions_t *options,
                                   rd_kafka_queue_t *rkqu);

/**@}*/

/**
 * @name Security APIs
 * @{
 *
 */

/**
 * @brief Set SASL/OAUTHBEARER token and metadata
 *
 * @param rk Client instance.
 * @param token_value the mandatory token value to set, often (but not
 *  necessarily) a JWS compact serialization as per
 *  https://tools.ietf.org/html/rfc7515#section-3.1.
 * @param md_lifetime_ms when the token expires, in terms of the number of
 *  milliseconds since the epoch.
 * @param md_principal_name the mandatory Kafka principal name associated
 *  with the token.
 * @param extensions optional SASL extensions key-value array with
 *  \p extensions_size elements (number of keys * 2), where [i] is the key and
 *  [i+1] is the key's value, to be communicated to the broker
 *  as additional key-value pairs during the initial client response as per
 *  https://tools.ietf.org/html/rfc7628#section-3.1. The key-value pairs are
 *  copied.
 * @param extension_size the number of SASL extension keys plus values,
 *  which must be a non-negative multiple of 2.
 * @param errstr A human readable error string (nul-terminated) is written to
 *               this location that must be of at least \p errstr_size bytes.
 *               The \p errstr is only written in case of error.
 * @param errstr_size Writable size in \p errstr.
 *
 * The SASL/OAUTHBEARER token refresh callback or event handler should invoke
 * this method upon success. The extension keys must not include the reserved
 * key "`auth`", and all extension keys and values must conform to the required
 * format as per https://tools.ietf.org/html/rfc7628#section-3.1:
 *
 *     key            = 1*(ALPHA)
 *     value          = *(VCHAR / SP / HTAB / CR / LF )
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, otherwise \p errstr set
 *              and:<br>
 *          \c RD_KAFKA_RESP_ERR__INVALID_ARG if any of the arguments are
 *              invalid;<br>
 *          \c RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED if SASL/OAUTHBEARER is not
 *              supported by this build;<br>
 *          \c RD_KAFKA_RESP_ERR__STATE if SASL/OAUTHBEARER is supported but is
 *              not configured as the client's authentication mechanism.<br>
 *
 * @sa rd_kafka_oauthbearer_set_token_failure
 * @sa rd_kafka_conf_set_oauthbearer_token_refresh_cb
 */
RD_EXPORT
rd_kafka_resp_err_t
rd_kafka_oauthbearer_set_token(rd_kafka_t *rk,
                               const char *token_value,
                               int64_t md_lifetime_ms,
                               const char *md_principal_name,
                               const char **extensions,
                               size_t extension_size,
                               char *errstr,
                               size_t errstr_size);

/**
 * @brief SASL/OAUTHBEARER token refresh failure indicator.
 *
 * @param rk Client instance.
 * @param errstr mandatory human readable error reason for failing to acquire
 *  a token.
 *
 * The SASL/OAUTHBEARER token refresh callback or event handler should invoke
 * this method upon failure.
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, otherwise:<br>
 *          \c RD_KAFKA_RESP_ERR__NOT_IMPLEMENTED if SASL/OAUTHBEARER is not
 *              supported by this build;<br>
 *          \c RD_KAFKA_RESP_ERR__STATE if SASL/OAUTHBEARER is supported but is
 *              not configured as the client's authentication mechanism,<br>
 *          \c RD_KAFKA_RESP_ERR__INVALID_ARG if no error string is supplied.
 *
 * @sa rd_kafka_oauthbearer_set_token
 * @sa rd_kafka_conf_set_oauthbearer_token_refresh_cb
 */
RD_EXPORT
rd_kafka_resp_err_t rd_kafka_oauthbearer_set_token_failure(rd_kafka_t *rk,
                                                           const char *errstr);

/**@}*/


/**
 * @name Transactional producer API
 *
 * The transactional producer operates on top of the idempotent producer,
 * and provides full exactly-once semantics (EOS) for Apache Kafka when used
 * with the transaction aware consumer (\c isolation.level=read_committed).
 *
 * A producer instance is configured for transactions by setting the
 * \c transactional.id to an identifier unique for the application. This
 * id will be used to fence stale transactions from previous instances of
 * the application, typically following an outage or crash.
 *
 * After creating the transactional producer instance using rd_kafka_new()
 * the transactional state must be initialized by calling
 * rd_kafka_init_transactions(). This is a blocking call that will
 * acquire a runtime producer id from the transaction coordinator broker
 * as well as abort any stale transactions and fence any still running producer
 * instances with the same \c transactional.id.
 *
 * Once transactions are initialized the application may begin a new
 * transaction by calling rd_kafka_begin_transaction().
 * A producer instance may only have one single on-going transaction.
 *
 * Any messages produced after the transaction has been started will
 * belong to the ongoing transaction and will be committed or aborted
 * atomically.
 * It is not permitted to produce messages outside a transaction
 * boundary, e.g., before rd_kafka_begin_transaction() or after
 * rd_kafka_commit_transaction(), rd_kafka_abort_transaction(), or after
 * the current transaction has failed.
 *
 * If consumed messages are used as input to the transaction, the consumer
 * instance must be configured with \c enable.auto.commit set to \c false.
 * To commit the consumed offsets along with the transaction pass the
 * list of consumed partitions and the last offset processed + 1 to
 * rd_kafka_send_offsets_to_transaction() prior to committing the transaction.
 * This allows an aborted transaction to be restarted using the previously
 * committed offsets.
 *
 * To commit the produced messages, and any consumed offsets, to the
 * current transaction, call rd_kafka_commit_transaction().
 * This call will block until the transaction has been fully committed or
 * failed (typically due to fencing by a newer producer instance).
 *
 * Alternatively, if processing fails, or an abortable transaction error is
 * raised, the transaction needs to be aborted by calling
 * rd_kafka_abort_transaction() which marks any produced messages and
 * offset commits as aborted.
 *
 * After the current transaction has been committed or aborted a new
 * transaction may be started by calling rd_kafka_begin_transaction() again.
 *
 * @par Retriable errors
 * Some error cases allow the attempted operation to be retried, this is
 * indicated by the error object having the retriable flag set which can
 * be detected by calling rd_kafka_error_is_retriable().
 * When this flag is set the application may retry the operation immediately
 * or preferably after a shorter grace period (to avoid busy-looping).
 * Retriable errors include timeouts, broker transport failures, etc.
 *
 * @par Abortable errors
 * An ongoing transaction may fail permanently due to various errors,
 * such as transaction coordinator becoming unavailable, write failures to the
 * Apache Kafka log, under-replicated partitions, etc.
 * At this point the producer application must abort the current transaction
 * using rd_kafka_abort_transaction() and optionally start a new transaction
 * by calling rd_kafka_begin_transaction().
 * Whether an error is abortable or not is detected by calling
 * rd_kafka_error_txn_requires_abort() on the returned error object.
 *
 * @par Fatal errors
 * While the underlying idempotent producer will typically only raise
 * fatal errors for unrecoverable cluster errors where the idempotency
 * guarantees can't be maintained, most of these are treated as abortable by
 * the transactional producer since transactions may be aborted and retried
 * in their entirety;
 * The transactional producer on the other hand introduces a set of additional
 * fatal errors which the application needs to handle by shutting down the
 * producer and terminate. There is no way for a producer instance to recover
 * from fatal errors.
 * Whether an error is fatal or not is detected by calling
 * rd_kafka_error_is_fatal() on the returned error object or by checking
 * the global rd_kafka_fatal_error() code.
 * Fatal errors are raised by triggering the \c error_cb (see the
 * Fatal error chapter in INTRODUCTION.md for more information), and any
 * subsequent transactional API calls will return RD_KAFKA_RESP_ERR__FATAL
 * or have the fatal flag set (see rd_kafka_error_is_fatal()).
 * The originating fatal error code can be retrieved by calling
 * rd_kafka_fatal_error().
 *
 * @par Handling of other errors
 * For errors that have neither retriable, abortable or the fatal flag set
 * it is not always obvious how to handle them. While some of these errors
 * may be indicative of bugs in the application code, such as when
 * an invalid parameter is passed to a method, other errors might originate
 * from the broker and be passed thru as-is to the application.
 * The general recommendation is to treat these errors, that have
 * neither the retriable or abortable flags set, as fatal.
 *
 * @par Error handling example
 * @code
 *     retry:
 *        rd_kafka_error_t *error;
 *
 *        error = rd_kafka_commit_transaction(producer, 10*1000);
 *        if (!error)
 *            return success;
 *        else if (rd_kafka_error_txn_requires_abort(error)) {
 *            do_abort_transaction_and_reset_inputs();
 *        } else if (rd_kafka_error_is_retriable(error)) {
 *            rd_kafka_error_destroy(error);
 *            goto retry;
 *        } else { // treat all other errors as fatal errors
 *            fatal_error(rd_kafka_error_string(error));
 *        }
 *        rd_kafka_error_destroy(error);
 * @endcode
 *
 *
 * @{
 */


/**
 * @brief Initialize transactions for the producer instance.
 *
 * This function ensures any transactions initiated by previous instances
 * of the producer with the same \c transactional.id are completed.
 * If the previous instance failed with a transaction in progress the
 * previous transaction will be aborted.
 * This function needs to be called before any other transactional or
 * produce functions are called when the \c transactional.id is configured.
 *
 * If the last transaction had begun completion (following transaction commit)
 * but not yet finished, this function will await the previous transaction's
 * completion.
 *
 * When any previous transactions have been fenced this function
 * will acquire the internal producer id and epoch, used in all future
 * transactional messages issued by this producer instance.
 *
 * @param rk Producer instance.
 * @param timeout_ms The maximum time to block. On timeout the operation
 *                   may continue in the background, depending on state,
 *                   and it is okay to call init_transactions() again.
 *                   If an infinite timeout (-1) is passed, the timeout will
 *                   be adjusted to 2 * \c transaction.timeout.ms.
 *
 * @remark This function may block up to \p timeout_ms milliseconds.
 *
 * @remark This call is resumable when a retriable timeout error is returned.
 *         Calling the function again will resume the operation that is
 *         progressing in the background.
 *
 * @returns NULL on success or an error object on failure.
 *          Check whether the returned error object permits retrying
 *          by calling rd_kafka_error_is_retriable(), or whether a fatal
 *          error has been raised by calling rd_kafka_error_is_fatal().
 *          Error codes:
 *          RD_KAFKA_RESP_ERR__TIMED_OUT if the transaction coordinator
 *          could be not be contacted within \p timeout_ms (retriable),
 *          RD_KAFKA_RESP_ERR_COORDINATOR_NOT_AVAILABLE if the transaction
 *          coordinator is not available (retriable),
 *          RD_KAFKA_RESP_ERR_CONCURRENT_TRANSACTIONS if a previous transaction
 *          would not complete within \p timeout_ms (retriable),
 *          RD_KAFKA_RESP_ERR__STATE if transactions have already been started
 *          or upon fatal error,
 *          RD_KAFKA_RESP_ERR__UNSUPPORTED_FEATURE if the broker(s) do not
 *          support transactions (<Apache Kafka 0.11), this also raises a
 *          fatal error,
 *          RD_KAFKA_RESP_ERR_INVALID_TRANSACTION_TIMEOUT if the configured
 *          \c transaction.timeout.ms is outside the broker-configured range,
 *          this also raises a fatal error,
 *          RD_KAFKA_RESP_ERR__NOT_CONFIGURED if transactions have not been
 *          configured for the producer instance,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \p rk is not a producer instance,
 *          or \p timeout_ms is out of range.
 *          Other error codes not listed here may be returned, depending on
 *          broker version.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_init_transactions(rd_kafka_t *rk, int timeout_ms);



/**
 * @brief Begin a new transaction.
 *
 * rd_kafka_init_transactions() must have been called successfully (once)
 * before this function is called.
 *
 * Upon successful return from this function the application has to perform at
 * least one of the following operations within \c transaction.timeout.ms to
 * avoid timing out the transaction on the broker:
 *   * rd_kafka_produce() (et.al)
 *   * rd_kafka_send_offsets_to_transaction()
 *   * rd_kafka_commit_transaction()
 *   * rd_kafka_abort_transaction()
 *
 * Any messages produced, offsets sent (rd_kafka_send_offsets_to_transaction()),
 * etc, after the successful return of this function will be part of
 * the transaction and committed or aborted atomatically.
 *
 * Finish the transaction by calling rd_kafka_commit_transaction() or
 * abort the transaction by calling rd_kafka_abort_transaction().
 *
 * @param rk Producer instance.
 *
 * @returns NULL on success or an error object on failure.
 *          Check whether a fatal error has been raised by
 *          calling rd_kafka_error_is_fatal().
 *          Error codes:
 *          RD_KAFKA_RESP_ERR__STATE if a transaction is already in progress
 *          or upon fatal error,
 *          RD_KAFKA_RESP_ERR__NOT_CONFIGURED if transactions have not been
 *          configured for the producer instance,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \p rk is not a producer instance.
 *          Other error codes not listed here may be returned, depending on
 *          broker version.
 *
 * @remark With the transactional producer, rd_kafka_produce(),
 *         rd_kafka_producev(), et.al, are only allowed during an on-going
 *         transaction, as started with this function.
 *         Any produce call outside an on-going transaction, or for a failed
 *         transaction, will fail.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_begin_transaction(rd_kafka_t *rk);


/**
 * @brief Sends a list of topic partition offsets to the consumer group
 *        coordinator for \p cgmetadata, and marks the offsets as part
 *        part of the current transaction.
 *        These offsets will be considered committed only if the transaction is
 *        committed successfully.
 *
 *        The offsets should be the next message your application will consume,
 *        i.e., the last processed message's offset + 1 for each partition.
 *        Either track the offsets manually during processing or use
 *        rd_kafka_position() (on the consumer) to get the current offsets for
 *        the partitions assigned to the consumer.
 *
 *        Use this method at the end of a consume-transform-produce loop prior
 *        to committing the transaction with rd_kafka_commit_transaction().
 *
 * @param rk Producer instance.
 * @param offsets List of offsets to commit to the consumer group upon
 *                successful commit of the transaction. Offsets should be
 *                the next message to consume, e.g., last processed message + 1.
 * @param cgmetadata The current consumer group metadata as returned by
 *                   rd_kafka_consumer_group_metadata() on the consumer
 *                   instance the provided offsets were consumed from.
 * @param timeout_ms Maximum time allowed to register the offsets on the broker.
 *
 * @remark This function must be called on the transactional producer instance,
 *         not the consumer.
 *
 * @remark The consumer must disable auto commits
 *         (set \c enable.auto.commit to false on the consumer).
 *
 * @remark Logical and invalid offsets (such as RD_KAFKA_OFFSET_INVALID) in
 *         \p offsets will be ignored, if there are no valid offsets in
 *         \p offsets the function will return NULL and no action will be taken.
 *
 * @remark This call is retriable but not resumable, which means a new request
 *         with a new set of provided offsets and group metadata will be
 *         sent to the transaction coordinator if the call is retried.
 *
 * @remark It is highly recommended to retry the call (upon retriable error)
 *         with identical \p offsets and \p cgmetadata parameters.
 *         Failure to do so risks inconsistent state between what is actually
 *         included in the transaction and what the application thinks is
 *         included in the transaction.
 *
 * @returns NULL on success or an error object on failure.
 *          Check whether the returned error object permits retrying
 *          by calling rd_kafka_error_is_retriable(), or whether an abortable
 *          or fatal error has been raised by calling
 *          rd_kafka_error_txn_requires_abort() or rd_kafka_error_is_fatal()
 *          respectively.
 *          Error codes:
 *          RD_KAFKA_RESP_ERR__STATE if not currently in a transaction,
 *          RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH if the current producer
 *          transaction has been fenced by a newer producer instance,
 *          RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED if the
 *          producer is no longer authorized to perform transactional
 *          operations,
 *          RD_KAFKA_RESP_ERR_GROUP_AUTHORIZATION_FAILED if the producer is
 *          not authorized to write the consumer offsets to the group
 *          coordinator,
 *          RD_KAFKA_RESP_ERR__NOT_CONFIGURED if transactions have not been
 *          configured for the producer instance,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \p rk is not a producer instance,
 *          or if the \p consumer_group_id or \p offsets are empty.
 *          Other error codes not listed here may be returned, depending on
 *          broker version.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_send_offsets_to_transaction(
    rd_kafka_t *rk,
    const rd_kafka_topic_partition_list_t *offsets,
    const rd_kafka_consumer_group_metadata_t *cgmetadata,
    int timeout_ms);


/**
 * @brief Commit the current transaction (as started with
 *        rd_kafka_begin_transaction()).
 *
 *        Any outstanding messages will be flushed (delivered) before actually
 *        committing the transaction.
 *
 *        If any of the outstanding messages fail permanently the current
 *        transaction will enter the abortable error state and this
 *        function will return an abortable error, in this case the application
 *        must call rd_kafka_abort_transaction() before attempting a new
 *        transaction with rd_kafka_begin_transaction().
 *
 * @param rk Producer instance.
 * @param timeout_ms The maximum time to block. On timeout the operation
 *                   may continue in the background, depending on state,
 *                   and it is okay to call this function again.
 *                   Pass -1 to use the remaining transaction timeout,
 *                   this is the recommended use.
 *
 * @remark It is strongly recommended to always pass -1 (remaining transaction
 *         time) as the \p timeout_ms. Using other values risk internal
 *         state desynchronization in case any of the underlying protocol
 *         requests fail.
 *
 * @remark This function will block until all outstanding messages are
 *         delivered and the transaction commit request has been successfully
 *         handled by the transaction coordinator, or until \p timeout_ms
 *         expires, which ever comes first. On timeout the application may
 *         call the function again.
 *
 * @remark Will automatically call rd_kafka_flush() to ensure all queued
 *         messages are delivered before attempting to commit the
 *         transaction.
 *         If the application has enabled RD_KAFKA_EVENT_DR it must
 *         serve the event queue in a separate thread since rd_kafka_flush()
 *         will not serve delivery reports in this mode.
 *
 * @remark This call is resumable when a retriable timeout error is returned.
 *         Calling the function again will resume the operation that is
 *         progressing in the background.
 *
 * @returns NULL on success or an error object on failure.
 *          Check whether the returned error object permits retrying
 *          by calling rd_kafka_error_is_retriable(), or whether an abortable
 *          or fatal error has been raised by calling
 *          rd_kafka_error_txn_requires_abort() or rd_kafka_error_is_fatal()
 *          respectively.
 *          Error codes:
 *          RD_KAFKA_RESP_ERR__STATE if not currently in a transaction,
 *          RD_KAFKA_RESP_ERR__TIMED_OUT if the transaction could not be
 *          complete commmitted within \p timeout_ms, this is a retriable
 *          error as the commit continues in the background,
 *          RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH if the current producer
 *          transaction has been fenced by a newer producer instance,
 *          RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED if the
 *          producer is no longer authorized to perform transactional
 *          operations,
 *          RD_KAFKA_RESP_ERR__NOT_CONFIGURED if transactions have not been
 *          configured for the producer instance,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \p rk is not a producer instance,
 *          Other error codes not listed here may be returned, depending on
 *          broker version.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_commit_transaction(rd_kafka_t *rk, int timeout_ms);


/**
 * @brief Aborts the ongoing transaction.
 *
 *        This function should also be used to recover from non-fatal abortable
 *        transaction errors.
 *
 *        Any outstanding messages will be purged and fail with
 *        RD_KAFKA_RESP_ERR__PURGE_INFLIGHT or RD_KAFKA_RESP_ERR__PURGE_QUEUE.
 *        See rd_kafka_purge() for details.
 *
 * @param rk Producer instance.
 * @param timeout_ms The maximum time to block. On timeout the operation
 *                   may continue in the background, depending on state,
 *                   and it is okay to call this function again.
 *                   Pass -1 to use the remaining transaction timeout,
 *                   this is the recommended use.
 *
 * @remark It is strongly recommended to always pass -1 (remaining transaction
 *         time) as the \p timeout_ms. Using other values risk internal
 *         state desynchronization in case any of the underlying protocol
 *         requests fail.
 *
 * @remark This function will block until all outstanding messages are purged
 *         and the transaction abort request has been successfully
 *         handled by the transaction coordinator, or until \p timeout_ms
 *         expires, which ever comes first. On timeout the application may
 *         call the function again.
 *         If the application has enabled RD_KAFKA_EVENT_DR it must
 *         serve the event queue in a separate thread since rd_kafka_flush()
 *         will not serve delivery reports in this mode.
 *
 * @remark This call is resumable when a retriable timeout error is returned.
 *         Calling the function again will resume the operation that is
 *         progressing in the background.
 *
 * @returns NULL on success or an error object on failure.
 *          Check whether the returned error object permits retrying
 *          by calling rd_kafka_error_is_retriable(), or whether a fatal error
 *          has been raised by calling rd_kafka_error_is_fatal().
 *          Error codes:
 *          RD_KAFKA_RESP_ERR__STATE if not currently in a transaction,
 *          RD_KAFKA_RESP_ERR__TIMED_OUT if the transaction could not be
 *          complete commmitted within \p timeout_ms, this is a retriable
 *          error as the commit continues in the background,
 *          RD_KAFKA_RESP_ERR_INVALID_PRODUCER_EPOCH if the current producer
 *          transaction has been fenced by a newer producer instance,
 *          RD_KAFKA_RESP_ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED if the
 *          producer is no longer authorized to perform transactional
 *          operations,
 *          RD_KAFKA_RESP_ERR__NOT_CONFIGURED if transactions have not been
 *          configured for the producer instance,
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if \p rk is not a producer instance,
 *          Other error codes not listed here may be returned, depending on
 *          broker version.
 *
 * @remark The returned error object (if not NULL) must be destroyed with
 *         rd_kafka_error_destroy().
 */
RD_EXPORT
rd_kafka_error_t *rd_kafka_abort_transaction(rd_kafka_t *rk, int timeout_ms);


/**@}*/

/* @cond NO_DOC */
#ifdef __cplusplus
}
#endif
#endif /* _RDKAFKA_H_ */
/* @endcond NO_DOC */
