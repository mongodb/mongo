/*
 * librdkafka - Apache Kafka C/C++ library
 *
 * Copyright (c) 2014-2022 Magnus Edenhill
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

#ifndef _RDKAFKACPP_H_
#define _RDKAFKACPP_H_

/**
 * @file rdkafkacpp.h
 * @brief Apache Kafka C/C++ consumer and producer client library.
 *
 * rdkafkacpp.h contains the public C++ API for librdkafka.
 * The API is documented in this file as comments prefixing the class,
 * function, type, enum, define, etc.
 * For more information, see the C interface in rdkafka.h and read the
 * manual in INTRODUCTION.md.
 * The C++ interface is STD C++ '03 compliant and adheres to the
 * Google C++ Style Guide.

 * @sa For the C interface see rdkafka.h
 *
 * @tableofcontents
 */

/**@cond NO_DOC*/
#include <string>
#include <list>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/types.h>

#ifdef _WIN32
#ifndef ssize_t
#ifndef _BASETSD_H_
#include <basetsd.h>
#endif
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif
#endif
#undef RD_EXPORT
#ifdef LIBRDKAFKA_STATICLIB
#define RD_EXPORT
#else
#ifdef LIBRDKAFKACPP_EXPORTS
#define RD_EXPORT __declspec(dllexport)
#else
#define RD_EXPORT __declspec(dllimport)
#endif
#endif
#else
#define RD_EXPORT
#endif

/**@endcond*/

extern "C" {
/* Forward declarations */
struct rd_kafka_s;
struct rd_kafka_topic_s;
struct rd_kafka_message_s;
struct rd_kafka_conf_s;
struct rd_kafka_topic_conf_s;
}

namespace RdKafka {

/**
 * @name Miscellaneous APIs
 * @{
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
 *         for runtime checks of version use RdKafka::version()
 */
#define RD_KAFKA_VERSION 0x020002ff

/**
 * @brief Returns the librdkafka version as integer.
 *
 * @sa See RD_KAFKA_VERSION for how to parse the integer format.
 */
RD_EXPORT
int version();

/**
 * @brief Returns the librdkafka version as string.
 */
RD_EXPORT
std::string version_str();

/**
 * @brief Returns a CSV list of the supported debug contexts
 *        for use with Conf::Set("debug", ..).
 */
RD_EXPORT
std::string get_debug_contexts();

/**
 * @brief Wait for all rd_kafka_t objects to be destroyed.
 *
 * @returns 0 if all kafka objects are now destroyed, or -1 if the
 * timeout was reached.
 * Since RdKafka handle deletion is an asynch operation the
 * \p wait_destroyed() function can be used for applications where
 * a clean shutdown is required.
 */
RD_EXPORT
int wait_destroyed(int timeout_ms);

/**
 * @brief Allocate memory using the same allocator librdkafka uses.
 *
 * This is typically an abstraction for the malloc(3) call and makes sure
 * the application can use the same memory allocator as librdkafka for
 * allocating pointers that are used by librdkafka.
 *
 * @remark Memory allocated by mem_malloc() must be freed using
 *         mem_free().
 */
RD_EXPORT
void *mem_malloc(size_t size);

/**
 * @brief Free pointer returned by librdkafka
 *
 * This is typically an abstraction for the free(3) call and makes sure
 * the application can use the same memory allocator as librdkafka for
 * freeing pointers returned by librdkafka.
 *
 * In standard setups it is usually not necessary to use this interface
 * rather than the free(3) function.
 *
 * @remark mem_free() must only be used for pointers returned by APIs
 *         that explicitly mention using this function for freeing.
 */
RD_EXPORT
void mem_free(void *ptr);

/**@}*/



/**
 * @name Constants, errors, types
 * @{
 *
 *
 */

/**
 * @brief Error codes.
 *
 * The negative error codes delimited by two underscores
 * (\c _ERR__..) denotes errors internal to librdkafka and are
 * displayed as \c \"Local: \<error string..\>\", while the error codes
 * delimited by a single underscore (\c ERR_..) denote broker
 * errors and are displayed as \c \"Broker: \<error string..\>\".
 *
 * @sa Use RdKafka::err2str() to translate an error code a human readable string
 */
enum ErrorCode {
  /* Internal errors to rdkafka: */
  /** Begin internal error codes */
  ERR__BEGIN = -200,
  /** Received message is incorrect */
  ERR__BAD_MSG = -199,
  /** Bad/unknown compression */
  ERR__BAD_COMPRESSION = -198,
  /** Broker is going away */
  ERR__DESTROY = -197,
  /** Generic failure */
  ERR__FAIL = -196,
  /** Broker transport failure */
  ERR__TRANSPORT = -195,
  /** Critical system resource */
  ERR__CRIT_SYS_RESOURCE = -194,
  /** Failed to resolve broker */
  ERR__RESOLVE = -193,
  /** Produced message timed out*/
  ERR__MSG_TIMED_OUT = -192,
  /** Reached the end of the topic+partition queue on
   *  the broker. Not really an error.
   *  This event is disabled by default,
   *  see the `enable.partition.eof` configuration property. */
  ERR__PARTITION_EOF = -191,
  /** Permanent: Partition does not exist in cluster. */
  ERR__UNKNOWN_PARTITION = -190,
  /** File or filesystem error */
  ERR__FS = -189,
  /** Permanent: Topic does not exist in cluster. */
  ERR__UNKNOWN_TOPIC = -188,
  /** All broker connections are down. */
  ERR__ALL_BROKERS_DOWN = -187,
  /** Invalid argument, or invalid configuration */
  ERR__INVALID_ARG = -186,
  /** Operation timed out */
  ERR__TIMED_OUT = -185,
  /** Queue is full */
  ERR__QUEUE_FULL = -184,
  /** ISR count < required.acks */
  ERR__ISR_INSUFF = -183,
  /** Broker node update */
  ERR__NODE_UPDATE = -182,
  /** SSL error */
  ERR__SSL = -181,
  /** Waiting for coordinator to become available. */
  ERR__WAIT_COORD = -180,
  /** Unknown client group */
  ERR__UNKNOWN_GROUP = -179,
  /** Operation in progress */
  ERR__IN_PROGRESS = -178,
  /** Previous operation in progress, wait for it to finish. */
  ERR__PREV_IN_PROGRESS = -177,
  /** This operation would interfere with an existing subscription */
  ERR__EXISTING_SUBSCRIPTION = -176,
  /** Assigned partitions (rebalance_cb) */
  ERR__ASSIGN_PARTITIONS = -175,
  /** Revoked partitions (rebalance_cb) */
  ERR__REVOKE_PARTITIONS = -174,
  /** Conflicting use */
  ERR__CONFLICT = -173,
  /** Wrong state */
  ERR__STATE = -172,
  /** Unknown protocol */
  ERR__UNKNOWN_PROTOCOL = -171,
  /** Not implemented */
  ERR__NOT_IMPLEMENTED = -170,
  /** Authentication failure*/
  ERR__AUTHENTICATION = -169,
  /** No stored offset */
  ERR__NO_OFFSET = -168,
  /** Outdated */
  ERR__OUTDATED = -167,
  /** Timed out in queue */
  ERR__TIMED_OUT_QUEUE = -166,
  /** Feature not supported by broker */
  ERR__UNSUPPORTED_FEATURE = -165,
  /** Awaiting cache update */
  ERR__WAIT_CACHE = -164,
  /** Operation interrupted */
  ERR__INTR = -163,
  /** Key serialization error */
  ERR__KEY_SERIALIZATION = -162,
  /** Value serialization error */
  ERR__VALUE_SERIALIZATION = -161,
  /** Key deserialization error */
  ERR__KEY_DESERIALIZATION = -160,
  /** Value deserialization error */
  ERR__VALUE_DESERIALIZATION = -159,
  /** Partial response */
  ERR__PARTIAL = -158,
  /** Modification attempted on read-only object */
  ERR__READ_ONLY = -157,
  /** No such entry / item not found */
  ERR__NOENT = -156,
  /** Read underflow */
  ERR__UNDERFLOW = -155,
  /** Invalid type */
  ERR__INVALID_TYPE = -154,
  /** Retry operation */
  ERR__RETRY = -153,
  /** Purged in queue */
  ERR__PURGE_QUEUE = -152,
  /** Purged in flight */
  ERR__PURGE_INFLIGHT = -151,
  /** Fatal error: see RdKafka::Handle::fatal_error() */
  ERR__FATAL = -150,
  /** Inconsistent state */
  ERR__INCONSISTENT = -149,
  /** Gap-less ordering would not be guaranteed if proceeding */
  ERR__GAPLESS_GUARANTEE = -148,
  /** Maximum poll interval exceeded */
  ERR__MAX_POLL_EXCEEDED = -147,
  /** Unknown broker */
  ERR__UNKNOWN_BROKER = -146,
  /** Functionality not configured */
  ERR__NOT_CONFIGURED = -145,
  /** Instance has been fenced */
  ERR__FENCED = -144,
  /** Application generated error */
  ERR__APPLICATION = -143,
  /** Assignment lost */
  ERR__ASSIGNMENT_LOST = -142,
  /** No operation performed */
  ERR__NOOP = -141,
  /** No offset to automatically reset to */
  ERR__AUTO_OFFSET_RESET = -140,

  /** End internal error codes */
  ERR__END = -100,

  /* Kafka broker errors: */
  /** Unknown broker error */
  ERR_UNKNOWN = -1,
  /** Success */
  ERR_NO_ERROR = 0,
  /** Offset out of range */
  ERR_OFFSET_OUT_OF_RANGE = 1,
  /** Invalid message */
  ERR_INVALID_MSG = 2,
  /** Unknown topic or partition */
  ERR_UNKNOWN_TOPIC_OR_PART = 3,
  /** Invalid message size */
  ERR_INVALID_MSG_SIZE = 4,
  /** Leader not available */
  ERR_LEADER_NOT_AVAILABLE = 5,
  /** Not leader for partition */
  ERR_NOT_LEADER_FOR_PARTITION = 6,
  /** Request timed out */
  ERR_REQUEST_TIMED_OUT = 7,
  /** Broker not available */
  ERR_BROKER_NOT_AVAILABLE = 8,
  /** Replica not available */
  ERR_REPLICA_NOT_AVAILABLE = 9,
  /** Message size too large */
  ERR_MSG_SIZE_TOO_LARGE = 10,
  /** StaleControllerEpochCode */
  ERR_STALE_CTRL_EPOCH = 11,
  /** Offset metadata string too large */
  ERR_OFFSET_METADATA_TOO_LARGE = 12,
  /** Broker disconnected before response received */
  ERR_NETWORK_EXCEPTION = 13,
  /** Coordinator load in progress */
  ERR_COORDINATOR_LOAD_IN_PROGRESS = 14,
/** Group coordinator load in progress */
#define ERR_GROUP_LOAD_IN_PROGRESS ERR_COORDINATOR_LOAD_IN_PROGRESS
  /** Coordinator not available */
  ERR_COORDINATOR_NOT_AVAILABLE = 15,
/** Group coordinator not available */
#define ERR_GROUP_COORDINATOR_NOT_AVAILABLE ERR_COORDINATOR_NOT_AVAILABLE
  /** Not coordinator */
  ERR_NOT_COORDINATOR = 16,
/** Not coordinator for group */
#define ERR_NOT_COORDINATOR_FOR_GROUP ERR_NOT_COORDINATOR
  /** Invalid topic */
  ERR_TOPIC_EXCEPTION = 17,
  /** Message batch larger than configured server segment size */
  ERR_RECORD_LIST_TOO_LARGE = 18,
  /** Not enough in-sync replicas */
  ERR_NOT_ENOUGH_REPLICAS = 19,
  /** Message(s) written to insufficient number of in-sync replicas */
  ERR_NOT_ENOUGH_REPLICAS_AFTER_APPEND = 20,
  /** Invalid required acks value */
  ERR_INVALID_REQUIRED_ACKS = 21,
  /** Specified group generation id is not valid */
  ERR_ILLEGAL_GENERATION = 22,
  /** Inconsistent group protocol */
  ERR_INCONSISTENT_GROUP_PROTOCOL = 23,
  /** Invalid group.id */
  ERR_INVALID_GROUP_ID = 24,
  /** Unknown member */
  ERR_UNKNOWN_MEMBER_ID = 25,
  /** Invalid session timeout */
  ERR_INVALID_SESSION_TIMEOUT = 26,
  /** Group rebalance in progress */
  ERR_REBALANCE_IN_PROGRESS = 27,
  /** Commit offset data size is not valid */
  ERR_INVALID_COMMIT_OFFSET_SIZE = 28,
  /** Topic authorization failed */
  ERR_TOPIC_AUTHORIZATION_FAILED = 29,
  /** Group authorization failed */
  ERR_GROUP_AUTHORIZATION_FAILED = 30,
  /** Cluster authorization failed */
  ERR_CLUSTER_AUTHORIZATION_FAILED = 31,
  /** Invalid timestamp */
  ERR_INVALID_TIMESTAMP = 32,
  /** Unsupported SASL mechanism */
  ERR_UNSUPPORTED_SASL_MECHANISM = 33,
  /** Illegal SASL state */
  ERR_ILLEGAL_SASL_STATE = 34,
  /** Unuspported version */
  ERR_UNSUPPORTED_VERSION = 35,
  /** Topic already exists */
  ERR_TOPIC_ALREADY_EXISTS = 36,
  /** Invalid number of partitions */
  ERR_INVALID_PARTITIONS = 37,
  /** Invalid replication factor */
  ERR_INVALID_REPLICATION_FACTOR = 38,
  /** Invalid replica assignment */
  ERR_INVALID_REPLICA_ASSIGNMENT = 39,
  /** Invalid config */
  ERR_INVALID_CONFIG = 40,
  /** Not controller for cluster */
  ERR_NOT_CONTROLLER = 41,
  /** Invalid request */
  ERR_INVALID_REQUEST = 42,
  /** Message format on broker does not support request */
  ERR_UNSUPPORTED_FOR_MESSAGE_FORMAT = 43,
  /** Policy violation */
  ERR_POLICY_VIOLATION = 44,
  /** Broker received an out of order sequence number */
  ERR_OUT_OF_ORDER_SEQUENCE_NUMBER = 45,
  /** Broker received a duplicate sequence number */
  ERR_DUPLICATE_SEQUENCE_NUMBER = 46,
  /** Producer attempted an operation with an old epoch */
  ERR_INVALID_PRODUCER_EPOCH = 47,
  /** Producer attempted a transactional operation in an invalid state */
  ERR_INVALID_TXN_STATE = 48,
  /** Producer attempted to use a producer id which is not
   *  currently assigned to its transactional id */
  ERR_INVALID_PRODUCER_ID_MAPPING = 49,
  /** Transaction timeout is larger than the maximum
   *  value allowed by the broker's max.transaction.timeout.ms */
  ERR_INVALID_TRANSACTION_TIMEOUT = 50,
  /** Producer attempted to update a transaction while another
   *  concurrent operation on the same transaction was ongoing */
  ERR_CONCURRENT_TRANSACTIONS = 51,
  /** Indicates that the transaction coordinator sending a
   *  WriteTxnMarker is no longer the current coordinator for a
   *  given producer */
  ERR_TRANSACTION_COORDINATOR_FENCED = 52,
  /** Transactional Id authorization failed */
  ERR_TRANSACTIONAL_ID_AUTHORIZATION_FAILED = 53,
  /** Security features are disabled */
  ERR_SECURITY_DISABLED = 54,
  /** Operation not attempted */
  ERR_OPERATION_NOT_ATTEMPTED = 55,
  /** Disk error when trying to access log file on the disk */
  ERR_KAFKA_STORAGE_ERROR = 56,
  /** The user-specified log directory is not found in the broker config */
  ERR_LOG_DIR_NOT_FOUND = 57,
  /** SASL Authentication failed */
  ERR_SASL_AUTHENTICATION_FAILED = 58,
  /** Unknown Producer Id */
  ERR_UNKNOWN_PRODUCER_ID = 59,
  /** Partition reassignment is in progress */
  ERR_REASSIGNMENT_IN_PROGRESS = 60,
  /** Delegation Token feature is not enabled */
  ERR_DELEGATION_TOKEN_AUTH_DISABLED = 61,
  /** Delegation Token is not found on server */
  ERR_DELEGATION_TOKEN_NOT_FOUND = 62,
  /** Specified Principal is not valid Owner/Renewer */
  ERR_DELEGATION_TOKEN_OWNER_MISMATCH = 63,
  /** Delegation Token requests are not allowed on this connection */
  ERR_DELEGATION_TOKEN_REQUEST_NOT_ALLOWED = 64,
  /** Delegation Token authorization failed */
  ERR_DELEGATION_TOKEN_AUTHORIZATION_FAILED = 65,
  /** Delegation Token is expired */
  ERR_DELEGATION_TOKEN_EXPIRED = 66,
  /** Supplied principalType is not supported */
  ERR_INVALID_PRINCIPAL_TYPE = 67,
  /** The group is not empty */
  ERR_NON_EMPTY_GROUP = 68,
  /** The group id does not exist */
  ERR_GROUP_ID_NOT_FOUND = 69,
  /** The fetch session ID was not found */
  ERR_FETCH_SESSION_ID_NOT_FOUND = 70,
  /** The fetch session epoch is invalid */
  ERR_INVALID_FETCH_SESSION_EPOCH = 71,
  /** No matching listener */
  ERR_LISTENER_NOT_FOUND = 72,
  /** Topic deletion is disabled */
  ERR_TOPIC_DELETION_DISABLED = 73,
  /** Leader epoch is older than broker epoch */
  ERR_FENCED_LEADER_EPOCH = 74,
  /** Leader epoch is newer than broker epoch */
  ERR_UNKNOWN_LEADER_EPOCH = 75,
  /** Unsupported compression type */
  ERR_UNSUPPORTED_COMPRESSION_TYPE = 76,
  /** Broker epoch has changed */
  ERR_STALE_BROKER_EPOCH = 77,
  /** Leader high watermark is not caught up */
  ERR_OFFSET_NOT_AVAILABLE = 78,
  /** Group member needs a valid member ID */
  ERR_MEMBER_ID_REQUIRED = 79,
  /** Preferred leader was not available */
  ERR_PREFERRED_LEADER_NOT_AVAILABLE = 80,
  /** Consumer group has reached maximum size */
  ERR_GROUP_MAX_SIZE_REACHED = 81,
  /** Static consumer fenced by other consumer with same
   * group.instance.id. */
  ERR_FENCED_INSTANCE_ID = 82,
  /** Eligible partition leaders are not available */
  ERR_ELIGIBLE_LEADERS_NOT_AVAILABLE = 83,
  /** Leader election not needed for topic partition */
  ERR_ELECTION_NOT_NEEDED = 84,
  /** No partition reassignment is in progress */
  ERR_NO_REASSIGNMENT_IN_PROGRESS = 85,
  /** Deleting offsets of a topic while the consumer group is
   *  subscribed to it */
  ERR_GROUP_SUBSCRIBED_TO_TOPIC = 86,
  /** Broker failed to validate record */
  ERR_INVALID_RECORD = 87,
  /** There are unstable offsets that need to be cleared */
  ERR_UNSTABLE_OFFSET_COMMIT = 88,
  /** Throttling quota has been exceeded */
  ERR_THROTTLING_QUOTA_EXCEEDED = 89,
  /** There is a newer producer with the same transactionalId
   *  which fences the current one */
  ERR_PRODUCER_FENCED = 90,
  /** Request illegally referred to resource that does not exist */
  ERR_RESOURCE_NOT_FOUND = 91,
  /** Request illegally referred to the same resource twice */
  ERR_DUPLICATE_RESOURCE = 92,
  /** Requested credential would not meet criteria for acceptability */
  ERR_UNACCEPTABLE_CREDENTIAL = 93,
  /** Indicates that the either the sender or recipient of a
   *  voter-only request is not one of the expected voters */
  ERR_INCONSISTENT_VOTER_SET = 94,
  /** Invalid update version */
  ERR_INVALID_UPDATE_VERSION = 95,
  /** Unable to update finalized features due to server error */
  ERR_FEATURE_UPDATE_FAILED = 96,
  /** Request principal deserialization failed during forwarding */
  ERR_PRINCIPAL_DESERIALIZATION_FAILURE = 97
};


/**
 * @brief Returns a human readable representation of a kafka error.
 */
RD_EXPORT
std::string err2str(RdKafka::ErrorCode err);



/**
 * @enum CertificateType
 * @brief SSL certificate types
 */
enum CertificateType {
  CERT_PUBLIC_KEY,  /**< Client's public key */
  CERT_PRIVATE_KEY, /**< Client's private key */
  CERT_CA,          /**< CA certificate */
  CERT__CNT
};

/**
 * @enum CertificateEncoding
 * @brief SSL certificate encoding
 */
enum CertificateEncoding {
  CERT_ENC_PKCS12, /**< PKCS#12 */
  CERT_ENC_DER,    /**< DER / binary X.509 ASN1 */
  CERT_ENC_PEM,    /**< PEM */
  CERT_ENC__CNT
};

/**@} */



/**@cond NO_DOC*/
/* Forward declarations */
class Handle;
class Producer;
class Message;
class Headers;
class Queue;
class Event;
class Topic;
class TopicPartition;
class Metadata;
class KafkaConsumer;
/**@endcond*/


/**
 * @name Error class
 * @{
 *
 */

/**
 * @brief The Error class is used as a return value from APIs to propagate
 *        an error. The error consists of an error code which is to be used
 *        programatically, an error string for showing to the user,
 *        and various error flags that can be used programmatically to decide
 *        how to handle the error; e.g., should the operation be retried,
 *        was it a fatal error, etc.
 *
 * Error objects must be deleted explicitly to free its resources.
 */
class RD_EXPORT Error {
 public:
  /**
   * @brief Create error object.
   */
  static Error *create(ErrorCode code, const std::string *errstr);

  virtual ~Error() {
  }

  /*
   * Error accessor methods
   */

  /**
   * @returns the error code, e.g., RdKafka::ERR_UNKNOWN_MEMBER_ID.
   */
  virtual ErrorCode code() const = 0;

  /**
   * @returns the error code name, e.g, "ERR_UNKNOWN_MEMBER_ID".
   */
  virtual std::string name() const = 0;

  /**
   * @returns a human readable error string.
   */
  virtual std::string str() const = 0;

  /**
   * @returns true if the error is a fatal error, indicating that the client
   *          instance is no longer usable, else false.
   */
  virtual bool is_fatal() const = 0;

  /**
   * @returns true if the operation may be retried, else false.
   */
  virtual bool is_retriable() const = 0;

  /**
   * @returns true if the error is an abortable transaction error in which case
   *          the application must call RdKafka::Producer::abort_transaction()
   *          and start a new transaction with
   *          RdKafka::Producer::begin_transaction() if it wishes to proceed
   *          with transactions.
   *          Else returns false.
   *
   * @remark The return value of this method is only valid for errors returned
   *         by the transactional API.
   */
  virtual bool txn_requires_abort() const = 0;
};

/**@}*/


/**
 * @name Callback classes
 * @{
 *
 *
 * librdkafka uses (optional) callbacks to propagate information and
 * delegate decisions to the application logic.
 *
 * An application must call RdKafka::poll() at regular intervals to
 * serve queued callbacks.
 */


/**
 * @brief Delivery Report callback class
 *
 * The delivery report callback will be called once for each message
 * accepted by RdKafka::Producer::produce() (et.al) with
 * RdKafka::Message::err() set to indicate the result of the produce request.
 *
 * The callback is called when a message is succesfully produced or
 * if librdkafka encountered a permanent failure, or the retry counter for
 * temporary errors has been exhausted.
 *
 * An application must call RdKafka::poll() at regular intervals to
 * serve queued delivery report callbacks.

 */
class RD_EXPORT DeliveryReportCb {
 public:
  /**
   * @brief Delivery report callback.
   */
  virtual void dr_cb(Message &message) = 0;

  virtual ~DeliveryReportCb() {
  }
};


/**
 * @brief SASL/OAUTHBEARER token refresh callback class
 *
 * The SASL/OAUTHBEARER token refresh callback is triggered via RdKafka::poll()
 * whenever OAUTHBEARER is the SASL mechanism and a token needs to be retrieved,
 * typically based on the configuration defined in \c sasl.oauthbearer.config.
 *
 * The \c oauthbearer_config argument is the value of the
 * \c sasl.oauthbearer.config configuration property.
 *
 * The callback should invoke RdKafka::Handle::oauthbearer_set_token() or
 * RdKafka::Handle::oauthbearer_set_token_failure() to indicate success or
 * failure, respectively.
 *
 * The refresh operation is eventable and may be received when an event
 * callback handler is set with an event type of
 * \c RdKafka::Event::EVENT_OAUTHBEARER_TOKEN_REFRESH.
 *
 * Note that before any SASL/OAUTHBEARER broker connection can succeed the
 * application must call RdKafka::Handle::oauthbearer_set_token() once -- either
 * directly or, more typically, by invoking RdKafka::poll() -- in order to
 * cause retrieval of an initial token to occur.
 *
 * An application must call RdKafka::poll() at regular intervals to
 * serve queued SASL/OAUTHBEARER token refresh callbacks (when
 * OAUTHBEARER is the SASL mechanism).
 */
class RD_EXPORT OAuthBearerTokenRefreshCb {
 public:
  /**
   * @brief SASL/OAUTHBEARER token refresh callback class.
   *
   * @param handle The RdKafka::Handle which requires a refreshed token.
   * @param oauthbearer_config The value of the
   * \p sasl.oauthbearer.config configuration property for \p handle.
   */
  virtual void oauthbearer_token_refresh_cb(
      RdKafka::Handle *handle,
      const std::string &oauthbearer_config) = 0;

  virtual ~OAuthBearerTokenRefreshCb() {
  }
};


/**
 * @brief Partitioner callback class
 *
 * Generic partitioner callback class for implementing custom partitioners.
 *
 * @sa RdKafka::Conf::set() \c "partitioner_cb"
 */
class RD_EXPORT PartitionerCb {
 public:
  /**
   * @brief Partitioner callback
   *
   * Return the partition to use for \p key in \p topic.
   *
   * The \p msg_opaque is the same \p msg_opaque provided in the
   * RdKafka::Producer::produce() call.
   *
   * @remark \p key may be NULL or the empty.
   *
   * @returns Must return a value between 0 and \p partition_cnt
   * (non-inclusive). May return RD_KAFKA_PARTITION_UA (-1) if partitioning
   * failed.
   *
   * @sa The callback may use RdKafka::Topic::partition_available() to check
   *     if a partition has an active leader broker.
   */
  virtual int32_t partitioner_cb(const Topic *topic,
                                 const std::string *key,
                                 int32_t partition_cnt,
                                 void *msg_opaque) = 0;

  virtual ~PartitionerCb() {
  }
};

/**
 * @brief  Variant partitioner with key pointer
 *
 */
class PartitionerKeyPointerCb {
 public:
  /**
   * @brief Variant partitioner callback that gets \p key as pointer and length
   *        instead of as a const std::string *.
   *
   * @remark \p key may be NULL or have \p key_len 0.
   *
   * @sa See RdKafka::PartitionerCb::partitioner_cb() for exact semantics
   */
  virtual int32_t partitioner_cb(const Topic *topic,
                                 const void *key,
                                 size_t key_len,
                                 int32_t partition_cnt,
                                 void *msg_opaque) = 0;

  virtual ~PartitionerKeyPointerCb() {
  }
};



/**
 * @brief Event callback class
 *
 * Events are a generic interface for propagating errors, statistics, logs, etc
 * from librdkafka to the application.
 *
 * @sa RdKafka::Event
 */
class RD_EXPORT EventCb {
 public:
  /**
   * @brief Event callback
   *
   * @sa RdKafka::Event
   */
  virtual void event_cb(Event &event) = 0;

  virtual ~EventCb() {
  }
};


/**
 * @brief Event object class as passed to the EventCb callback.
 */
class RD_EXPORT Event {
 public:
  /** @brief Event type */
  enum Type {
    EVENT_ERROR,   /**< Event is an error condition */
    EVENT_STATS,   /**< Event is a statistics JSON document */
    EVENT_LOG,     /**< Event is a log message */
    EVENT_THROTTLE /**< Event is a throttle level signaling from the broker */
  };

  /** @brief EVENT_LOG severities (conforms to syslog(3) severities) */
  enum Severity {
    EVENT_SEVERITY_EMERG    = 0,
    EVENT_SEVERITY_ALERT    = 1,
    EVENT_SEVERITY_CRITICAL = 2,
    EVENT_SEVERITY_ERROR    = 3,
    EVENT_SEVERITY_WARNING  = 4,
    EVENT_SEVERITY_NOTICE   = 5,
    EVENT_SEVERITY_INFO     = 6,
    EVENT_SEVERITY_DEBUG    = 7
  };

  virtual ~Event() {
  }

  /*
   * Event Accessor methods
   */

  /**
   * @returns The event type
   * @remark Applies to all event types
   */
  virtual Type type() const = 0;

  /**
   * @returns Event error, if any.
   * @remark Applies to all event types except THROTTLE
   */
  virtual ErrorCode err() const = 0;

  /**
   * @returns Log severity level.
   * @remark Applies to LOG event type.
   */
  virtual Severity severity() const = 0;

  /**
   * @returns Log facility string.
   * @remark Applies to LOG event type.
   */
  virtual std::string fac() const = 0;

  /**
   * @returns Log message string.
   *
   * \c EVENT_LOG: Log message string.
   * \c EVENT_STATS: JSON object (as string).
   *
   * @remark Applies to LOG event type.
   */
  virtual std::string str() const = 0;

  /**
   * @returns Throttle time in milliseconds.
   * @remark Applies to THROTTLE event type.
   */
  virtual int throttle_time() const = 0;

  /**
   * @returns Throttling broker's name.
   * @remark Applies to THROTTLE event type.
   */
  virtual std::string broker_name() const = 0;

  /**
   * @returns Throttling broker's id.
   * @remark Applies to THROTTLE event type.
   */
  virtual int broker_id() const = 0;


  /**
   * @returns true if this is a fatal error.
   * @remark Applies to ERROR event type.
   * @sa RdKafka::Handle::fatal_error()
   */
  virtual bool fatal() const = 0;
};



/**
 * @brief Consume callback class
 */
class RD_EXPORT ConsumeCb {
 public:
  /**
   * @brief The consume callback is used with
   *        RdKafka::Consumer::consume_callback()
   *        methods and will be called for each consumed \p message.
   *
   * The callback interface is optional but provides increased performance.
   */
  virtual void consume_cb(Message &message, void *opaque) = 0;

  virtual ~ConsumeCb() {
  }
};


/**
 * @brief \b KafkaConsumer: Rebalance callback class
 */
class RD_EXPORT RebalanceCb {
 public:
  /**
   * @brief Group rebalance callback for use with RdKafka::KafkaConsumer
   *
   * Registering a \p rebalance_cb turns off librdkafka's automatic
   * partition assignment/revocation and instead delegates that responsibility
   * to the application's \p rebalance_cb.
   *
   * The rebalance callback is responsible for updating librdkafka's
   * assignment set based on the two events: RdKafka::ERR__ASSIGN_PARTITIONS
   * and RdKafka::ERR__REVOKE_PARTITIONS but should also be able to handle
   * arbitrary rebalancing failures where \p err is neither of those.
   * @remark In this latter case (arbitrary error), the application must
   *         call unassign() to synchronize state.
   *
   * For eager/non-cooperative `partition.assignment.strategy` assignors,
   * such as `range` and `roundrobin`, the application must use
   * assign assign() to set and unassign() to clear the entire assignment.
   * For the cooperative assignors, such as `cooperative-sticky`, the
   * application must use incremental_assign() for ERR__ASSIGN_PARTITIONS and
   * incremental_unassign() for ERR__REVOKE_PARTITIONS.
   *
   * Without a rebalance callback this is done automatically by librdkafka
   * but registering a rebalance callback gives the application flexibility
   * in performing other operations along with the assinging/revocation,
   * such as fetching offsets from an alternate location (on assign)
   * or manually committing offsets (on revoke).
   *
   * @sa RdKafka::KafkaConsumer::assign()
   * @sa RdKafka::KafkaConsumer::incremental_assign()
   * @sa RdKafka::KafkaConsumer::incremental_unassign()
   * @sa RdKafka::KafkaConsumer::assignment_lost()
   * @sa RdKafka::KafkaConsumer::rebalance_protocol()
   *
   * The following example show's the application's responsibilities:
   * @code
   *    class MyRebalanceCb : public RdKafka::RebalanceCb {
   *     public:
   *      void rebalance_cb (RdKafka::KafkaConsumer *consumer,
   *                    RdKafka::ErrorCode err,
   *                    std::vector<RdKafka::TopicPartition*> &partitions) {
   *         if (err == RdKafka::ERR__ASSIGN_PARTITIONS) {
   *           // application may load offets from arbitrary external
   *           // storage here and update \p partitions
   *           if (consumer->rebalance_protocol() == "COOPERATIVE")
   *             consumer->incremental_assign(partitions);
   *           else
   *             consumer->assign(partitions);
   *
   *         } else if (err == RdKafka::ERR__REVOKE_PARTITIONS) {
   *           // Application may commit offsets manually here
   *           // if auto.commit.enable=false
   *           if (consumer->rebalance_protocol() == "COOPERATIVE")
   *             consumer->incremental_unassign(partitions);
   *           else
   *             consumer->unassign();
   *
   *         } else {
   *           std::cerr << "Rebalancing error: " <<
   *                        RdKafka::err2str(err) << std::endl;
   *           consumer->unassign();
   *         }
   *     }
   *  }
   * @endcode
   *
   * @remark The above example lacks error handling for assign calls, see
   *         the examples/ directory.
   */
  virtual void rebalance_cb(RdKafka::KafkaConsumer *consumer,
                            RdKafka::ErrorCode err,
                            std::vector<TopicPartition *> &partitions) = 0;

  virtual ~RebalanceCb() {
  }
};


/**
 * @brief Offset Commit callback class
 */
class RD_EXPORT OffsetCommitCb {
 public:
  /**
   * @brief Set offset commit callback for use with consumer groups
   *
   * The results of automatic or manual offset commits will be scheduled
   * for this callback and is served by RdKafka::KafkaConsumer::consume().
   *
   * If no partitions had valid offsets to commit this callback will be called
   * with \p err == ERR__NO_OFFSET which is not to be considered an error.
   *
   * The \p offsets list contains per-partition information:
   *   - \c topic      The topic committed
   *   - \c partition  The partition committed
   *   - \c offset:    Committed offset (attempted)
   *   - \c err:       Commit error
   */
  virtual void offset_commit_cb(RdKafka::ErrorCode err,
                                std::vector<TopicPartition *> &offsets) = 0;

  virtual ~OffsetCommitCb() {
  }
};



/**
 * @brief SSL broker certificate verification class.
 *
 * @remark Class instance must outlive the RdKafka client instance.
 */
class RD_EXPORT SslCertificateVerifyCb {
 public:
  /**
   * @brief SSL broker certificate verification callback.
   *
   * The verification callback is triggered from internal librdkafka threads
   * upon connecting to a broker. On each connection attempt the callback
   * will be called for each certificate in the broker's certificate chain,
   * starting at the root certification, as long as the application callback
   * returns 1 (valid certificate).
   *
   * \p broker_name and \p broker_id correspond to the broker the connection
   * is being made to.
   * The \c x509_error argument indicates if OpenSSL's verification of
   * the certificate succeed (0) or failed (an OpenSSL error code).
   * The application may set the SSL context error code by returning 0
   * from the verify callback and providing a non-zero SSL context error code
   * in \p x509_error.
   * If the verify callback sets \p x509_error to 0, returns 1, and the
   * original \p x509_error was non-zero, the error on the SSL context will
   * be cleared.
   * \p x509_error is always a valid pointer to an int.
   *
   * \p depth is the depth of the current certificate in the chain, starting
   * at the root certificate.
   *
   * The certificate itself is passed in binary DER format in \p buf of
   * size \p size.
   *
   * The callback must 1 if verification succeeds, or 0 if verification fails
   * and write a human-readable error message
   * to \p errstr.
   *
   * @warning This callback will be called from internal librdkafka threads.
   *
   * @remark See <openssl/x509_vfy.h> in the OpenSSL source distribution
   *         for a list of \p x509_error codes.
   */
  virtual bool ssl_cert_verify_cb(const std::string &broker_name,
                                  int32_t broker_id,
                                  int *x509_error,
                                  int depth,
                                  const char *buf,
                                  size_t size,
                                  std::string &errstr) = 0;

  virtual ~SslCertificateVerifyCb() {
  }
};


/**
 * @brief \b Portability: SocketCb callback class
 *
 */
class RD_EXPORT SocketCb {
 public:
  /**
   * @brief Socket callback
   *
   * The socket callback is responsible for opening a socket
   * according to the supplied \p domain, \p type and \p protocol.
   * The socket shall be created with \c CLOEXEC set in a racefree fashion, if
   * possible.
   *
   * It is typically not required to register an alternative socket
   * implementation
   *
   * @returns The socket file descriptor or -1 on error (\c errno must be set)
   */
  virtual int socket_cb(int domain, int type, int protocol) = 0;

  virtual ~SocketCb() {
  }
};


/**
 * @brief \b Portability: OpenCb callback class
 *
 */
class RD_EXPORT OpenCb {
 public:
  /**
   * @brief Open callback
   * The open callback is responsible for opening the file specified by
   * \p pathname, using \p flags and \p mode.
   * The file shall be opened with \c CLOEXEC set in a racefree fashion, if
   * possible.
   *
   * It is typically not required to register an alternative open implementation
   *
   * @remark Not currently available on native Win32
   */
  virtual int open_cb(const std::string &path, int flags, int mode) = 0;

  virtual ~OpenCb() {
  }
};


/**@}*/



/**
 * @name Configuration interface
 * @{
 *
 */

/**
 * @brief Configuration interface
 *
 * Holds either global or topic configuration that are passed to
 * RdKafka::Consumer::create(), RdKafka::Producer::create(),
 * RdKafka::KafkaConsumer::create(), etc.
 *
 * @sa CONFIGURATION.md for the full list of supported properties.
 */
class RD_EXPORT Conf {
 public:
  /**
   * @brief Configuration object type
   */
  enum ConfType {
    CONF_GLOBAL, /**< Global configuration */
    CONF_TOPIC   /**< Topic specific configuration */
  };

  /**
   * @brief RdKafka::Conf::Set() result code
   */
  enum ConfResult {
    CONF_UNKNOWN = -2, /**< Unknown configuration property */
    CONF_INVALID = -1, /**< Invalid configuration value */
    CONF_OK      = 0   /**< Configuration property was succesfully set */
  };


  /**
   * @brief Create configuration object
   */
  static Conf *create(ConfType type);

  virtual ~Conf() {
  }

  /**
   * @brief Set configuration property \p name to value \p value.
   *
   * Fallthrough:
   * Topic-level configuration properties may be set using this interface
   * in which case they are applied on the \c default_topic_conf.
   * If no \c default_topic_conf has been set one will be created.
   * Any sub-sequent set("default_topic_conf", ..) calls will
   * replace the current default topic configuration.

   * @returns CONF_OK on success, else writes a human readable error
   *          description to \p errstr on error.
   */
  virtual Conf::ConfResult set(const std::string &name,
                               const std::string &value,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"dr_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               DeliveryReportCb *dr_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"oauthbearer_token_refresh_cb\" */
  virtual Conf::ConfResult set(
      const std::string &name,
      OAuthBearerTokenRefreshCb *oauthbearer_token_refresh_cb,
      std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"event_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               EventCb *event_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"default_topic_conf\"
   *
   * Sets the default topic configuration to use for for automatically
   * subscribed topics.
   *
   * @sa RdKafka::KafkaConsumer::subscribe()
   */
  virtual Conf::ConfResult set(const std::string &name,
                               const Conf *topic_conf,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"partitioner_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               PartitionerCb *partitioner_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"partitioner_key_pointer_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               PartitionerKeyPointerCb *partitioner_kp_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"socket_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               SocketCb *socket_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"open_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               OpenCb *open_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"rebalance_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               RebalanceCb *rebalance_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"offset_commit_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               OffsetCommitCb *offset_commit_cb,
                               std::string &errstr) = 0;

  /** @brief Use with \p name = \c \"ssl_cert_verify_cb\".
   *  @returns CONF_OK on success or CONF_INVALID if SSL is
   *           not supported in this build.
   */
  virtual Conf::ConfResult set(const std::string &name,
                               SslCertificateVerifyCb *ssl_cert_verify_cb,
                               std::string &errstr) = 0;

  /**
   * @brief Set certificate/key \p cert_type from the \p cert_enc encoded
   *        memory at \p buffer of \p size bytes.
   *
   * @param cert_type Certificate or key type to configure.
   * @param cert_enc  Buffer \p encoding type.
   * @param buffer Memory pointer to encoded certificate or key.
   *               The memory is not referenced after this function returns.
   * @param size Size of memory at \p buffer.
   * @param errstr A human-readable error string will be written to this string
   *               on failure.
   *
   * @returns CONF_OK on success or CONF_INVALID if the memory in
   *          \p buffer is of incorrect encoding, or if librdkafka
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
  virtual Conf::ConfResult set_ssl_cert(RdKafka::CertificateType cert_type,
                                        RdKafka::CertificateEncoding cert_enc,
                                        const void *buffer,
                                        size_t size,
                                        std::string &errstr) = 0;

  /** @brief Query single configuration value
   *
   * Do not use this method to get callbacks registered by the configuration
   * file. Instead use the specific get() methods with the specific callback
   * parameter in the signature.
   *
   * Fallthrough:
   * Topic-level configuration properties from the \c default_topic_conf
   * may be retrieved using this interface.
   *
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p value. */
  virtual Conf::ConfResult get(const std::string &name,
                               std::string &value) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p dr_cb. */
  virtual Conf::ConfResult get(DeliveryReportCb *&dr_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p oauthbearer_token_refresh_cb. */
  virtual Conf::ConfResult get(
      OAuthBearerTokenRefreshCb *&oauthbearer_token_refresh_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p event_cb. */
  virtual Conf::ConfResult get(EventCb *&event_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p partitioner_cb. */
  virtual Conf::ConfResult get(PartitionerCb *&partitioner_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p partitioner_kp_cb. */
  virtual Conf::ConfResult get(
      PartitionerKeyPointerCb *&partitioner_kp_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p socket_cb. */
  virtual Conf::ConfResult get(SocketCb *&socket_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p open_cb. */
  virtual Conf::ConfResult get(OpenCb *&open_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p rebalance_cb. */
  virtual Conf::ConfResult get(RebalanceCb *&rebalance_cb) const = 0;

  /** @brief Query single configuration value
   *  @returns CONF_OK if the property was set previously set and
   *           returns the value in \p offset_commit_cb. */
  virtual Conf::ConfResult get(OffsetCommitCb *&offset_commit_cb) const = 0;

  /** @brief Use with \p name = \c \"ssl_cert_verify_cb\" */
  virtual Conf::ConfResult get(
      SslCertificateVerifyCb *&ssl_cert_verify_cb) const = 0;

  /** @brief Dump configuration names and values to list containing
   *         name,value tuples */
  virtual std::list<std::string> *dump() = 0;

  /** @brief Use with \p name = \c \"consume_cb\" */
  virtual Conf::ConfResult set(const std::string &name,
                               ConsumeCb *consume_cb,
                               std::string &errstr) = 0;

  /**
   * @brief Returns the underlying librdkafka C rd_kafka_conf_t handle.
   *
   * @warning Calling the C API on this handle is not recommended and there
   *          is no official support for it, but for cases where the C++
   *          does not provide the proper functionality this C handle can be
   *          used to interact directly with the core librdkafka API.
   *
   * @remark The lifetime of the returned pointer is the same as the Conf
   *         object this method is called on.
   *
   * @remark Include <rdkafka/rdkafka.h> prior to including
   *         <rdkafka/rdkafkacpp.h>
   *
   * @returns \c rd_kafka_conf_t* if this is a CONF_GLOBAL object, else NULL.
   */
  virtual struct rd_kafka_conf_s *c_ptr_global() = 0;

  /**
   * @brief Returns the underlying librdkafka C rd_kafka_topic_conf_t handle.
   *
   * @warning Calling the C API on this handle is not recommended and there
   *          is no official support for it, but for cases where the C++
   *          does not provide the proper functionality this C handle can be
   *          used to interact directly with the core librdkafka API.
   *
   * @remark The lifetime of the returned pointer is the same as the Conf
   *         object this method is called on.
   *
   * @remark Include <rdkafka/rdkafka.h> prior to including
   *         <rdkafka/rdkafkacpp.h>
   *
   * @returns \c rd_kafka_topic_conf_t* if this is a CONF_TOPIC object,
   *          else NULL.
   */
  virtual struct rd_kafka_topic_conf_s *c_ptr_topic() = 0;

  /**
   * @brief Set callback_data for ssl engine.
   *
   * @remark The \c ssl.engine.location configuration must be set for this
   *         to have affect.
   *
   * @remark The memory pointed to by \p value must remain valid for the
   *         lifetime of the configuration object and any Kafka clients that
   *         use it.
   *
   * @returns CONF_OK on success, else CONF_INVALID.
   */
  virtual Conf::ConfResult set_engine_callback_data(void *value,
                                                    std::string &errstr) = 0;


  /** @brief Enable/disable creation of a queue specific to SASL events
   *        and callbacks.
   *
   * For SASL mechanisms that trigger callbacks (currently OAUTHBEARER) this
   * configuration API allows an application to get a dedicated
   * queue for the SASL events/callbacks. After enabling the queue with this API
   * the application can retrieve the queue by calling
   * RdKafka::Handle::get_sasl_queue() on the client instance.
   * This queue may then be served directly by the application
   * (RdKafka::Queue::poll()) or forwarded to another queue, such as
   * the background queue.
   *
   * A convenience function is available to automatically forward the SASL queue
   * to librdkafka's background thread, see
   * RdKafka::Handle::sasl_background_callbacks_enable().
   *
   * By default (\p enable = false) the main queue (as served by
   * RdKafka::Handle::poll(), et.al.) is used for SASL callbacks.
   *
   * @remark The SASL queue is currently only used by the SASL OAUTHBEARER "
   *         mechanism's token refresh callback.
   */
  virtual Conf::ConfResult enable_sasl_queue(bool enable,
                                             std::string &errstr) = 0;
};

/**@}*/


/**
 * @name Kafka base client handle
 * @{
 *
 */

/**
 * @brief Base handle, super class for specific clients.
 */
class RD_EXPORT Handle {
 public:
  virtual ~Handle() {
  }

  /** @returns the name of the handle */
  virtual std::string name() const = 0;

  /**
   * @brief Returns the client's broker-assigned group member id
   *
   * @remark This currently requires the high-level KafkaConsumer
   *
   * @returns Last assigned member id, or empty string if not currently
   *          a group member.
   */
  virtual std::string memberid() const = 0;


  /**
   * @brief Polls the provided kafka handle for events.
   *
   * Events will trigger application provided callbacks to be called.
   *
   * The \p timeout_ms argument specifies the maximum amount of time
   * (in milliseconds) that the call will block waiting for events.
   * For non-blocking calls, provide 0 as \p timeout_ms.
   * To wait indefinately for events, provide -1.
   *
   * Events:
   *   - delivery report callbacks (if an RdKafka::DeliveryCb is configured)
   * [producer]
   *   - event callbacks (if an RdKafka::EventCb is configured) [producer &
   * consumer]
   *
   * @remark  An application should make sure to call poll() at regular
   *          intervals to serve any queued callbacks waiting to be called.
   *
   * @warning This method MUST NOT be used with the RdKafka::KafkaConsumer,
   *          use its RdKafka::KafkaConsumer::consume() instead.
   *
   * @returns the number of events served.
   */
  virtual int poll(int timeout_ms) = 0;

  /**
   * @brief  Returns the current out queue length
   *
   * The out queue contains messages and requests waiting to be sent to,
   * or acknowledged by, the broker.
   */
  virtual int outq_len() = 0;

  /**
   * @brief Request Metadata from broker.
   *
   * Parameters:
   *  \p all_topics  - if non-zero: request info about all topics in cluster,
   *                   if zero: only request info about locally known topics.
   *  \p only_rkt    - only request info about this topic
   *  \p metadatap   - pointer to hold metadata result.
   *                   The \p *metadatap pointer must be released with \c
   * delete. \p timeout_ms  - maximum response time before failing.
   *
   * @returns RdKafka::ERR_NO_ERROR on success (in which case \p *metadatap
   * will be set), else RdKafka::ERR__TIMED_OUT on timeout or
   * other error code on error.
   */
  virtual ErrorCode metadata(bool all_topics,
                             const Topic *only_rkt,
                             Metadata **metadatap,
                             int timeout_ms) = 0;


  /**
   * @brief Pause producing or consumption for the provided list of partitions.
   *
   * Success or error is returned per-partition in the \p partitions list.
   *
   * @returns ErrorCode::NO_ERROR
   *
   * @sa resume()
   */
  virtual ErrorCode pause(std::vector<TopicPartition *> &partitions) = 0;


  /**
   * @brief Resume producing or consumption for the provided list of partitions.
   *
   * Success or error is returned per-partition in the \p partitions list.
   *
   * @returns ErrorCode::NO_ERROR
   *
   * @sa pause()
   */
  virtual ErrorCode resume(std::vector<TopicPartition *> &partitions) = 0;


  /**
   * @brief Query broker for low (oldest/beginning)
   *        and high (newest/end) offsets for partition.
   *
   * Offsets are returned in \p *low and \p *high respectively.
   *
   * @returns RdKafka::ERR_NO_ERROR on success or an error code on failure.
   */
  virtual ErrorCode query_watermark_offsets(const std::string &topic,
                                            int32_t partition,
                                            int64_t *low,
                                            int64_t *high,
                                            int timeout_ms) = 0;

  /**
   * @brief Get last known low (oldest/beginning)
   *        and high (newest/end) offsets for partition.
   *
   * The low offset is updated periodically (if statistics.interval.ms is set)
   * while the high offset is updated on each fetched message set from the
   * broker.
   *
   * If there is no cached offset (either low or high, or both) then
   * OFFSET_INVALID will be returned for the respective offset.
   *
   * Offsets are returned in \p *low and \p *high respectively.
   *
   * @returns RdKafka::ERR_NO_ERROR on success or an error code on failure.
   *
   * @remark Shall only be used with an active consumer instance.
   */
  virtual ErrorCode get_watermark_offsets(const std::string &topic,
                                          int32_t partition,
                                          int64_t *low,
                                          int64_t *high) = 0;


  /**
   * @brief Look up the offsets for the given partitions by timestamp.
   *
   * The returned offset for each partition is the earliest offset whose
   * timestamp is greater than or equal to the given timestamp in the
   * corresponding partition.
   *
   * The timestamps to query are represented as \c offset in \p offsets
   * on input, and \c offset() will return the closest earlier offset
   * for the timestamp on output.
   *
   * Timestamps are expressed as milliseconds since epoch (UTC).
   *
   * The function will block for at most \p timeout_ms milliseconds.
   *
   * @remark Duplicate Topic+Partitions are not supported.
   * @remark Errors are also returned per TopicPartition, see \c err()
   *
   * @returns an error code for general errors, else RdKafka::ERR_NO_ERROR
   *          in which case per-partition errors might be set.
   */
  virtual ErrorCode offsetsForTimes(std::vector<TopicPartition *> &offsets,
                                    int timeout_ms) = 0;


  /**
   * @brief Retrieve queue for a given partition.
   *
   * @returns The fetch queue for the given partition if successful. Else,
   *          NULL is returned.
   *
   * @remark This function only works on consumers.
   */
  virtual Queue *get_partition_queue(const TopicPartition *partition) = 0;

  /**
   * @brief Forward librdkafka logs (and debug) to the specified queue
   *        for serving with one of the ..poll() calls.
   *
   *        This allows an application to serve log callbacks (\c log_cb)
   *        in its thread of choice.
   *
   * @param queue Queue to forward logs to. If the value is NULL the logs
   *        are forwarded to the main queue.
   *
   * @remark The configuration property \c log.queue MUST also be set to true.
   *
   * @remark librdkafka maintains its own reference to the provided queue.
   *
   * @returns ERR_NO_ERROR on success or an error code on error.
   */
  virtual ErrorCode set_log_queue(Queue *queue) = 0;

  /**
   * @brief Cancels the current callback dispatcher (Handle::poll(),
   *        KafkaConsumer::consume(), etc).
   *
   * A callback may use this to force an immediate return to the calling
   * code (caller of e.g. Handle::poll()) without processing any further
   * events.
   *
   * @remark This function MUST ONLY be called from within a
   *         librdkafka callback.
   */
  virtual void yield() = 0;

  /**
   * @brief Returns the ClusterId as reported in broker metadata.
   *
   * @param timeout_ms If there is no cached value from metadata retrieval
   *                   then this specifies the maximum amount of time
   *                   (in milliseconds) the call will block waiting
   *                   for metadata to be retrieved.
   *                   Use 0 for non-blocking calls.
   *
   * @remark Requires broker version >=0.10.0 and api.version.request=true.
   *
   * @returns Last cached ClusterId, or empty string if no ClusterId could be
   *          retrieved in the allotted timespan.
   */
  virtual std::string clusterid(int timeout_ms) = 0;

  /**
   * @brief Returns the underlying librdkafka C rd_kafka_t handle.
   *
   * @warning Calling the C API on this handle is not recommended and there
   *          is no official support for it, but for cases where the C++
   *          does not provide the proper functionality this C handle can be
   *          used to interact directly with the core librdkafka API.
   *
   * @remark The lifetime of the returned pointer is the same as the Topic
   *         object this method is called on.
   *
   * @remark Include <rdkafka/rdkafka.h> prior to including
   *         <rdkafka/rdkafkacpp.h>
   *
   * @returns \c rd_kafka_t*
   */
  virtual struct rd_kafka_s *c_ptr() = 0;

  /**
   * @brief Returns the current ControllerId (controller broker id)
   *        as reported in broker metadata.
   *
   * @param timeout_ms If there is no cached value from metadata retrieval
   *                   then this specifies the maximum amount of time
   *                   (in milliseconds) the call will block waiting
   *                   for metadata to be retrieved.
   *                   Use 0 for non-blocking calls.
   *
   * @remark Requires broker version >=0.10.0 and api.version.request=true.
   *
   * @returns Last cached ControllerId, or -1 if no ControllerId could be
   *          retrieved in the allotted timespan.
   */
  virtual int32_t controllerid(int timeout_ms) = 0;


  /**
   * @brief Returns the first fatal error set on this client instance,
   *        or ERR_NO_ERROR if no fatal error has occurred.
   *
   * This function is to be used with the Idempotent Producer and
   * the Event class for \c EVENT_ERROR events to detect fatal errors.
   *
   * Generally all errors raised by the error event are to be considered
   * informational and temporary, the client will try to recover from all
   * errors in a graceful fashion (by retrying, etc).
   *
   * However, some errors should logically be considered fatal to retain
   * consistency; in particular a set of errors that may occur when using the
   * Idempotent Producer and the in-order or exactly-once producer guarantees
   * can't be satisfied.
   *
   * @param errstr A human readable error string if a fatal error was set.
   *
   * @returns ERR_NO_ERROR if no fatal error has been raised, else
   *          any other error code.
   */
  virtual ErrorCode fatal_error(std::string &errstr) const = 0;

  /**
   * @brief Set SASL/OAUTHBEARER token and metadata
   *
   * @param token_value the mandatory token value to set, often (but not
   *  necessarily) a JWS compact serialization as per
   *  https://tools.ietf.org/html/rfc7515#section-3.1.
   * @param md_lifetime_ms when the token expires, in terms of the number of
   *  milliseconds since the epoch.
   * @param md_principal_name the Kafka principal name associated with the
   *  token.
   * @param extensions potentially empty SASL extension keys and values where
   *  element [i] is the key and [i+1] is the key's value, to be communicated
   *  to the broker as additional key-value pairs during the initial client
   *  response as per https://tools.ietf.org/html/rfc7628#section-3.1.  The
   *  number of SASL extension keys plus values must be a non-negative multiple
   *  of 2. Any provided keys and values are copied.
   * @param errstr A human readable error string is written here, only if
   *  there is an error.
   *
   * The SASL/OAUTHBEARER token refresh callback should invoke
   * this method upon success. The extension keys must not include the reserved
   * key "`auth`", and all extension keys and values must conform to the
   * required format as per https://tools.ietf.org/html/rfc7628#section-3.1:
   *
   *     key            = 1*(ALPHA)
   *     value          = *(VCHAR / SP / HTAB / CR / LF )
   *
   * @returns \c RdKafka::ERR_NO_ERROR on success, otherwise \p errstr set
   *              and:<br>
   *          \c RdKafka::ERR__INVALID_ARG if any of the arguments are
   *              invalid;<br>
   *          \c RdKafka::ERR__NOT_IMPLEMENTED if SASL/OAUTHBEARER is not
   *              supported by this build;<br>
   *          \c RdKafka::ERR__STATE if SASL/OAUTHBEARER is supported but is
   *              not configured as the client's authentication mechanism.<br>
   *
   * @sa RdKafka::oauthbearer_set_token_failure
   * @sa RdKafka::Conf::set() \c "oauthbearer_token_refresh_cb"
   */
  virtual ErrorCode oauthbearer_set_token(
      const std::string &token_value,
      int64_t md_lifetime_ms,
      const std::string &md_principal_name,
      const std::list<std::string> &extensions,
      std::string &errstr) = 0;

  /**
   * @brief SASL/OAUTHBEARER token refresh failure indicator.
   *
   * @param errstr human readable error reason for failing to acquire a token.
   *
   * The SASL/OAUTHBEARER token refresh callback should
   * invoke this method upon failure to refresh the token.
   *
   * @returns \c RdKafka::ERR_NO_ERROR on success, otherwise:<br>
   *          \c RdKafka::ERR__NOT_IMPLEMENTED if SASL/OAUTHBEARER is not
   *              supported by this build;<br>
   *          \c RdKafka::ERR__STATE if SASL/OAUTHBEARER is supported but is
   *              not configured as the client's authentication mechanism.
   *
   * @sa RdKafka::oauthbearer_set_token
   * @sa RdKafka::Conf::set() \c "oauthbearer_token_refresh_cb"
   */
  virtual ErrorCode oauthbearer_set_token_failure(
      const std::string &errstr) = 0;

  /**
   * @brief Enable SASL OAUTHBEARER refresh callbacks on the librdkafka
   *        background thread.
   *
   * This serves as an alternative for applications that do not
   * call RdKafka::Handle::poll() (et.al.) at regular intervals.
   */
  virtual Error *sasl_background_callbacks_enable() = 0;


  /**
   * @returns the SASL callback queue, if enabled, else NULL.
   *
   * @sa RdKafka::Conf::enable_sasl_queue()
   */
  virtual Queue *get_sasl_queue() = 0;

  /**
   * @returns the librdkafka background thread queue.
   */
  virtual Queue *get_background_queue() = 0;



  /**
   * @brief Allocate memory using the same allocator librdkafka uses.
   *
   * This is typically an abstraction for the malloc(3) call and makes sure
   * the application can use the same memory allocator as librdkafka for
   * allocating pointers that are used by librdkafka.
   *
   * @remark Memory allocated by mem_malloc() must be freed using
   *         mem_free().
   */
  virtual void *mem_malloc(size_t size) = 0;

  /**
   * @brief Free pointer returned by librdkafka
   *
   * This is typically an abstraction for the free(3) call and makes sure
   * the application can use the same memory allocator as librdkafka for
   * freeing pointers returned by librdkafka.
   *
   * In standard setups it is usually not necessary to use this interface
   * rather than the free(3) function.
   *
   * @remark mem_free() must only be used for pointers returned by APIs
   *         that explicitly mention using this function for freeing.
   */
  virtual void mem_free(void *ptr) = 0;

  /**
   * @brief Sets SASL credentials used for SASL PLAIN and SCRAM mechanisms by
   *        this Kafka client.
   *
   * This function sets or resets the SASL username and password credentials
   * used by this Kafka client. The new credentials will be used the next time
   * this client needs to authenticate to a broker.
   * will not disconnect existing connections that might have been made using
   * the old credentials.
   *
   * @remark This function only applies to the SASL PLAIN and SCRAM mechanisms.
   *
   * @returns NULL on success or an error object on error.
   */
  virtual Error *sasl_set_credentials(const std::string &username,
                                      const std::string &password) = 0;
};


/**@}*/


/**
 * @name Topic and partition objects
 * @{
 *
 */

/**
 * @brief Topic+Partition
 *
 * This is a generic type to hold a single partition and various
 * information about it.
 *
 * Is typically used with std::vector<RdKafka::TopicPartition*> to provide
 * a list of partitions for different operations.
 */
class RD_EXPORT TopicPartition {
 public:
  /**
   * @brief Create topic+partition object for \p topic and \p partition.
   *
   * Use \c delete to deconstruct.
   */
  static TopicPartition *create(const std::string &topic, int partition);

  /**
   * @brief Create topic+partition object for \p topic and \p partition
   *        with offset \p offset.
   *
   * Use \c delete to deconstruct.
   */
  static TopicPartition *create(const std::string &topic,
                                int partition,
                                int64_t offset);

  virtual ~TopicPartition() = 0;

  /**
   * @brief Destroy/delete the TopicPartitions in \p partitions
   *        and clear the vector.
   */
  static void destroy(std::vector<TopicPartition *> &partitions);

  /** @returns topic name */
  virtual const std::string &topic() const = 0;

  /** @returns partition id */
  virtual int partition() const = 0;

  /** @returns offset (if applicable) */
  virtual int64_t offset() const = 0;

  /** @brief Set offset */
  virtual void set_offset(int64_t offset) = 0;

  /** @returns error code (if applicable) */
  virtual ErrorCode err() const = 0;
};



/**
 * @brief Topic handle
 *
 */
class RD_EXPORT Topic {
 public:
  /**
   * @brief Unassigned partition.
   *
   * The unassigned partition is used by the producer API for messages
   * that should be partitioned using the configured or default partitioner.
   */
  static const int32_t PARTITION_UA;

  /** @brief Special offsets */
  static const int64_t OFFSET_BEGINNING; /**< Consume from beginning */
  static const int64_t OFFSET_END;       /**< Consume from end */
  static const int64_t OFFSET_STORED;    /**< Use offset storage */
  static const int64_t OFFSET_INVALID;   /**< Invalid offset */


  /**
   * @brief Creates a new topic handle for topic named \p topic_str
   *
   * \p conf is an optional configuration for the topic  that will be used
   * instead of the default topic configuration.
   * The \p conf object is reusable after this call.
   *
   * @returns the new topic handle or NULL on error (see \p errstr).
   */
  static Topic *create(Handle *base,
                       const std::string &topic_str,
                       const Conf *conf,
                       std::string &errstr);

  virtual ~Topic() = 0;


  /** @returns the topic name */
  virtual std::string name() const = 0;

  /**
   * @returns true if \p partition is available for the topic (has leader).
   * @warning \b MUST \b ONLY be called from within a
   *          RdKafka::PartitionerCb callback.
   */
  virtual bool partition_available(int32_t partition) const = 0;

  /**
   * @brief Store offset \p offset + 1 for topic partition \p partition.
   * The offset will be committed (written) to the broker (or file) according
   * to \p auto.commit.interval.ms or next manual offset-less commit call.
   *
   * @remark \c enable.auto.offset.store must be set to \c false when using
   *         this API.
   *
   * @returns RdKafka::ERR_NO_ERROR on success or an error code if none of the
   *          offsets could be stored.
   */
  virtual ErrorCode offset_store(int32_t partition, int64_t offset) = 0;

  /**
   * @brief Returns the underlying librdkafka C rd_kafka_topic_t handle.
   *
   * @warning Calling the C API on this handle is not recommended and there
   *          is no official support for it, but for cases where the C++ API
   *          does not provide the underlying functionality this C handle can be
   *          used to interact directly with the core librdkafka API.
   *
   * @remark The lifetime of the returned pointer is the same as the Topic
   *         object this method is called on.
   *
   * @remark Include <rdkafka/rdkafka.h> prior to including
   *         <rdkafka/rdkafkacpp.h>
   *
   * @returns \c rd_kafka_topic_t*
   */
  virtual struct rd_kafka_topic_s *c_ptr() = 0;
};


/**@}*/


/**
 * @name Message object
 * @{
 *
 */


/**
 * @brief Message timestamp object
 *
 * Represents the number of milliseconds since the epoch (UTC).
 *
 * The MessageTimestampType dictates the timestamp type or origin.
 *
 * @remark Requires Apache Kafka broker version >= 0.10.0
 *
 */

class RD_EXPORT MessageTimestamp {
 public:
  /*! Message timestamp type */
  enum MessageTimestampType {
    MSG_TIMESTAMP_NOT_AVAILABLE,  /**< Timestamp not available */
    MSG_TIMESTAMP_CREATE_TIME,    /**< Message creation time (source) */
    MSG_TIMESTAMP_LOG_APPEND_TIME /**< Message log append time (broker) */
  };

  MessageTimestampType type; /**< Timestamp type */
  int64_t timestamp;         /**< Milliseconds since epoch (UTC). */
};


/**
 * @brief Headers object
 *
 * Represents message headers.
 *
 * https://cwiki.apache.org/confluence/display/KAFKA/KIP-82+-+Add+Record+Headers
 *
 * @remark Requires Apache Kafka >= 0.11.0 brokers
 */
class RD_EXPORT Headers {
 public:
  virtual ~Headers() = 0;

  /**
   * @brief Header object
   *
   * This object represents a single Header with a key value pair
   * and an ErrorCode
   *
   * @remark dynamic allocation of this object is not supported.
   */
  class Header {
   public:
    /**
     * @brief Header object to encapsulate a single Header
     *
     * @param key the string value for the header key
     * @param value the bytes of the header value, or NULL
     * @param value_size the length in bytes of the header value
     *
     * @remark key and value are copied.
     *
     */
    Header(const std::string &key, const void *value, size_t value_size) :
        key_(key), err_(ERR_NO_ERROR), value_size_(value_size) {
      value_ = copy_value(value, value_size);
    }

    /**
     * @brief Header object to encapsulate a single Header
     *
     * @param key the string value for the header key
     * @param value the bytes of the header value
     * @param value_size the length in bytes of the header value
     * @param err the error code if one returned
     *
     * @remark The error code is used for when the Header is constructed
     *         internally by using RdKafka::Headers::get_last which constructs
     *         a Header encapsulating the ErrorCode in the process.
     *         If err is set, the value and value_size fields will be undefined.
     */
    Header(const std::string &key,
           const void *value,
           size_t value_size,
           const RdKafka::ErrorCode err) :
        key_(key), err_(err), value_(NULL), value_size_(value_size) {
      if (err == ERR_NO_ERROR)
        value_ = copy_value(value, value_size);
    }

    /**
     * @brief Copy constructor
     *
     * @param other Header to make a copy of.
     */
    Header(const Header &other) :
        key_(other.key_), err_(other.err_), value_size_(other.value_size_) {
      value_ = copy_value(other.value_, value_size_);
    }

    /**
     * @brief Assignment operator
     *
     * @param other Header to make a copy of.
     */
    Header &operator=(const Header &other) {
      if (&other == this) {
        return *this;
      }

      key_        = other.key_;
      err_        = other.err_;
      value_size_ = other.value_size_;

      if (value_ != NULL)
        mem_free(value_);

      value_ = copy_value(other.value_, value_size_);

      return *this;
    }

    ~Header() {
      if (value_ != NULL)
        mem_free(value_);
    }

    /** @returns the key/name associated with this Header */
    std::string key() const {
      return key_;
    }

    /** @returns returns the binary value, or NULL */
    const void *value() const {
      return value_;
    }

    /** @returns returns the value casted to a nul-terminated C string,
     *           or NULL. */
    const char *value_string() const {
      return static_cast<const char *>(value_);
    }

    /** @returns Value Size the length of the Value in bytes */
    size_t value_size() const {
      return value_size_;
    }

    /** @returns the error code of this Header (usually ERR_NO_ERROR) */
    RdKafka::ErrorCode err() const {
      return err_;
    }

   private:
    char *copy_value(const void *value, size_t value_size) {
      if (!value)
        return NULL;

      char *dest = (char *)mem_malloc(value_size + 1);
      memcpy(dest, (const char *)value, value_size);
      dest[value_size] = '\0';

      return dest;
    }

    std::string key_;
    RdKafka::ErrorCode err_;
    char *value_;
    size_t value_size_;
    void *operator new(size_t); /* Prevent dynamic allocation */
  };

  /**
   * @brief Create a new instance of the Headers object
   *
   * @returns an empty Headers list
   */
  static Headers *create();

  /**
   * @brief Create a new instance of the Headers object from a std::vector
   *
   * @param headers std::vector of RdKafka::Headers::Header objects.
   *                The headers are copied, not referenced.
   *
   * @returns a Headers list from std::vector set to the size of the std::vector
   */
  static Headers *create(const std::vector<Header> &headers);

  /**
   * @brief Adds a Header to the end of the list.
   *
   * @param key header key/name
   * @param value binary value, or NULL
   * @param value_size size of the value
   *
   * @returns an ErrorCode signalling success or failure to add the header.
   */
  virtual ErrorCode add(const std::string &key,
                        const void *value,
                        size_t value_size) = 0;

  /**
   * @brief Adds a Header to the end of the list.
   *
   * Convenience method for adding a std::string as a value for the header.
   *
   * @param key header key/name
   * @param value value string
   *
   * @returns an ErrorCode signalling success or failure to add the header.
   */
  virtual ErrorCode add(const std::string &key, const std::string &value) = 0;

  /**
   * @brief Adds a Header to the end of the list.
   *
   * This method makes a copy of the passed header.
   *
   * @param header Existing header to copy
   *
   * @returns an ErrorCode signalling success or failure to add the header.
   */
  virtual ErrorCode add(const Header &header) = 0;

  /**
   * @brief Removes all the Headers of a given key
   *
   * @param key header key/name to remove
   *
   * @returns An ErrorCode signalling a success or failure to remove the Header.
   */
  virtual ErrorCode remove(const std::string &key) = 0;

  /**
   * @brief Gets all of the Headers of a given key
   *
   * @param key header key/name
   *
   * @remark If duplicate keys exist this will return them all as a std::vector
   *
   * @returns a std::vector containing all the Headers of the given key.
   */
  virtual std::vector<Header> get(const std::string &key) const = 0;

  /**
   * @brief Gets the last occurrence of a Header of a given key
   *
   * @param key header key/name
   *
   * @remark This will only return the most recently added header
   *
   * @returns the Header if found, otherwise a Header with an err set to
   *          ERR__NOENT.
   */
  virtual Header get_last(const std::string &key) const = 0;

  /**
   * @brief Returns all Headers
   *
   * @returns a std::vector containing all of the Headers
   */
  virtual std::vector<Header> get_all() const = 0;

  /**
   * @returns the number of headers.
   */
  virtual size_t size() const = 0;
};


/**
 * @brief Message object
 *
 * This object represents either a single consumed or produced message,
 * or an event (\p err() is set).
 *
 * An application must check RdKafka::Message::err() to see if the
 * object is a proper message (error is RdKafka::ERR_NO_ERROR) or a
 * an error event.
 *
 */
class RD_EXPORT Message {
 public:
  /** @brief Message persistence status can be used by the application to
   *         find out if a produced message was persisted in the topic log. */
  enum Status {
    /** Message was never transmitted to the broker, or failed with
     *  an error indicating it was not written to the log.
     *  Application retry risks ordering, but not duplication. */
    MSG_STATUS_NOT_PERSISTED = 0,

    /** Message was transmitted to broker, but no acknowledgement was
     *  received.
     *  Application retry risks ordering and duplication. */
    MSG_STATUS_POSSIBLY_PERSISTED = 1,

    /** Message was written to the log and fully acknowledged.
     *  No reason for application to retry.
     *  Note: this value should only be trusted with \c acks=all. */
    MSG_STATUS_PERSISTED = 2,
  };

  /**
   * @brief Accessor functions*
   * @remark Not all fields are present in all types of callbacks.
   */

  /** @returns The error string if object represent an error event,
   *           else an empty string. */
  virtual std::string errstr() const = 0;

  /** @returns The error code if object represents an error event, else 0. */
  virtual ErrorCode err() const = 0;

  /** @returns the RdKafka::Topic object for a message (if applicable),
   *            or NULL if a corresponding RdKafka::Topic object has not been
   *            explicitly created with RdKafka::Topic::create().
   *            In this case use topic_name() instead. */
  virtual Topic *topic() const = 0;

  /** @returns Topic name (if applicable, else empty string) */
  virtual std::string topic_name() const = 0;

  /** @returns Partition (if applicable) */
  virtual int32_t partition() const = 0;

  /** @returns Message payload (if applicable) */
  virtual void *payload() const = 0;

  /** @returns Message payload length (if applicable) */
  virtual size_t len() const = 0;

  /** @returns Message key as string (if applicable) */
  virtual const std::string *key() const = 0;

  /** @returns Message key as void pointer  (if applicable) */
  virtual const void *key_pointer() const = 0;

  /** @returns Message key's binary length (if applicable) */
  virtual size_t key_len() const = 0;

  /** @returns Message or error offset (if applicable) */
  virtual int64_t offset() const = 0;

  /** @returns Message timestamp (if applicable) */
  virtual MessageTimestamp timestamp() const = 0;

  /** @returns The \p msg_opaque as provided to RdKafka::Producer::produce() */
  virtual void *msg_opaque() const = 0;

  virtual ~Message() = 0;

  /** @returns the latency in microseconds for a produced message measured
   *           from the produce() call, or -1 if latency is not available. */
  virtual int64_t latency() const = 0;

  /**
   * @brief Returns the underlying librdkafka C rd_kafka_message_t handle.
   *
   * @warning Calling the C API on this handle is not recommended and there
   *          is no official support for it, but for cases where the C++ API
   *          does not provide the underlying functionality this C handle can be
   *          used to interact directly with the core librdkafka API.
   *
   * @remark The lifetime of the returned pointer is the same as the Message
   *         object this method is called on.
   *
   * @remark Include <rdkafka/rdkafka.h> prior to including
   *         <rdkafka/rdkafkacpp.h>
   *
   * @returns \c rd_kafka_message_t*
   */
  virtual struct rd_kafka_message_s *c_ptr() = 0;

  /**
   * @brief Returns the message's persistence status in the topic log.
   */
  virtual Status status() const = 0;

  /** @returns the Headers instance for this Message, or NULL if there
   *  are no headers.
   *
   * @remark The lifetime of the Headers are the same as the Message. */
  virtual RdKafka::Headers *headers() = 0;

  /** @returns the Headers instance for this Message (if applicable).
   *  If NULL is returned the reason is given in \p err, which
   *  is either ERR__NOENT if there were no headers, or another
   *  error code if header parsing failed.
   *
   * @remark The lifetime of the Headers are the same as the Message. */
  virtual RdKafka::Headers *headers(RdKafka::ErrorCode *err) = 0;

  /** @returns the broker id of the broker the message was produced to or
   *           fetched from, or -1 if not known/applicable. */
  virtual int32_t broker_id() const = 0;
};

/**@}*/


/**
 * @name Queue interface
 * @{
 *
 */


/**
 * @brief Queue interface
 *
 * Create a new message queue.  Message queues allows the application
 * to re-route consumed messages from multiple topic+partitions into
 * one single queue point.  This queue point, containing messages from
 * a number of topic+partitions, may then be served by a single
 * consume() method, rather than one per topic+partition combination.
 *
 * See the RdKafka::Consumer::start(), RdKafka::Consumer::consume(), and
 * RdKafka::Consumer::consume_callback() methods that take a queue as the first
 * parameter for more information.
 */
class RD_EXPORT Queue {
 public:
  /**
   * @brief Create Queue object
   */
  static Queue *create(Handle *handle);

  /**
   * @brief Forward/re-route queue to \p dst.
   * If \p dst is \c NULL, the forwarding is removed.
   *
   * The internal refcounts for both queues are increased.
   *
   * @remark Regardless of whether \p dst is NULL or not, after calling this
   *         function, \p src will not forward it's fetch queue to the consumer
   *         queue.
   */
  virtual ErrorCode forward(Queue *dst) = 0;


  /**
   * @brief Consume message or get error event from the queue.
   *
   * @remark Use \c delete to free the message.
   *
   * @returns One of:
   *  - proper message (RdKafka::Message::err() is ERR_NO_ERROR)
   *  - error event (RdKafka::Message::err() is != ERR_NO_ERROR)
   *  - timeout due to no message or event in \p timeout_ms
   *    (RdKafka::Message::err() is ERR__TIMED_OUT)
   */
  virtual Message *consume(int timeout_ms) = 0;

  /**
   * @brief Poll queue, serving any enqueued callbacks.
   *
   * @remark Must NOT be used for queues containing messages.
   *
   * @returns the number of events served or 0 on timeout.
   */
  virtual int poll(int timeout_ms) = 0;

  virtual ~Queue() = 0;

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
   * @remark When using forwarded queues the IO event must only be enabled
   *         on the final forwarded-to (destination) queue.
   */
  virtual void io_event_enable(int fd, const void *payload, size_t size) = 0;
};

/**@}*/

/**
 * @name ConsumerGroupMetadata
 * @{
 *
 */
/**
 * @brief ConsumerGroupMetadata holds a consumer instance's group
 *        metadata state.
 *
 * This class currently does not have any public methods.
 */
class RD_EXPORT ConsumerGroupMetadata {
 public:
  virtual ~ConsumerGroupMetadata() = 0;
};

/**@}*/

/**
 * @name KafkaConsumer
 * @{
 *
 */


/**
 * @brief High-level KafkaConsumer (for brokers 0.9 and later)
 *
 * @remark Requires Apache Kafka >= 0.9.0 brokers
 *
 * Currently supports the \c range and \c roundrobin partition assignment
 * strategies (see \c partition.assignment.strategy)
 */
class RD_EXPORT KafkaConsumer : public virtual Handle {
 public:
  /**
   * @brief Creates a KafkaConsumer.
   *
   * The \p conf object must have \c group.id set to the consumer group to join.
   *
   * Use RdKafka::KafkaConsumer::close() to shut down the consumer.
   *
   * @sa RdKafka::RebalanceCb
   * @sa CONFIGURATION.md for \c group.id, \c session.timeout.ms,
   *     \c partition.assignment.strategy, etc.
   */
  static KafkaConsumer *create(const Conf *conf, std::string &errstr);

  virtual ~KafkaConsumer() = 0;


  /** @brief Returns the current partition assignment as set by
   *         RdKafka::KafkaConsumer::assign() */
  virtual ErrorCode assignment(
      std::vector<RdKafka::TopicPartition *> &partitions) = 0;

  /** @brief Returns the current subscription as set by
   *         RdKafka::KafkaConsumer::subscribe() */
  virtual ErrorCode subscription(std::vector<std::string> &topics) = 0;

  /**
   * @brief Update the subscription set to \p topics.
   *
   * Any previous subscription will be unassigned and  unsubscribed first.
   *
   * The subscription set denotes the desired topics to consume and this
   * set is provided to the partition assignor (one of the elected group
   * members) for all clients which then uses the configured
   * \c partition.assignment.strategy to assign the subscription sets's
   * topics's partitions to the consumers, depending on their subscription.
   *
   * The result of such an assignment is a rebalancing which is either
   * handled automatically in librdkafka or can be overridden by the application
   * by providing a RdKafka::RebalanceCb.
   *
   * The rebalancing passes the assigned partition set to
   * RdKafka::KafkaConsumer::assign() to update what partitions are actually
   * being fetched by the KafkaConsumer.
   *
   * Regex pattern matching automatically performed for topics prefixed
   * with \c \"^\" (e.g. \c \"^myPfx[0-9]_.*\"
   *
   * @remark A consumer error will be raised for each unavailable topic in the
   *  \p topics. The error will be ERR_UNKNOWN_TOPIC_OR_PART
   *  for non-existent topics, and
   *  ERR_TOPIC_AUTHORIZATION_FAILED for unauthorized topics.
   *  The consumer error will be raised through consume() (et.al.)
   *  with the \c RdKafka::Message::err() returning one of the
   *  error codes mentioned above.
   *  The subscribe function itself is asynchronous and will not return
   *  an error on unavailable topics.
   *
   * @returns an error if the provided list of topics is invalid.
   */
  virtual ErrorCode subscribe(const std::vector<std::string> &topics) = 0;

  /** @brief Unsubscribe from the current subscription set. */
  virtual ErrorCode unsubscribe() = 0;

  /**
   *  @brief Update the assignment set to \p partitions.
   *
   * The assignment set is the set of partitions actually being consumed
   * by the KafkaConsumer.
   */
  virtual ErrorCode assign(const std::vector<TopicPartition *> &partitions) = 0;

  /**
   * @brief Stop consumption and remove the current assignment.
   */
  virtual ErrorCode unassign() = 0;

  /**
   * @brief Consume message or get error event, triggers callbacks.
   *
   * Will automatically call registered callbacks for any such queued events,
   * including RdKafka::RebalanceCb, RdKafka::EventCb, RdKafka::OffsetCommitCb,
   * etc.
   *
   * @remark Use \c delete to free the message.
   *
   * @remark  An application should make sure to call consume() at regular
   *          intervals, even if no messages are expected, to serve any
   *          queued callbacks waiting to be called. This is especially
   *          important when a RebalanceCb has been registered as it needs
   *          to be called and handled properly to synchronize internal
   *          consumer state.
   *
   * @remark Application MUST NOT call \p poll() on KafkaConsumer objects.
   *
   * @returns One of:
   *  - proper message (RdKafka::Message::err() is ERR_NO_ERROR)
   *  - error event (RdKafka::Message::err() is != ERR_NO_ERROR)
   *  - timeout due to no message or event in \p timeout_ms
   *    (RdKafka::Message::err() is ERR__TIMED_OUT)
   */
  virtual Message *consume(int timeout_ms) = 0;

  /**
   * @brief Commit offsets for the current assignment.
   *
   * @remark This is the synchronous variant that blocks until offsets
   *         are committed or the commit fails (see return value).
   *
   * @remark If a RdKafka::OffsetCommitCb callback is registered it will
   *         be called with commit details on a future call to
   *         RdKafka::KafkaConsumer::consume()

   *
   * @returns ERR_NO_ERROR or error code.
   */
  virtual ErrorCode commitSync() = 0;

  /**
   * @brief Asynchronous version of RdKafka::KafkaConsumer::CommitSync()
   *
   * @sa RdKafka::KafkaConsumer::commitSync()
   */
  virtual ErrorCode commitAsync() = 0;

  /**
   * @brief Commit offset for a single topic+partition based on \p message
   *
   * @remark The offset committed will be the message's offset + 1.
   *
   * @remark This is the synchronous variant.
   *
   * @sa RdKafka::KafkaConsumer::commitSync()
   */
  virtual ErrorCode commitSync(Message *message) = 0;

  /**
   * @brief Commit offset for a single topic+partition based on \p message
   *
   * @remark The offset committed will be the message's offset + 1.
   *
   * @remark This is the asynchronous variant.
   *
   * @sa RdKafka::KafkaConsumer::commitSync()
   */
  virtual ErrorCode commitAsync(Message *message) = 0;

  /**
   * @brief Commit offsets for the provided list of partitions.
   *
   * @remark The \c .offset of the partitions in \p offsets should be the
   *         offset where consumption will resume, i.e., the last
   *         processed offset + 1.
   *
   * @remark This is the synchronous variant.
   */
  virtual ErrorCode commitSync(std::vector<TopicPartition *> &offsets) = 0;

  /**
   * @brief Commit offset for the provided list of partitions.
   *
   * @remark The \c .offset of the partitions in \p offsets should be the
   *         offset where consumption will resume, i.e., the last
   *         processed offset + 1.
   *
   * @remark This is the asynchronous variant.
   */
  virtual ErrorCode commitAsync(
      const std::vector<TopicPartition *> &offsets) = 0;

  /**
   * @brief Commit offsets for the current assignment.
   *
   * @remark This is the synchronous variant that blocks until offsets
   *         are committed or the commit fails (see return value).
   *
   * @remark The provided callback will be called from this function.
   *
   * @returns ERR_NO_ERROR or error code.
   */
  virtual ErrorCode commitSync(OffsetCommitCb *offset_commit_cb) = 0;

  /**
   * @brief Commit offsets for the provided list of partitions.
   *
   * @remark This is the synchronous variant that blocks until offsets
   *         are committed or the commit fails (see return value).
   *
   * @remark The provided callback will be called from this function.
   *
   * @returns ERR_NO_ERROR or error code.
   */
  virtual ErrorCode commitSync(std::vector<TopicPartition *> &offsets,
                               OffsetCommitCb *offset_commit_cb) = 0;



  /**
   * @brief Retrieve committed offsets for topics+partitions.
   *
   * @returns ERR_NO_ERROR on success in which case the
   *          \p offset or \p err field of each \p partitions' element is filled
   *          in with the stored offset, or a partition specific error.
   *          Else returns an error code.
   */
  virtual ErrorCode committed(std::vector<TopicPartition *> &partitions,
                              int timeout_ms) = 0;

  /**
   * @brief Retrieve current positions (offsets) for topics+partitions.
   *
   * @returns ERR_NO_ERROR on success in which case the
   *          \p offset or \p err field of each \p partitions' element is filled
   *          in with the stored offset, or a partition specific error.
   *          Else returns an error code.
   */
  virtual ErrorCode position(std::vector<TopicPartition *> &partitions) = 0;


  /**
   * For pausing and resuming consumption, see
   * @sa RdKafka::Handle::pause() and RdKafka::Handle::resume()
   */


  /**
   * @brief Close and shut down the consumer.
   *
   * This call will block until the following operations are finished:
   *  - Trigger a local rebalance to void the current assignment (if any).
   *  - Stop consumption for current assignment (if any).
   *  - Commit offsets (if any).
   *  - Leave group (if applicable).
   *
   * The maximum blocking time is roughly limited to session.timeout.ms.
   *
   * @remark Callbacks, such as RdKafka::RebalanceCb and
   *         RdKafka::OffsetCommitCb, etc, may be called.
   *
   * @remark The consumer object must later be freed with \c delete
   */
  virtual ErrorCode close() = 0;


  /**
   * @brief Seek consumer for topic+partition to offset which is either an
   *        absolute or logical offset.
   *
   * If \p timeout_ms is not 0 the call will wait this long for the
   * seek to be performed. If the timeout is reached the internal state
   * will be unknown and this function returns `ERR__TIMED_OUT`.
   * If \p timeout_ms is 0 it will initiate the seek but return
   * immediately without any error reporting (e.g., async).
   *
   * This call triggers a fetch queue barrier flush.
   *
   * @remark Consumption for the given partition must have started for the
   *         seek to work. Use assign() to set the starting offset.
   *
   * @returns an ErrorCode to indicate success or failure.
   */
  virtual ErrorCode seek(const TopicPartition &partition, int timeout_ms) = 0;


  /**
   * @brief Store offset \p offset for topic partition \p partition.
   * The offset will be committed (written) to the offset store according
   * to \p auto.commit.interval.ms or the next manual offset-less commit*()
   *
   * Per-partition success/error status propagated through TopicPartition.err()
   *
   * @remark The \c .offset field is stored as is, it will NOT be + 1.
   *
   * @remark \c enable.auto.offset.store must be set to \c false when using
   *         this API.
   *
   * @returns RdKafka::ERR_NO_ERROR on success, or
   *          RdKafka::ERR___UNKNOWN_PARTITION if none of the offsets could
   *          be stored, or
   *          RdKafka::ERR___INVALID_ARG if \c enable.auto.offset.store is true.
   */
  virtual ErrorCode offsets_store(std::vector<TopicPartition *> &offsets) = 0;


  /**
   * @returns the current consumer group metadata associated with this consumer,
   *          or NULL if the consumer is configured with a \c group.id.
   *          This metadata object should be passed to the transactional
   *          producer's RdKafka::Producer::send_offsets_to_transaction() API.
   *
   * @remark The returned object must be deleted by the application.
   *
   * @sa RdKafka::Producer::send_offsets_to_transaction()
   */
  virtual ConsumerGroupMetadata *groupMetadata() = 0;


  /** @brief Check whether the consumer considers the current assignment to
   *         have been lost involuntarily. This method is only applicable for
   *         use with a subscribing consumer. Assignments are revoked
   *         immediately when determined to have been lost, so this method is
   *         only useful within a rebalance callback. Partitions that have
   *         been lost may already be owned by other members in the group and
   *         therefore commiting offsets, for example, may fail.
   *
   * @remark Calling assign(), incremental_assign() or incremental_unassign()
   *         resets this flag.
   *
   * @returns Returns true if the current partition assignment is considered
   *          lost, false otherwise.
   */
  virtual bool assignment_lost() = 0;

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
   * @returns an empty string on error, or one of
   *          "NONE", "EAGER", "COOPERATIVE" on success.
   */

  virtual std::string rebalance_protocol() = 0;


  /**
   * @brief Incrementally add \p partitions to the current assignment.
   *
   * If a COOPERATIVE assignor (i.e. incremental rebalancing) is being used,
   * this method should be used in a rebalance callback to adjust the current
   * assignment appropriately in the case where the rebalance type is
   * ERR__ASSIGN_PARTITIONS. The application must pass the partition list
   * passed to the callback (or a copy of it), even if the list is empty.
   * This method may also be used outside the context of a rebalance callback.
   *
   * @returns NULL on success, or an error object if the operation was
   *          unsuccessful.
   *
   * @remark The returned object must be deleted by the application.
   */
  virtual Error *incremental_assign(
      const std::vector<TopicPartition *> &partitions) = 0;


  /**
   * @brief Incrementally remove \p partitions from the current assignment.
   *
   * If a COOPERATIVE assignor (i.e. incremental rebalancing) is being used,
   * this method should be used in a rebalance callback to adjust the current
   * assignment appropriately in the case where the rebalance type is
   * ERR__REVOKE_PARTITIONS. The application must pass the partition list
   * passed to the callback (or a copy of it), even if the list is empty.
   * This method may also be used outside the context of a rebalance callback.
   *
   * @returns NULL on success, or an error object if the operation was
   *          unsuccessful.
   *
   * @remark The returned object must be deleted by the application.
   */
  virtual Error *incremental_unassign(
      const std::vector<TopicPartition *> &partitions) = 0;

  /**
   * @brief Close and shut down the consumer.
   *
   * Performs the same actions as RdKafka::KafkaConsumer::close() but in a
   * background thread.
   *
   * Rebalance events/callbacks (etc) will be forwarded to the
   * application-provided \p queue. The application must poll this queue until
   * RdKafka::KafkaConsumer::closed() returns true.
   *
   * @remark Depending on consumer group join state there may or may not be
   *         rebalance events emitted on \p rkqu.
   *
   * @returns an error object if the consumer close failed, else NULL.
   *
   * @sa RdKafka::KafkaConsumer::closed()
   */
  virtual Error *close(Queue *queue) = 0;


  /** @returns true if the consumer is closed, else 0.
   *
   * @sa RdKafka::KafkaConsumer::close()
   */
  virtual bool closed() = 0;
};


/**@}*/


/**
 * @name Simple Consumer (legacy)
 * @{
 *
 */

/**
 * @brief Simple Consumer (legacy)
 *
 * A simple non-balanced, non-group-aware, consumer.
 */
class RD_EXPORT Consumer : public virtual Handle {
 public:
  /**
   * @brief Creates a new Kafka consumer handle.
   *
   * \p conf is an optional object that will be used instead of the default
   * configuration.
   * The \p conf object is reusable after this call.
   *
   * @returns the new handle on success or NULL on error in which case
   * \p errstr is set to a human readable error message.
   */
  static Consumer *create(const Conf *conf, std::string &errstr);

  virtual ~Consumer() = 0;


  /**
   * @brief Start consuming messages for topic and \p partition
   * at offset \p offset which may either be a proper offset (0..N)
   * or one of the the special offsets: \p OFFSET_BEGINNING or \p OFFSET_END.
   *
   * rdkafka will attempt to keep \p queued.min.messages (config property)
   * messages in the local queue by repeatedly fetching batches of messages
   * from the broker until the threshold is reached.
   *
   * The application shall use one of the \p ..->consume*() functions
   * to consume messages from the local queue, each kafka message being
   * represented as a `RdKafka::Message *` object.
   *
   * \p ..->start() must not be called multiple times for the same
   * topic and partition without stopping consumption first with
   * \p ..->stop().
   *
   * @returns an ErrorCode to indicate success or failure.
   */
  virtual ErrorCode start(Topic *topic, int32_t partition, int64_t offset) = 0;

  /**
   * @brief Start consuming messages for topic and \p partition on
   *        queue \p queue.
   *
   * @sa RdKafka::Consumer::start()
   */
  virtual ErrorCode start(Topic *topic,
                          int32_t partition,
                          int64_t offset,
                          Queue *queue) = 0;

  /**
   * @brief Stop consuming messages for topic and \p partition, purging
   *        all messages currently in the local queue.
   *
   * The application needs to be stop all consumers before destroying
   * the Consumer handle.
   *
   * @returns an ErrorCode to indicate success or failure.
   */
  virtual ErrorCode stop(Topic *topic, int32_t partition) = 0;

  /**
   * @brief Seek consumer for topic+partition to \p offset which is either an
   *        absolute or logical offset.
   *
   * If \p timeout_ms is not 0 the call will wait this long for the
   * seek to be performed. If the timeout is reached the internal state
   * will be unknown and this function returns `ERR__TIMED_OUT`.
   * If \p timeout_ms is 0 it will initiate the seek but return
   * immediately without any error reporting (e.g., async).
   *
   * This call triggers a fetch queue barrier flush.
   *
   * @returns an ErrorCode to indicate success or failure.
   */
  virtual ErrorCode seek(Topic *topic,
                         int32_t partition,
                         int64_t offset,
                         int timeout_ms) = 0;

  /**
   * @brief Consume a single message from \p topic and \p partition.
   *
   * \p timeout_ms is maximum amount of time to wait for a message to be
   * received.
   * Consumer must have been previously started with \p ..->start().
   *
   * @returns a Message object, the application needs to check if message
   * is an error or a proper message RdKafka::Message::err() and checking for
   * \p ERR_NO_ERROR.
   *
   * The message object must be destroyed when the application is done with it.
   *
   * Errors (in RdKafka::Message::err()):
   *  - ERR__TIMED_OUT - \p timeout_ms was reached with no new messages fetched.
   *  - ERR__PARTITION_EOF - End of partition reached, not an error.
   */
  virtual Message *consume(Topic *topic, int32_t partition, int timeout_ms) = 0;

  /**
   * @brief Consume a single message from the specified queue.
   *
   * \p timeout_ms is maximum amount of time to wait for a message to be
   * received.
   * Consumer must have been previously started on the queue with
   * \p ..->start().
   *
   * @returns a Message object, the application needs to check if message
   * is an error or a proper message \p Message->err() and checking for
   * \p ERR_NO_ERROR.
   *
   * The message object must be destroyed when the application is done with it.
   *
   * Errors (in RdKafka::Message::err()):
   *   - ERR__TIMED_OUT - \p timeout_ms was reached with no new messages fetched
   *
   * Note that Message->topic() may be nullptr after certain kinds of
   * errors, so applications should check that it isn't null before
   * dereferencing it.
   */
  virtual Message *consume(Queue *queue, int timeout_ms) = 0;

  /**
   * @brief Consumes messages from \p topic and \p partition, calling
   *        the provided callback for each consumed messsage.
   *
   * \p consume_callback() provides higher throughput performance
   * than \p consume().
   *
   * \p timeout_ms is the maximum amount of time to wait for one or
   * more messages to arrive.
   *
   * The provided \p consume_cb instance has its \p consume_cb function
   * called for every message received.
   *
   * The \p opaque argument is passed to the \p consume_cb as \p opaque.
   *
   * @returns the number of messages processed or -1 on error.
   *
   * @sa RdKafka::Consumer::consume()
   */
  virtual int consume_callback(Topic *topic,
                               int32_t partition,
                               int timeout_ms,
                               ConsumeCb *consume_cb,
                               void *opaque) = 0;

  /**
   * @brief Consumes messages from \p queue, calling the provided callback for
   *        each consumed messsage.
   *
   * @sa RdKafka::Consumer::consume_callback()
   */
  virtual int consume_callback(Queue *queue,
                               int timeout_ms,
                               RdKafka::ConsumeCb *consume_cb,
                               void *opaque) = 0;

  /**
   * @brief Converts an offset into the logical offset from the tail of a topic.
   *
   * \p offset is the (positive) number of items from the end.
   *
   * @returns the logical offset for message \p offset from the tail, this value
   *          may be passed to Consumer::start, et.al.
   * @remark The returned logical offset is specific to librdkafka.
   */
  static int64_t OffsetTail(int64_t offset);
};

/**@}*/


/**
 * @name Producer
 * @{
 *
 */


/**
 * @brief Producer
 */
class RD_EXPORT Producer : public virtual Handle {
 public:
  /**
   * @brief Creates a new Kafka producer handle.
   *
   * \p conf is an optional object that will be used instead of the default
   * configuration.
   * The \p conf object is reusable after this call.
   *
   * @returns the new handle on success or NULL on error in which case
   *          \p errstr is set to a human readable error message.
   */
  static Producer *create(const Conf *conf, std::string &errstr);


  virtual ~Producer() = 0;

  /**
   * @brief RdKafka::Producer::produce() \p msgflags
   *
   * These flags are optional.
   */
  enum {
    RK_MSG_FREE = 0x1, /**< rdkafka will free(3) \p payload
                        * when it is done with it.
                        * Mutually exclusive with RK_MSG_COPY. */
    RK_MSG_COPY = 0x2, /**< the \p payload data will be copied
                        * and the \p payload pointer will not
                        * be used by rdkafka after the
                        * call returns.
                        * Mutually exclusive with RK_MSG_FREE. */
    RK_MSG_BLOCK = 0x4 /**< Block produce*() on message queue
                        *   full.
                        *   WARNING:
                        *   If a delivery report callback
                        *   is used the application MUST
                        *   call rd_kafka_poll() (or equiv.)
                        *   to make sure delivered messages
                        *   are drained from the internal
                        *   delivery report queue.
                        *   Failure to do so will result
                        *   in indefinately blocking on
                        *   the produce() call when the
                        *   message queue is full.
                        */


  /**@cond NO_DOC*/
  /* For backwards compatibility: */
#ifndef MSG_COPY /* defined in sys/msg.h */
    ,            /** this comma must exist betwen
                  *  RK_MSG_BLOCK and MSG_FREE
                  */
    MSG_FREE = RK_MSG_FREE,
    MSG_COPY = RK_MSG_COPY
#endif
    /**@endcond*/
  };

  /**
   * @brief Produce and send a single message to broker.
   *
   * This is an asynch non-blocking API.
   *
   * \p partition is the target partition, either:
   *   - RdKafka::Topic::PARTITION_UA (unassigned) for
   *     automatic partitioning using the topic's partitioner function, or
   *   - a fixed partition (0..N)
   *
   * \p msgflags is zero or more of the following flags OR:ed together:
   *    RK_MSG_BLOCK - block \p produce*() call if
   *                   \p queue.buffering.max.messages or
   *                   \p queue.buffering.max.kbytes are exceeded.
   *                   Messages are considered in-queue from the point they
   *                   are accepted by produce() until their corresponding
   *                   delivery report callback/event returns.
   *                   It is thus a requirement to call
   *                   poll() (or equiv.) from a separate
   *                   thread when RK_MSG_BLOCK is used.
   *                   See WARNING on \c RK_MSG_BLOCK above.
   *    RK_MSG_FREE - rdkafka will free(3) \p payload when it is done with it.
   *    RK_MSG_COPY - the \p payload data will be copied and the \p payload
   *               pointer will not be used by rdkafka after the
   *               call returns.
   *
   *  NOTE: RK_MSG_FREE and RK_MSG_COPY are mutually exclusive.
   *
   *  If the function returns an error code and RK_MSG_FREE was specified, then
   *  the memory associated with the payload is still the caller's
   *  responsibility.
   *
   * \p payload is the message payload of size \p len bytes.
   *
   * \p key is an optional message key, if non-NULL it
   * will be passed to the topic partitioner as well as be sent with the
   * message to the broker and passed on to the consumer.
   *
   * \p msg_opaque is an optional application-provided per-message opaque
   * pointer that will provided in the delivery report callback (\p dr_cb) for
   * referencing this message.
   *
   * @returns an ErrorCode to indicate success or failure:
   *  - ERR_NO_ERROR           - message successfully enqueued for transmission.
   *
   *  - ERR__QUEUE_FULL        - maximum number of outstanding messages has been
   *                             reached: \c queue.buffering.max.message
   *
   *  - ERR_MSG_SIZE_TOO_LARGE - message is larger than configured max size:
   *                            \c messages.max.bytes
   *
   *  - ERR__UNKNOWN_PARTITION - requested \p partition is unknown in the
   *                           Kafka cluster.
   *
   *  - ERR__UNKNOWN_TOPIC     - topic is unknown in the Kafka cluster.
   */
  virtual ErrorCode produce(Topic *topic,
                            int32_t partition,
                            int msgflags,
                            void *payload,
                            size_t len,
                            const std::string *key,
                            void *msg_opaque) = 0;

  /**
   * @brief Variant produce() that passes the key as a pointer and length
   *        instead of as a const std::string *.
   */
  virtual ErrorCode produce(Topic *topic,
                            int32_t partition,
                            int msgflags,
                            void *payload,
                            size_t len,
                            const void *key,
                            size_t key_len,
                            void *msg_opaque) = 0;

  /**
   * @brief produce() variant that takes topic as a string (no need for
   *        creating a Topic object), and also allows providing the
   *        message timestamp (milliseconds since beginning of epoch, UTC).
   *        Otherwise identical to produce() above.
   */
  virtual ErrorCode produce(const std::string topic_name,
                            int32_t partition,
                            int msgflags,
                            void *payload,
                            size_t len,
                            const void *key,
                            size_t key_len,
                            int64_t timestamp,
                            void *msg_opaque) = 0;

  /**
   * @brief produce() variant that that allows for Header support on produce
   *        Otherwise identical to produce() above.
   *
   * @warning The \p headers will be freed/deleted if the produce() call
   *          succeeds, or left untouched if produce() fails.
   */
  virtual ErrorCode produce(const std::string topic_name,
                            int32_t partition,
                            int msgflags,
                            void *payload,
                            size_t len,
                            const void *key,
                            size_t key_len,
                            int64_t timestamp,
                            RdKafka::Headers *headers,
                            void *msg_opaque) = 0;


  /**
   * @brief Variant produce() that accepts vectors for key and payload.
   *        The vector data will be copied.
   */
  virtual ErrorCode produce(Topic *topic,
                            int32_t partition,
                            const std::vector<char> *payload,
                            const std::vector<char> *key,
                            void *msg_opaque) = 0;


  /**
   * @brief Wait until all outstanding produce requests, et.al, are completed.
   *        This should typically be done prior to destroying a producer
   * instance to make sure all queued and in-flight produce requests are
   * completed before terminating.
   *
   * @remark The \c linger.ms time will be ignored for the duration of the call,
   *         queued messages will be sent to the broker as soon as possible.
   *
   * @remark This function will call Producer::poll() and thus
   *         trigger callbacks.
   *
   * @returns ERR__TIMED_OUT if \p timeout_ms was reached before all
   *          outstanding requests were completed, else ERR_NO_ERROR
   */
  virtual ErrorCode flush(int timeout_ms) = 0;


  /**
   * @brief Purge messages currently handled by the producer instance.
   *
   * @param purge_flags tells which messages should be purged and how.
   *
   * The application will need to call Handle::poll() or Producer::flush()
   * afterwards to serve the delivery report callbacks of the purged messages.
   *
   * Messages purged from internal queues fail with the delivery report
   * error code set to ERR__PURGE_QUEUE, while purged messages that
   * are in-flight to or from the broker will fail with the error code set to
   * ERR__PURGE_INFLIGHT.
   *
   * @warning Purging messages that are in-flight to or from the broker
   *          will ignore any sub-sequent acknowledgement for these messages
   *          received from the broker, effectively making it impossible
   *          for the application to know if the messages were successfully
   *          produced or not. This may result in duplicate messages if the
   *          application retries these messages at a later time.
   *
   * @remark This call may block for a short time while background thread
   *         queues are purged.
   *
   * @returns ERR_NO_ERROR on success,
   *          ERR__INVALID_ARG if the \p purge flags are invalid or unknown,
   *          ERR__NOT_IMPLEMENTED if called on a non-producer client instance.
   */
  virtual ErrorCode purge(int purge_flags) = 0;

  /**
   * @brief RdKafka::Handle::purge() \p purge_flags
   */
  enum {
    PURGE_QUEUE = 0x1, /**< Purge messages in internal queues */

    PURGE_INFLIGHT = 0x2, /*! Purge messages in-flight to or from the broker.
                           *  Purging these messages will void any future
                           *  acknowledgements from the broker, making it
                           *  impossible for the application to know if these
                           *  messages were successfully delivered or not.
                           *  Retrying these messages may lead to duplicates. */

    PURGE_NON_BLOCKING = 0x4 /* Don't wait for background queue
                              * purging to finish. */
  };

  /**
   * @name Transactional API
   * @{
   *
   * Requires Kafka broker version v0.11.0 or later
   *
   * See the Transactional API documentation in rdkafka.h for more information.
   */

  /**
   * @brief Initialize transactions for the producer instance.
   *
   * @param timeout_ms The maximum time to block. On timeout the operation
   *                   may continue in the background, depending on state,
   *                   and it is okay to call init_transactions() again.
   *
   * @returns an RdKafka::Error object on error, or NULL on success.
   *          Check whether the returned error object permits retrying
   *          by calling RdKafka::Error::is_retriable(), or whether a fatal
   *          error has been raised by calling RdKafka::Error::is_fatal().
   *
   * @remark The returned error object (if not NULL) must be deleted.
   *
   * See rd_kafka_init_transactions() in rdkafka.h for more information.
   *
   */
  virtual Error *init_transactions(int timeout_ms) = 0;


  /**
   * @brief init_transactions() must have been called successfully
   *        (once) before this function is called.
   *
   * @returns an RdKafka::Error object on error, or NULL on success.
   *          Check whether a fatal error has been raised by calling
   *          RdKafka::Error::is_fatal_error().
   *
   * @remark The returned error object (if not NULL) must be deleted.
   *
   * See rd_kafka_begin_transaction() in rdkafka.h for more information.
   */
  virtual Error *begin_transaction() = 0;

  /**
   * @brief Sends a list of topic partition offsets to the consumer group
   *        coordinator for \p group_metadata, and marks the offsets as part
   *        part of the current transaction.
   *        These offsets will be considered committed only if the transaction
   *        is committed successfully.
   *
   *        The offsets should be the next message your application will
   *        consume,
   *        i.e., the last processed message's offset + 1 for each partition.
   *        Either track the offsets manually during processing or use
   *        RdKafka::KafkaConsumer::position() (on the consumer) to get the
   *        current offsets for
   *        the partitions assigned to the consumer.
   *
   *        Use this method at the end of a consume-transform-produce loop prior
   *        to committing the transaction with commit_transaction().
   *
   * @param offsets List of offsets to commit to the consumer group upon
   *                successful commit of the transaction. Offsets should be
   *                the next message to consume,
   *                e.g., last processed message + 1.
   * @param group_metadata The current consumer group metadata as returned by
   *                   RdKafka::KafkaConsumer::groupMetadata() on the consumer
   *                   instance the provided offsets were consumed from.
   * @param timeout_ms Maximum time allowed to register the
   *                   offsets on the broker.
   *
   * @remark This function must be called on the transactional producer
   *         instance, not the consumer.
   *
   * @remark The consumer must disable auto commits
   *         (set \c enable.auto.commit to false on the consumer).
   *
   * @returns an RdKafka::Error object on error, or NULL on success.
   *          Check whether the returned error object permits retrying
   *          by calling RdKafka::Error::is_retriable(), or whether an abortable
   *          or fatal error has been raised by calling
   *          RdKafka::Error::txn_requires_abort() or RdKafka::Error::is_fatal()
   *          respectively.
   *
   * @remark The returned error object (if not NULL) must be deleted.
   *
   * See rd_kafka_send_offsets_to_transaction() in rdkafka.h for
   * more information.
   */
  virtual Error *send_offsets_to_transaction(
      const std::vector<TopicPartition *> &offsets,
      const ConsumerGroupMetadata *group_metadata,
      int timeout_ms) = 0;

  /**
   * @brief Commit the current transaction as started with begin_transaction().
   *
   *        Any outstanding messages will be flushed (delivered) before actually
   *        committing the transaction.
   *
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
   * @returns an RdKafka::Error object on error, or NULL on success.
   *          Check whether the returned error object permits retrying
   *          by calling RdKafka::Error::is_retriable(), or whether an abortable
   *          or fatal error has been raised by calling
   *          RdKafka::Error::txn_requires_abort() or RdKafka::Error::is_fatal()
   *          respectively.
   *
   * @remark The returned error object (if not NULL) must be deleted.
   *
   * See rd_kafka_commit_transaction() in rdkafka.h for more information.
   */
  virtual Error *commit_transaction(int timeout_ms) = 0;

  /**
   * @brief Aborts the ongoing transaction.
   *
   *        This function should also be used to recover from non-fatal
   * abortable transaction errors.
   *
   *        Any outstanding messages will be purged and fail with
   *        RdKafka::ERR__PURGE_INFLIGHT or RdKafka::ERR__PURGE_QUEUE.
   *        See RdKafka::Producer::purge() for details.
   *
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
   * @returns an RdKafka::Error object on error, or NULL on success.
   *          Check whether the returned error object permits retrying
   *          by calling RdKafka::Error::is_retriable(), or whether a
   *          fatal error has been raised by calling RdKafka::Error::is_fatal().
   *
   * @remark The returned error object (if not NULL) must be deleted.
   *
   * See rd_kafka_abort_transaction() in rdkafka.h for more information.
   */
  virtual Error *abort_transaction(int timeout_ms) = 0;

  /**@}*/
};

/**@}*/


/**
 * @name Metadata interface
 * @{
 *
 */


/**
 * @brief Metadata: Broker information
 */
class BrokerMetadata {
 public:
  /** @returns Broker id */
  virtual int32_t id() const = 0;

  /** @returns Broker hostname */
  virtual std::string host() const = 0;

  /** @returns Broker listening port */
  virtual int port() const = 0;

  virtual ~BrokerMetadata() = 0;
};



/**
 * @brief Metadata: Partition information
 */
class PartitionMetadata {
 public:
  /** @brief Replicas */
  typedef std::vector<int32_t> ReplicasVector;
  /** @brief ISRs (In-Sync-Replicas) */
  typedef std::vector<int32_t> ISRSVector;

  /** @brief Replicas iterator */
  typedef ReplicasVector::const_iterator ReplicasIterator;
  /** @brief ISRs iterator */
  typedef ISRSVector::const_iterator ISRSIterator;


  /** @returns Partition id */
  virtual int32_t id() const = 0;

  /** @returns Partition error reported by broker */
  virtual ErrorCode err() const = 0;

  /** @returns Leader broker (id) for partition */
  virtual int32_t leader() const = 0;

  /** @returns Replica brokers */
  virtual const std::vector<int32_t> *replicas() const = 0;

  /** @returns In-Sync-Replica brokers
   *  @warning The broker may return a cached/outdated list of ISRs.
   */
  virtual const std::vector<int32_t> *isrs() const = 0;

  virtual ~PartitionMetadata() = 0;
};



/**
 * @brief Metadata: Topic information
 */
class TopicMetadata {
 public:
  /** @brief Partitions */
  typedef std::vector<const PartitionMetadata *> PartitionMetadataVector;
  /** @brief Partitions iterator */
  typedef PartitionMetadataVector::const_iterator PartitionMetadataIterator;

  /** @returns Topic name */
  virtual std::string topic() const = 0;

  /** @returns Partition list */
  virtual const PartitionMetadataVector *partitions() const = 0;

  /** @returns Topic error reported by broker */
  virtual ErrorCode err() const = 0;

  virtual ~TopicMetadata() = 0;
};


/**
 * @brief Metadata container
 */
class Metadata {
 public:
  /** @brief Brokers */
  typedef std::vector<const BrokerMetadata *> BrokerMetadataVector;
  /** @brief Topics */
  typedef std::vector<const TopicMetadata *> TopicMetadataVector;

  /** @brief Brokers iterator */
  typedef BrokerMetadataVector::const_iterator BrokerMetadataIterator;
  /** @brief Topics iterator */
  typedef TopicMetadataVector::const_iterator TopicMetadataIterator;


  /**
   * @brief Broker list
   * @remark Ownership of the returned pointer is retained by the instance of
   * Metadata that is called.
   */
  virtual const BrokerMetadataVector *brokers() const = 0;

  /**
   * @brief Topic list
   * @remark Ownership of the returned pointer is retained by the instance of
   * Metadata that is called.
   */
  virtual const TopicMetadataVector *topics() const = 0;

  /** @brief Broker (id) originating this metadata */
  virtual int32_t orig_broker_id() const = 0;

  /** @brief Broker (name) originating this metadata */
  virtual std::string orig_broker_name() const = 0;

  virtual ~Metadata() = 0;
};

/**@}*/

}  // namespace RdKafka


#endif /* _RDKAFKACPP_H_ */
