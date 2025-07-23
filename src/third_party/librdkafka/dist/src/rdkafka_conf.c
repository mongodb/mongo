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

#include "rdkafka_int.h"
#include "rd.h"
#include "rdfloat.h"

#include <stdlib.h>
#include <ctype.h>
#include <stddef.h>

#include "rdkafka_int.h"
#include "rdkafka_feature.h"
#include "rdkafka_interceptor.h"
#include "rdkafka_idempotence.h"
#include "rdkafka_assignor.h"
#include "rdkafka_sasl_oauthbearer.h"
#if WITH_PLUGINS
#include "rdkafka_plugin.h"
#endif
#include "rdunittest.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#else

#ifndef WIN32_MEAN_AND_LEAN
#define WIN32_MEAN_AND_LEAN
#endif
#include <windows.h>
#endif

struct rd_kafka_property {
        rd_kafka_conf_scope_t scope;
        const char *name;
        enum { _RK_C_STR,
               _RK_C_INT,
               _RK_C_DBL, /* Double */
               _RK_C_S2I, /* String to Integer mapping.
                           * Supports limited canonical str->int mappings
                           * using s2i[] */
               _RK_C_S2F, /* CSV String to Integer flag mapping (OR:ed) */
               _RK_C_BOOL,
               _RK_C_PTR,     /* Only settable through special set functions */
               _RK_C_PATLIST, /* Pattern list */
               _RK_C_KSTR,    /* Kafka string */
               _RK_C_ALIAS, /* Alias: points to other property through .sdef */
               _RK_C_INTERNAL, /* Internal, don't expose to application */
               _RK_C_INVALID,  /* Invalid property, used to catch known
                                * but unsupported Java properties. */
        } type;
        int offset;
        const char *desc;
        int vmin;
        int vmax;
        int vdef;         /* Default value (int) */
        const char *sdef; /* Default value (string) */
        void *pdef;       /* Default value (pointer) */
        double ddef;      /* Default value (double) */
        double dmin;
        double dmax;
        struct {
                int val;
                const char *str;
                const char *unsupported; /**< Reason for value not being
                                          *   supported in this build. */
        } s2i[20];                       /* _RK_C_S2I and _RK_C_S2F */

        const char *unsupported; /**< Reason for propery not being supported
                                  *   in this build.
                                  *   Will be included in the conf_set()
                                  *   error string. */

        /* Value validator (STR) */
        int (*validate)(const struct rd_kafka_property *prop,
                        const char *val,
                        int ival);

        /* Configuration object constructors and destructor for use when
         * the property value itself is not used, or needs extra care. */
        void (*ctor)(int scope, void *pconf);
        void (*dtor)(int scope, void *pconf);
        void (*copy)(int scope,
                     void *pdst,
                     const void *psrc,
                     void *dstptr,
                     const void *srcptr,
                     size_t filter_cnt,
                     const char **filter);

        rd_kafka_conf_res_t (*set)(int scope,
                                   void *pconf,
                                   const char *name,
                                   const char *value,
                                   void *dstptr,
                                   rd_kafka_conf_set_mode_t set_mode,
                                   char *errstr,
                                   size_t errstr_size);
};


#define _RK(field)  offsetof(rd_kafka_conf_t, field)
#define _RKT(field) offsetof(rd_kafka_topic_conf_t, field)

#if WITH_SSL
#define _UNSUPPORTED_SSL .unsupported = NULL
#else
#define _UNSUPPORTED_SSL .unsupported = "OpenSSL not available at build time"
#endif

#if OPENSSL_VERSION_NUMBER >= 0x1000200fL && defined(WITH_SSL) &&              \
    !defined(LIBRESSL_VERSION_NUMBER)
#define _UNSUPPORTED_OPENSSL_1_0_2 .unsupported = NULL
#else
#define _UNSUPPORTED_OPENSSL_1_0_2                                             \
        .unsupported = "OpenSSL >= 1.0.2 not available at build time"
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10100000 && defined(WITH_SSL) &&               \
    !defined(LIBRESSL_VERSION_NUMBER)
#define _UNSUPPORTED_OPENSSL_1_1_0 .unsupported = NULL
#else
#define _UNSUPPORTED_OPENSSL_1_1_0                                             \
        .unsupported = "OpenSSL >= 1.1.0 not available at build time"
#endif

#if WITH_SSL_ENGINE
#define _UNSUPPORTED_SSL_ENGINE .unsupported = NULL
#else
#define _UNSUPPORTED_SSL_ENGINE                                                \
        .unsupported = "OpenSSL >= 1.1.x not available at build time"
#endif

#if OPENSSL_VERSION_NUMBER >= 0x30000000 && defined(WITH_SSL)
#define _UNSUPPORTED_SSL_3 .unsupported = NULL
#else
#define _UNSUPPORTED_SSL_3                                                     \
        .unsupported = "OpenSSL >= 3.0.0 not available at build time"
#endif


#if WITH_ZLIB
#define _UNSUPPORTED_ZLIB .unsupported = NULL
#else
#define _UNSUPPORTED_ZLIB .unsupported = "zlib not available at build time"
#endif

#if WITH_SNAPPY
#define _UNSUPPORTED_SNAPPY .unsupported = NULL
#else
#define _UNSUPPORTED_SNAPPY .unsupported = "snappy not enabled at build time"
#endif

#if WITH_ZSTD
#define _UNSUPPORTED_ZSTD .unsupported = NULL
#else
#define _UNSUPPORTED_ZSTD .unsupported = "libzstd not available at build time"
#endif

#if WITH_CURL
#define _UNSUPPORTED_HTTP .unsupported = NULL
#else
#define _UNSUPPORTED_HTTP .unsupported = "libcurl not available at build time"
#endif

#if WITH_OAUTHBEARER_OIDC
#define _UNSUPPORTED_OIDC .unsupported = NULL
#else
#define _UNSUPPORTED_OIDC                                                      \
        .unsupported =                                                         \
            "OAuth/OIDC depends on libcurl and OpenSSL which were not "        \
            "available at build time"
#endif

#ifdef _WIN32
#define _UNSUPPORTED_WIN32_GSSAPI                                              \
        .unsupported =                                                         \
            "Kerberos keytabs are not supported on Windows, "                  \
            "instead the logged on "                                           \
            "user's credentials are used through native SSPI"
#else
#define _UNSUPPORTED_WIN32_GSSAPI .unsupported = NULL
#endif

#if defined(_WIN32) || defined(WITH_SASL_CYRUS)
#define _UNSUPPORTED_GSSAPI .unsupported = NULL
#else
#define _UNSUPPORTED_GSSAPI                                                    \
        .unsupported = "cyrus-sasl/libsasl2 not available at build time"
#endif

#define _UNSUPPORTED_OAUTHBEARER _UNSUPPORTED_SSL


static rd_kafka_conf_res_t
rd_kafka_anyconf_get0(const void *conf,
                      const struct rd_kafka_property *prop,
                      char *dest,
                      size_t *dest_size);



/**
 * @returns a unique index for property \p prop, using the byte position
 *          of the field.
 */
static RD_INLINE int rd_kafka_prop2idx(const struct rd_kafka_property *prop) {
        return prop->offset;
}



/**
 * @brief Set the property as modified.
 *
 * We do this by mapping the property's conf struct field byte offset
 * to a bit in a bit vector.
 * If the bit is set the property has been modified, otherwise it is
 * at its default unmodified value.
 *
 * \p is_modified 1: set as modified, 0: clear modified
 */
static void rd_kafka_anyconf_set_modified(void *conf,
                                          const struct rd_kafka_property *prop,
                                          int is_modified) {
        int idx                              = rd_kafka_prop2idx(prop);
        int bkt                              = idx / 64;
        uint64_t bit                         = (uint64_t)1 << (idx % 64);
        struct rd_kafka_anyconf_hdr *confhdr = conf;

        rd_assert(idx < RD_KAFKA_CONF_PROPS_IDX_MAX &&
                  *"Increase RD_KAFKA_CONF_PROPS_IDX_MAX");

        if (is_modified)
                confhdr->modified[bkt] |= bit;
        else
                confhdr->modified[bkt] &= ~bit;
}

/**
 * @brief Clear is_modified for all properties.
 * @warning Does NOT clear/reset the value.
 */
static void rd_kafka_anyconf_clear_all_is_modified(void *conf) {
        struct rd_kafka_anyconf_hdr *confhdr = conf;

        memset(confhdr, 0, sizeof(*confhdr));
}


/**
 * @returns true of the property has been set/modified, else false.
 */
static rd_bool_t
rd_kafka_anyconf_is_modified(const void *conf,
                             const struct rd_kafka_property *prop) {
        int idx                                    = rd_kafka_prop2idx(prop);
        int bkt                                    = idx / 64;
        uint64_t bit                               = (uint64_t)1 << (idx % 64);
        const struct rd_kafka_anyconf_hdr *confhdr = conf;

        return !!(confhdr->modified[bkt] & bit);
}

/**
 * @returns true if any property in \p conf has been set/modified.
 */
static rd_bool_t rd_kafka_anyconf_is_any_modified(const void *conf) {
        const struct rd_kafka_anyconf_hdr *confhdr = conf;
        int i;

        for (i = 0; i < (int)RD_ARRAYSIZE(confhdr->modified); i++)
                if (confhdr->modified[i])
                        return rd_true;

        return rd_false;
}



/**
 * @brief Validate \p broker.version.fallback property.
 */
static int
rd_kafka_conf_validate_broker_version(const struct rd_kafka_property *prop,
                                      const char *val,
                                      int ival) {
        struct rd_kafka_ApiVersion *apis;
        size_t api_cnt;
        return rd_kafka_get_legacy_ApiVersions(val, &apis, &api_cnt, NULL);
}

/**
 * @brief Validate that string is a single item, without delimters (, space).
 */
static RD_UNUSED int
rd_kafka_conf_validate_single(const struct rd_kafka_property *prop,
                              const char *val,
                              int ival) {
        return !strchr(val, ',') && !strchr(val, ' ');
}

/**
 * @brief Validate builtin partitioner string
 */
static RD_UNUSED int
rd_kafka_conf_validate_partitioner(const struct rd_kafka_property *prop,
                                   const char *val,
                                   int ival) {
        return !strcmp(val, "random") || !strcmp(val, "consistent") ||
               !strcmp(val, "consistent_random") || !strcmp(val, "murmur2") ||
               !strcmp(val, "murmur2_random") || !strcmp(val, "fnv1a") ||
               !strcmp(val, "fnv1a_random");
}


/**
 * librdkafka configuration property definitions.
 */
static const struct rd_kafka_property rd_kafka_properties[] = {
    /* Global properties */
    {_RK_GLOBAL, "builtin.features", _RK_C_S2F, _RK(builtin_features),
     "Indicates the builtin features for this build of librdkafka. "
     "An application can either query this value or attempt to set it "
     "with its list of required features to check for library support.",
     0, 0x7fffffff, 0xffff,
     .s2i = {{0x1, "gzip", _UNSUPPORTED_ZLIB},
             {0x2, "snappy", _UNSUPPORTED_SNAPPY},
             {0x4, "ssl", _UNSUPPORTED_SSL},
             {0x8, "sasl"},
             {0x10, "regex"},
             {0x20, "lz4"},
             {0x40, "sasl_gssapi", _UNSUPPORTED_GSSAPI},
             {0x80, "sasl_plain"},
             {0x100, "sasl_scram", _UNSUPPORTED_SSL},
             {0x200, "plugins"
#if !WITH_PLUGINS
              ,
              .unsupported = "libdl/dlopen(3) not available at "
                             "build time"
#endif
             },
             {0x400, "zstd", _UNSUPPORTED_ZSTD},
             {0x800, "sasl_oauthbearer", _UNSUPPORTED_SSL},
             {0x1000, "http", _UNSUPPORTED_HTTP},
             {0x2000, "oidc", _UNSUPPORTED_OIDC},
             {0, NULL}}},
    {_RK_GLOBAL, "client.id", _RK_C_STR, _RK(client_id_str),
     "Client identifier.", .sdef = "rdkafka"},
    {_RK_GLOBAL | _RK_HIDDEN, "client.software.name", _RK_C_STR, _RK(sw_name),
     "Client software name as reported to broker version >= v2.4.0. "
     "Broker-side character restrictions apply, as of broker version "
     "v2.4.0 the allowed characters are `a-zA-Z0-9.-`. The local client "
     "will replace any other character with `-` and strip leading and "
     "trailing non-alphanumeric characters before tranmission to "
     "the broker. "
     "This property should only be set by high-level language "
     "librdkafka client bindings.",
     .sdef = "librdkafka"},
    {
        _RK_GLOBAL | _RK_HIDDEN,
        "client.software.version",
        _RK_C_STR,
        _RK(sw_version),
        "Client software version as reported to broker version >= v2.4.0. "
        "Broker-side character restrictions apply, as of broker version "
        "v2.4.0 the allowed characters are `a-zA-Z0-9.-`. The local client "
        "will replace any other character with `-` and strip leading and "
        "trailing non-alphanumeric characters before tranmission to "
        "the broker. "
        "This property should only be set by high-level language "
        "librdkafka client bindings."
        "If changing this property it is highly recommended to append the "
        "librdkafka version.",
    },
    {_RK_GLOBAL | _RK_HIGH, "metadata.broker.list", _RK_C_STR, _RK(brokerlist),
     "Initial list of brokers as a CSV list of broker host or host:port. "
     "The application may also use `rd_kafka_brokers_add()` to add "
     "brokers during runtime."},
    {_RK_GLOBAL | _RK_HIGH, "bootstrap.servers", _RK_C_ALIAS, 0,
     "See metadata.broker.list", .sdef = "metadata.broker.list"},
    {_RK_GLOBAL | _RK_MED, "message.max.bytes", _RK_C_INT, _RK(max_msg_size),
     "Maximum Kafka protocol request message size. "
     "Due to differing framing overhead between protocol versions the "
     "producer is unable to reliably enforce a strict max message limit "
     "at produce time and may exceed the maximum size by one message in "
     "protocol ProduceRequests, the broker will enforce the the topic's "
     "`max.message.bytes` limit (see Apache Kafka documentation).",
     1000, 1000000000, 1000000},
    {_RK_GLOBAL, "message.copy.max.bytes", _RK_C_INT, _RK(msg_copy_max_size),
     "Maximum size for message to be copied to buffer. "
     "Messages larger than this will be passed by reference (zero-copy) "
     "at the expense of larger iovecs.",
     0, 1000000000, 0xffff},
    {_RK_GLOBAL | _RK_MED, "receive.message.max.bytes", _RK_C_INT,
     _RK(recv_max_msg_size),
     "Maximum Kafka protocol response message size. "
     "This serves as a safety precaution to avoid memory exhaustion in "
     "case of protocol hickups. "
     "This value must be at least `fetch.max.bytes`  + 512 to allow "
     "for protocol overhead; the value is adjusted automatically "
     "unless the configuration property is explicitly set.",
     1000, INT_MAX, 100000000},
    {_RK_GLOBAL, "max.in.flight.requests.per.connection", _RK_C_INT,
     _RK(max_inflight),
     "Maximum number of in-flight requests per broker connection. "
     "This is a generic property applied to all broker communication, "
     "however it is primarily relevant to produce requests. "
     "In particular, note that other mechanisms limit the number "
     "of outstanding consumer fetch request per broker to one.",
     1, 1000000, 1000000},
    {_RK_GLOBAL, "max.in.flight", _RK_C_ALIAS,
     .sdef = "max.in.flight.requests.per.connection"},
    {_RK_GLOBAL | _RK_DEPRECATED | _RK_HIDDEN, "metadata.request.timeout.ms",
     _RK_C_INT, _RK(metadata_request_timeout_ms), "Not used.", 10, 900 * 1000,
     10},
    {_RK_GLOBAL, "topic.metadata.refresh.interval.ms", _RK_C_INT,
     _RK(metadata_refresh_interval_ms),
     "Period of time in milliseconds at which topic and broker "
     "metadata is refreshed in order to proactively discover any new "
     "brokers, topics, partitions or partition leader changes. "
     "Use -1 to disable the intervalled refresh (not recommended). "
     "If there are no locally referenced topics "
     "(no topic objects created, no messages produced, "
     "no subscription or no assignment) then only the broker list will "
     "be refreshed every interval but no more often than every 10s.",
     -1, 3600 * 1000, 5 * 60 * 1000},
    {_RK_GLOBAL, "metadata.max.age.ms", _RK_C_INT, _RK(metadata_max_age_ms),
     "Metadata cache max age. "
     "Defaults to topic.metadata.refresh.interval.ms * 3",
     1, 24 * 3600 * 1000, 5 * 60 * 1000 * 3},
    {_RK_GLOBAL, "topic.metadata.refresh.fast.interval.ms", _RK_C_INT,
     _RK(metadata_refresh_fast_interval_ms),
     "When a topic loses its leader a new metadata request will be "
     "enqueued with this initial interval, exponentially increasing "
     "until the topic metadata has been refreshed. "
     "This is used to recover quickly from transitioning leader brokers.",
     1, 60 * 1000, 250},
    {_RK_GLOBAL | _RK_DEPRECATED, "topic.metadata.refresh.fast.cnt", _RK_C_INT,
     _RK(metadata_refresh_fast_cnt), "No longer used.", 0, 1000, 10},
    {_RK_GLOBAL, "topic.metadata.refresh.sparse", _RK_C_BOOL,
     _RK(metadata_refresh_sparse),
     "Sparse metadata requests (consumes less network bandwidth)", 0, 1, 1},
    {_RK_GLOBAL, "topic.metadata.propagation.max.ms", _RK_C_INT,
     _RK(metadata_propagation_max_ms),
     "Apache Kafka topic creation is asynchronous and it takes some "
     "time for a new topic to propagate throughout the cluster to all "
     "brokers. "
     "If a client requests topic metadata after manual topic creation but "
     "before the topic has been fully propagated to the broker the "
     "client is requesting metadata from, the topic will seem to be "
     "non-existent and the client will mark the topic as such, "
     "failing queued produced messages with `ERR__UNKNOWN_TOPIC`. "
     "This setting delays marking a topic as non-existent until the "
     "configured propagation max time has passed. "
     "The maximum propagation time is calculated from the time the "
     "topic is first referenced in the client, e.g., on produce().",
     0, 60 * 60 * 1000, 30 * 1000},
    {_RK_GLOBAL, "topic.blacklist", _RK_C_PATLIST, _RK(topic_blacklist),
     "Topic blacklist, a comma-separated list of regular expressions "
     "for matching topic names that should be ignored in "
     "broker metadata information as if the topics did not exist."},
    {_RK_GLOBAL | _RK_MED, "debug", _RK_C_S2F, _RK(debug),
     "A comma-separated list of debug contexts to enable. "
     "Detailed Producer debugging: broker,topic,msg. "
     "Consumer: consumer,cgrp,topic,fetch",
     .s2i = {{RD_KAFKA_DBG_GENERIC, "generic"},
             {RD_KAFKA_DBG_BROKER, "broker"},
             {RD_KAFKA_DBG_TOPIC, "topic"},
             {RD_KAFKA_DBG_METADATA, "metadata"},
             {RD_KAFKA_DBG_FEATURE, "feature"},
             {RD_KAFKA_DBG_QUEUE, "queue"},
             {RD_KAFKA_DBG_MSG, "msg"},
             {RD_KAFKA_DBG_PROTOCOL, "protocol"},
             {RD_KAFKA_DBG_CGRP, "cgrp"},
             {RD_KAFKA_DBG_SECURITY, "security"},
             {RD_KAFKA_DBG_FETCH, "fetch"},
             {RD_KAFKA_DBG_INTERCEPTOR, "interceptor"},
             {RD_KAFKA_DBG_PLUGIN, "plugin"},
             {RD_KAFKA_DBG_CONSUMER, "consumer"},
             {RD_KAFKA_DBG_ADMIN, "admin"},
             {RD_KAFKA_DBG_EOS, "eos"},
             {RD_KAFKA_DBG_MOCK, "mock"},
             {RD_KAFKA_DBG_ASSIGNOR, "assignor"},
             {RD_KAFKA_DBG_CONF, "conf"},
             {RD_KAFKA_DBG_ALL, "all"}}},
    {_RK_GLOBAL, "socket.timeout.ms", _RK_C_INT, _RK(socket_timeout_ms),
     "Default timeout for network requests. "
     "Producer: ProduceRequests will use the lesser value of "
     "`socket.timeout.ms` and remaining `message.timeout.ms` for the "
     "first message in the batch. "
     "Consumer: FetchRequests will use "
     "`fetch.wait.max.ms` + `socket.timeout.ms`. "
     "Admin: Admin requests will use `socket.timeout.ms` or explicitly "
     "set `rd_kafka_AdminOptions_set_operation_timeout()` value.",
     10, 300 * 1000, 60 * 1000},
    {_RK_GLOBAL | _RK_DEPRECATED, "socket.blocking.max.ms", _RK_C_INT,
     _RK(socket_blocking_max_ms), "No longer used.", 1, 60 * 1000, 1000},
    {_RK_GLOBAL, "socket.send.buffer.bytes", _RK_C_INT, _RK(socket_sndbuf_size),
     "Broker socket send buffer size. System default is used if 0.", 0,
     100000000, 0},
    {_RK_GLOBAL, "socket.receive.buffer.bytes", _RK_C_INT,
     _RK(socket_rcvbuf_size),
     "Broker socket receive buffer size. System default is used if 0.", 0,
     100000000, 0},
    {_RK_GLOBAL, "socket.keepalive.enable", _RK_C_BOOL, _RK(socket_keepalive),
     "Enable TCP keep-alives (SO_KEEPALIVE) on broker sockets", 0, 1, 0
#ifndef SO_KEEPALIVE
     ,
     .unsupported = "SO_KEEPALIVE not available at build time"
#endif
    },
    {_RK_GLOBAL, "socket.nagle.disable", _RK_C_BOOL, _RK(socket_nagle_disable),
     "Disable the Nagle algorithm (TCP_NODELAY) on broker sockets.", 0, 1, 0
#ifndef TCP_NODELAY
     ,
     .unsupported = "TCP_NODELAY not available at build time"
#endif
    },
    {_RK_GLOBAL, "socket.max.fails", _RK_C_INT, _RK(socket_max_fails),
     "Disconnect from broker when this number of send failures "
     "(e.g., timed out requests) is reached. Disable with 0. "
     "WARNING: It is highly recommended to leave this setting at "
     "its default value of 1 to avoid the client and broker to "
     "become desynchronized in case of request timeouts. "
     "NOTE: The connection is automatically re-established.",
     0, 1000000, 1},
    {_RK_GLOBAL, "broker.address.ttl", _RK_C_INT, _RK(broker_addr_ttl),
     "How long to cache the broker address resolving "
     "results (milliseconds).",
     0, 86400 * 1000, 1 * 1000},
    {_RK_GLOBAL, "broker.address.family", _RK_C_S2I, _RK(broker_addr_family),
     "Allowed broker IP address families: any, v4, v6", .vdef = AF_UNSPEC,
     .s2i =
         {
             {AF_UNSPEC, "any"},
             {AF_INET, "v4"},
             {AF_INET6, "v6"},
         }},
    {_RK_GLOBAL | _RK_MED, "socket.connection.setup.timeout.ms", _RK_C_INT,
     _RK(socket_connection_setup_timeout_ms),
     "Maximum time allowed for broker connection setup "
     "(TCP connection setup as well SSL and SASL handshake). "
     "If the connection to the broker is not fully functional after this "
     "the connection will be closed and retried.",
     1000, INT_MAX, 30 * 1000 /* 30s */},
    {_RK_GLOBAL | _RK_MED, "connections.max.idle.ms", _RK_C_INT,
     _RK(connections_max_idle_ms),
     "Close broker connections after the specified time of "
     "inactivity. "
     "Disable with 0. "
     "If this property is left at its default value some heuristics are "
     "performed to determine a suitable default value, this is currently "
     "limited to identifying brokers on Azure "
     "(see librdkafka issue #3109 for more info).",
     0, INT_MAX, 0},
    {_RK_GLOBAL | _RK_MED | _RK_HIDDEN, "enable.sparse.connections", _RK_C_BOOL,
     _RK(sparse_connections),
     "When enabled the client will only connect to brokers "
     "it needs to communicate with. When disabled the client "
     "will maintain connections to all brokers in the cluster.",
     0, 1, 1},
    {_RK_GLOBAL | _RK_DEPRECATED, "reconnect.backoff.jitter.ms", _RK_C_INT,
     _RK(reconnect_jitter_ms),
     "No longer used. See `reconnect.backoff.ms` and "
     "`reconnect.backoff.max.ms`.",
     0, 60 * 60 * 1000, 0},
    {_RK_GLOBAL | _RK_MED, "reconnect.backoff.ms", _RK_C_INT,
     _RK(reconnect_backoff_ms),
     "The initial time to wait before reconnecting to a broker "
     "after the connection has been closed. "
     "The time is increased exponentially until "
     "`reconnect.backoff.max.ms` is reached. "
     "-25% to +50% jitter is applied to each reconnect backoff. "
     "A value of 0 disables the backoff and reconnects immediately.",
     0, 60 * 60 * 1000, 100},
    {_RK_GLOBAL | _RK_MED, "reconnect.backoff.max.ms", _RK_C_INT,
     _RK(reconnect_backoff_max_ms),
     "The maximum time to wait before reconnecting to a broker "
     "after the connection has been closed.",
     0, 60 * 60 * 1000, 10 * 1000},
    {_RK_GLOBAL | _RK_HIGH, "statistics.interval.ms", _RK_C_INT,
     _RK(stats_interval_ms),
     "librdkafka statistics emit interval. The application also needs to "
     "register a stats callback using `rd_kafka_conf_set_stats_cb()`. "
     "The granularity is 1000ms. A value of 0 disables statistics.",
     0, 86400 * 1000, 0},
    {_RK_GLOBAL, "enabled_events", _RK_C_INT, _RK(enabled_events),
     "See `rd_kafka_conf_set_events()`", 0, 0x7fffffff, 0},
    {_RK_GLOBAL, "error_cb", _RK_C_PTR, _RK(error_cb),
     "Error callback (set with rd_kafka_conf_set_error_cb())"},
    {_RK_GLOBAL, "throttle_cb", _RK_C_PTR, _RK(throttle_cb),
     "Throttle callback (set with rd_kafka_conf_set_throttle_cb())"},
    {_RK_GLOBAL, "stats_cb", _RK_C_PTR, _RK(stats_cb),
     "Statistics callback (set with rd_kafka_conf_set_stats_cb())"},
    {_RK_GLOBAL, "log_cb", _RK_C_PTR, _RK(log_cb),
     "Log callback (set with rd_kafka_conf_set_log_cb())",
     .pdef = rd_kafka_log_print},
    {_RK_GLOBAL, "log_level", _RK_C_INT, _RK(log_level),
     "Logging level (syslog(3) levels)", 0, 7, 6},
    {_RK_GLOBAL, "log.queue", _RK_C_BOOL, _RK(log_queue),
     "Disable spontaneous log_cb from internal librdkafka "
     "threads, instead enqueue log messages on queue set with "
     "`rd_kafka_set_log_queue()` and serve log callbacks or "
     "events through the standard poll APIs. "
     "**NOTE**: Log messages will linger in a temporary queue "
     "until the log queue has been set.",
     0, 1, 0},
    {_RK_GLOBAL, "log.thread.name", _RK_C_BOOL, _RK(log_thread_name),
     "Print internal thread name in log messages "
     "(useful for debugging librdkafka internals)",
     0, 1, 1},
    {_RK_GLOBAL, "enable.random.seed", _RK_C_BOOL, _RK(enable_random_seed),
     "If enabled librdkafka will initialize the PRNG "
     "with srand(current_time.milliseconds) on the first invocation of "
     "rd_kafka_new() (required only if rand_r() is not available on your "
     "platform). "
     "If disabled the application must call srand() prior to calling "
     "rd_kafka_new().",
     0, 1, 1},
    {_RK_GLOBAL, "log.connection.close", _RK_C_BOOL, _RK(log_connection_close),
     "Log broker disconnects. "
     "It might be useful to turn this off when interacting with "
     "0.9 brokers with an aggressive `connections.max.idle.ms` value.",
     0, 1, 1},
    {_RK_GLOBAL, "background_event_cb", _RK_C_PTR, _RK(background_event_cb),
     "Background queue event callback "
     "(set with rd_kafka_conf_set_background_event_cb())"},
    {_RK_GLOBAL, "socket_cb", _RK_C_PTR, _RK(socket_cb),
     "Socket creation callback to provide race-free CLOEXEC",
     .pdef =
#ifdef __linux__
         rd_kafka_socket_cb_linux
#else
          rd_kafka_socket_cb_generic
#endif
    },
    {
        _RK_GLOBAL,
        "connect_cb",
        _RK_C_PTR,
        _RK(connect_cb),
        "Socket connect callback",
    },
    {
        _RK_GLOBAL,
        "closesocket_cb",
        _RK_C_PTR,
        _RK(closesocket_cb),
        "Socket close callback",
    },
    {_RK_GLOBAL, "open_cb", _RK_C_PTR, _RK(open_cb),
     "File open callback to provide race-free CLOEXEC",
     .pdef =
#ifdef __linux__
         rd_kafka_open_cb_linux
#else
          rd_kafka_open_cb_generic
#endif
    },
    {_RK_GLOBAL, "resolve_cb", _RK_C_PTR, _RK(resolve_cb),
     "Address resolution callback (set with rd_kafka_conf_set_resolve_cb())."},
    {_RK_GLOBAL, "opaque", _RK_C_PTR, _RK(opaque),
     "Application opaque (set with rd_kafka_conf_set_opaque())"},
    {_RK_GLOBAL, "default_topic_conf", _RK_C_PTR, _RK(topic_conf),
     "Default topic configuration for automatically subscribed topics"},
    {_RK_GLOBAL, "internal.termination.signal", _RK_C_INT, _RK(term_sig),
     "Signal that librdkafka will use to quickly terminate on "
     "rd_kafka_destroy(). If this signal is not set then there will be a "
     "delay before rd_kafka_wait_destroyed() returns true "
     "as internal threads are timing out their system calls. "
     "If this signal is set however the delay will be minimal. "
     "The application should mask this signal as an internal "
     "signal handler is installed.",
     0, 128, 0},
    {_RK_GLOBAL | _RK_HIGH, "api.version.request", _RK_C_BOOL,
     _RK(api_version_request),
     "Request broker's supported API versions to adjust functionality to "
     "available protocol features. If set to false, or the "
     "ApiVersionRequest fails, the fallback version "
     "`broker.version.fallback` will be used. "
     "**NOTE**: Depends on broker version >=0.10.0. If the request is not "
     "supported by (an older) broker the `broker.version.fallback` fallback is "
     "used.",
     0, 1, 1},
    {_RK_GLOBAL, "api.version.request.timeout.ms", _RK_C_INT,
     _RK(api_version_request_timeout_ms),
     "Timeout for broker API version requests.", 1, 5 * 60 * 1000, 10 * 1000},
    {_RK_GLOBAL | _RK_MED, "api.version.fallback.ms", _RK_C_INT,
     _RK(api_version_fallback_ms),
     "Dictates how long the `broker.version.fallback` fallback is used "
     "in the case the ApiVersionRequest fails. "
     "**NOTE**: The ApiVersionRequest is only issued when a new connection "
     "to the broker is made (such as after an upgrade).",
     0, 86400 * 7 * 1000, 0},

    {_RK_GLOBAL | _RK_MED, "broker.version.fallback", _RK_C_STR,
     _RK(broker_version_fallback),
     "Older broker versions (before 0.10.0) provide no way for a client to "
     "query "
     "for supported protocol features "
     "(ApiVersionRequest, see `api.version.request`) making it impossible "
     "for the client to know what features it may use. "
     "As a workaround a user may set this property to the expected broker "
     "version and the client will automatically adjust its feature set "
     "accordingly if the ApiVersionRequest fails (or is disabled). "
     "The fallback broker version will be used for `api.version.fallback.ms`. "
     "Valid values are: 0.9.0, 0.8.2, 0.8.1, 0.8.0. "
     "Any other value >= 0.10, such as 0.10.2.1, "
     "enables ApiVersionRequests.",
     .sdef = "0.10.0", .validate = rd_kafka_conf_validate_broker_version},
    {_RK_GLOBAL, "allow.auto.create.topics", _RK_C_BOOL,
     _RK(allow_auto_create_topics),
     "Allow automatic topic creation on the broker when subscribing to "
     "or assigning non-existent topics. "
     "The broker must also be configured with "
     "`auto.create.topics.enable=true` for this configuration to "
     "take effect. "
     "Note: the default value (true) for the producer is "
     "different from the default value (false) for the consumer. "
     "Further, the consumer default value is different from the Java "
     "consumer (true), and this property is not supported by the Java "
     "producer. Requires broker version >= 0.11.0.0, for older broker "
     "versions only the broker configuration applies.",
     0, 1, 0},

    /* Security related global properties */
    {_RK_GLOBAL | _RK_HIGH, "security.protocol", _RK_C_S2I,
     _RK(security_protocol), "Protocol used to communicate with brokers.",
     .vdef = RD_KAFKA_PROTO_PLAINTEXT,
     .s2i  = {{RD_KAFKA_PROTO_PLAINTEXT, "plaintext"},
             {RD_KAFKA_PROTO_SSL, "ssl", _UNSUPPORTED_SSL},
             {RD_KAFKA_PROTO_SASL_PLAINTEXT, "sasl_plaintext"},
             {RD_KAFKA_PROTO_SASL_SSL, "sasl_ssl", _UNSUPPORTED_SSL},
             {0, NULL}}},

    {_RK_GLOBAL, "ssl.cipher.suites", _RK_C_STR, _RK(ssl.cipher_suites),
     "A cipher suite is a named combination of authentication, "
     "encryption, MAC and key exchange algorithm used to negotiate the "
     "security settings for a network connection using TLS or SSL network "
     "protocol. See manual page for `ciphers(1)` and "
     "`SSL_CTX_set_cipher_list(3).",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.curves.list", _RK_C_STR, _RK(ssl.curves_list),
     "The supported-curves extension in the TLS ClientHello message specifies "
     "the curves (standard/named, or 'explicit' GF(2^k) or GF(p)) the client "
     "is willing to have the server use. See manual page for "
     "`SSL_CTX_set1_curves_list(3)`. OpenSSL >= 1.0.2 required.",
     _UNSUPPORTED_OPENSSL_1_0_2},
    {_RK_GLOBAL, "ssl.sigalgs.list", _RK_C_STR, _RK(ssl.sigalgs_list),
     "The client uses the TLS ClientHello signature_algorithms extension "
     "to indicate to the server which signature/hash algorithm pairs "
     "may be used in digital signatures. See manual page for "
     "`SSL_CTX_set1_sigalgs_list(3)`. OpenSSL >= 1.0.2 required.",
     _UNSUPPORTED_OPENSSL_1_0_2},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl.key.location", _RK_C_STR,
     _RK(ssl.key_location),
     "Path to client's private key (PEM) used for authentication.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl.key.password", _RK_C_STR,
     _RK(ssl.key_password),
     "Private key passphrase (for use with `ssl.key.location` "
     "and `set_ssl_cert()`)",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl.key.pem", _RK_C_STR, _RK(ssl.key_pem),
     "Client's private key string (PEM format) used for authentication.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl_key", _RK_C_INTERNAL, _RK(ssl.key),
     "Client's private key as set by rd_kafka_conf_set_ssl_cert()",
     .dtor = rd_kafka_conf_cert_dtor, .copy = rd_kafka_conf_cert_copy,
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.certificate.location", _RK_C_STR, _RK(ssl.cert_location),
     "Path to client's public key (PEM) used for authentication.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.certificate.pem", _RK_C_STR, _RK(ssl.cert_pem),
     "Client's public key string (PEM format) used for authentication.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl_certificate", _RK_C_INTERNAL, _RK(ssl.key),
     "Client's public key as set by rd_kafka_conf_set_ssl_cert()",
     .dtor = rd_kafka_conf_cert_dtor, .copy = rd_kafka_conf_cert_copy,
     _UNSUPPORTED_SSL},

    {_RK_GLOBAL, "ssl.ca.location", _RK_C_STR, _RK(ssl.ca_location),
     "File or directory path to CA certificate(s) for verifying "
     "the broker's key. "
     "Defaults: "
     "On Windows the system's CA certificates are automatically looked "
     "up in the Windows Root certificate store. "
     "On Mac OSX this configuration defaults to `probe`. "
     "It is recommended to install openssl using Homebrew, "
     "to provide CA certificates. "
     "On Linux install the distribution's ca-certificates package. "
     "If OpenSSL is statically linked or `ssl.ca.location` is set to "
     "`probe` a list of standard paths will be probed and the first one "
     "found will be used as the default CA certificate location path. "
     "If OpenSSL is dynamically linked the OpenSSL library's default "
     "path will be used (see `OPENSSLDIR` in `openssl version -a`).",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl.ca.pem", _RK_C_STR, _RK(ssl.ca_pem),
     "CA certificate string (PEM format) for verifying the broker's key.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl_ca", _RK_C_INTERNAL, _RK(ssl.ca),
     "CA certificate as set by rd_kafka_conf_set_ssl_cert()",
     .dtor = rd_kafka_conf_cert_dtor, .copy = rd_kafka_conf_cert_copy,
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.ca.certificate.stores", _RK_C_STR,
     _RK(ssl.ca_cert_stores),
     "Comma-separated list of Windows Certificate stores to load "
     "CA certificates from. Certificates will be loaded in the same "
     "order as stores are specified. If no certificates can be loaded "
     "from any of the specified stores an error is logged and the "
     "OpenSSL library's default CA location is used instead. "
     "Store names are typically one or more of: MY, Root, Trust, CA.",
     .sdef = "Root",
#if !defined(_WIN32)
     .unsupported = "configuration only valid on Windows"
#endif
    },

    {_RK_GLOBAL, "ssl.crl.location", _RK_C_STR, _RK(ssl.crl_location),
     "Path to CRL for verifying broker's certificate validity.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.keystore.location", _RK_C_STR, _RK(ssl.keystore_location),
     "Path to client's keystore (PKCS#12) used for authentication.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL | _RK_SENSITIVE, "ssl.keystore.password", _RK_C_STR,
     _RK(ssl.keystore_password), "Client's keystore (PKCS#12) password.",
     _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.providers", _RK_C_STR, _RK(ssl.providers),
     "Comma-separated list of OpenSSL 3.0.x implementation providers. "
     "E.g., \"default,legacy\".",
     _UNSUPPORTED_SSL_3},
    {_RK_GLOBAL | _RK_DEPRECATED, "ssl.engine.location", _RK_C_STR,
     _RK(ssl.engine_location),
     "Path to OpenSSL engine library. OpenSSL >= 1.1.x required. "
     "DEPRECATED: OpenSSL engine support is deprecated and should be "
     "replaced by OpenSSL 3 providers.",
     _UNSUPPORTED_SSL_ENGINE},
    {_RK_GLOBAL, "ssl.engine.id", _RK_C_STR, _RK(ssl.engine_id),
     "OpenSSL engine id is the name used for loading engine.",
     .sdef = "dynamic", _UNSUPPORTED_SSL_ENGINE},
    {_RK_GLOBAL, "ssl_engine_callback_data", _RK_C_PTR,
     _RK(ssl.engine_callback_data),
     "OpenSSL engine callback data (set "
     "with rd_kafka_conf_set_engine_callback_data()).",
     _UNSUPPORTED_SSL_ENGINE},
    {_RK_GLOBAL, "enable.ssl.certificate.verification", _RK_C_BOOL,
     _RK(ssl.enable_verify),
     "Enable OpenSSL's builtin broker (server) certificate verification. "
     "This verification can be extended by the application by "
     "implementing a certificate_verify_cb.",
     0, 1, 1, _UNSUPPORTED_SSL},
    {_RK_GLOBAL, "ssl.endpoint.identification.algorithm", _RK_C_S2I,
     _RK(ssl.endpoint_identification),
     "Endpoint identification algorithm to validate broker "
     "hostname using broker certificate. "
     "https - Server (broker) hostname verification as "
     "specified in RFC2818. "
     "none - No endpoint verification. "
     "OpenSSL >= 1.0.2 required.",
     .vdef = RD_KAFKA_SSL_ENDPOINT_ID_HTTPS,
     .s2i  = {{RD_KAFKA_SSL_ENDPOINT_ID_NONE, "none"},
             {RD_KAFKA_SSL_ENDPOINT_ID_HTTPS, "https"}},
     _UNSUPPORTED_OPENSSL_1_0_2},
    {_RK_GLOBAL, "ssl.certificate.verify_cb", _RK_C_PTR,
     _RK(ssl.cert_verify_cb),
     "Callback to verify the broker certificate chain.", _UNSUPPORTED_SSL},

    /* Point user in the right direction if they try to apply
     * Java client SSL / JAAS properties. */
    {_RK_GLOBAL, "ssl.truststore.location", _RK_C_INVALID, _RK(dummy),
     "Java TrustStores are not supported, use `ssl.ca.location` "
     "and a certificate file instead. "
     "See "
     "https://github.com/edenhill/librdkafka/wiki/Using-SSL-with-librdkafka "
     "for more information."},
    {_RK_GLOBAL, "sasl.jaas.config", _RK_C_INVALID, _RK(dummy),
     "Java JAAS configuration is not supported, see "
     "https://github.com/edenhill/librdkafka/wiki/Using-SASL-with-librdkafka "
     "for more information."},

    {_RK_GLOBAL | _RK_HIGH, "sasl.mechanisms", _RK_C_STR, _RK(sasl.mechanisms),
     "SASL mechanism to use for authentication. "
     "Supported: GSSAPI, PLAIN, SCRAM-SHA-256, SCRAM-SHA-512, OAUTHBEARER. "
     "**NOTE**: Despite the name only one mechanism must be configured.",
     .sdef = "GSSAPI", .validate = rd_kafka_conf_validate_single},
    {_RK_GLOBAL | _RK_HIGH, "sasl.mechanism", _RK_C_ALIAS,
     .sdef = "sasl.mechanisms"},
    {_RK_GLOBAL, "sasl.kerberos.service.name", _RK_C_STR,
     _RK(sasl.service_name),
     "Kerberos principal name that Kafka runs as, "
     "not including /hostname@REALM",
     .sdef = "kafka"},
    {_RK_GLOBAL, "sasl.kerberos.principal", _RK_C_STR, _RK(sasl.principal),
     "This client's Kerberos principal name. "
     "(Not supported on Windows, will use the logon user's principal).",
     .sdef = "kafkaclient"},
    {_RK_GLOBAL, "sasl.kerberos.kinit.cmd", _RK_C_STR, _RK(sasl.kinit_cmd),
     "Shell command to refresh or acquire the client's Kerberos ticket. "
     "This command is executed on client creation and every "
     "sasl.kerberos.min.time.before.relogin (0=disable). "
     "%{config.prop.name} is replaced by corresponding config "
     "object value.",
     .sdef =
         /* First attempt to refresh, else acquire. */
     "kinit -R -t \"%{sasl.kerberos.keytab}\" "
     "-k %{sasl.kerberos.principal} || "
     "kinit -t \"%{sasl.kerberos.keytab}\" -k %{sasl.kerberos.principal}",
     _UNSUPPORTED_WIN32_GSSAPI},
    {_RK_GLOBAL, "sasl.kerberos.keytab", _RK_C_STR, _RK(sasl.keytab),
     "Path to Kerberos keytab file. "
     "This configuration property is only used as a variable in "
     "`sasl.kerberos.kinit.cmd` as "
     "` ... -t \"%{sasl.kerberos.keytab}\"`.",
     _UNSUPPORTED_WIN32_GSSAPI},
    {_RK_GLOBAL, "sasl.kerberos.min.time.before.relogin", _RK_C_INT,
     _RK(sasl.relogin_min_time),
     "Minimum time in milliseconds between key refresh attempts. "
     "Disable automatic key refresh by setting this property to 0.",
     0, 86400 * 1000, 60 * 1000, _UNSUPPORTED_WIN32_GSSAPI},
    {_RK_GLOBAL | _RK_HIGH | _RK_SENSITIVE, "sasl.username", _RK_C_STR,
     _RK(sasl.username),
     "SASL username for use with the PLAIN and SASL-SCRAM-.. mechanisms"},
    {_RK_GLOBAL | _RK_HIGH | _RK_SENSITIVE, "sasl.password", _RK_C_STR,
     _RK(sasl.password),
     "SASL password for use with the PLAIN and SASL-SCRAM-.. mechanism"},
    {_RK_GLOBAL | _RK_SENSITIVE, "sasl.oauthbearer.config", _RK_C_STR,
     _RK(sasl.oauthbearer_config),
     "SASL/OAUTHBEARER configuration. The format is "
     "implementation-dependent and must be parsed accordingly. The "
     "default unsecured token implementation (see "
     "https://tools.ietf.org/html/rfc7515#appendix-A.5) recognizes "
     "space-separated name=value pairs with valid names including "
     "principalClaimName, principal, scopeClaimName, scope, and "
     "lifeSeconds. The default value for principalClaimName is \"sub\", "
     "the default value for scopeClaimName is \"scope\", and the default "
     "value for lifeSeconds is 3600. The scope value is CSV format with "
     "the default value being no/empty scope. For example: "
     "`principalClaimName=azp principal=admin scopeClaimName=roles "
     "scope=role1,role2 lifeSeconds=600`. In addition, SASL extensions "
     "can be communicated to the broker via "
     "`extension_NAME=value`. For example: "
     "`principal=admin extension_traceId=123`",
     _UNSUPPORTED_OAUTHBEARER},
    {_RK_GLOBAL, "enable.sasl.oauthbearer.unsecure.jwt", _RK_C_BOOL,
     _RK(sasl.enable_oauthbearer_unsecure_jwt),
     "Enable the builtin unsecure JWT OAUTHBEARER token handler "
     "if no oauthbearer_refresh_cb has been set. "
     "This builtin handler should only be used for development "
     "or testing, and not in production.",
     0, 1, 0, _UNSUPPORTED_OAUTHBEARER},
    {_RK_GLOBAL, "oauthbearer_token_refresh_cb", _RK_C_PTR,
     _RK(sasl.oauthbearer.token_refresh_cb),
     "SASL/OAUTHBEARER token refresh callback (set with "
     "rd_kafka_conf_set_oauthbearer_token_refresh_cb(), triggered by "
     "rd_kafka_poll(), et.al. "
     "This callback will be triggered when it is time to refresh "
     "the client's OAUTHBEARER token. "
     "Also see `rd_kafka_conf_enable_sasl_queue()`.",
     _UNSUPPORTED_OAUTHBEARER},
    {
        _RK_GLOBAL | _RK_HIDDEN,
        "enable_sasl_queue",
        _RK_C_BOOL,
        _RK(sasl.enable_callback_queue),
        "Enable the SASL callback queue "
        "(set with rd_kafka_conf_enable_sasl_queue()).",
        0,
        1,
        0,
    },
    {_RK_GLOBAL, "sasl.oauthbearer.method", _RK_C_S2I,
     _RK(sasl.oauthbearer.method),
     "Set to \"default\" or \"oidc\" to control which login method "
     "to be used. If set to \"oidc\", the following properties must also be "
     "be specified: "
     "`sasl.oauthbearer.client.id`, `sasl.oauthbearer.client.secret`, "
     "and `sasl.oauthbearer.token.endpoint.url`.",
     .vdef = RD_KAFKA_SASL_OAUTHBEARER_METHOD_DEFAULT,
     .s2i  = {{RD_KAFKA_SASL_OAUTHBEARER_METHOD_DEFAULT, "default"},
             {RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC, "oidc"}},
     _UNSUPPORTED_OIDC},
    {_RK_GLOBAL, "sasl.oauthbearer.client.id", _RK_C_STR,
     _RK(sasl.oauthbearer.client_id),
     "Public identifier for the application. "
     "Must be unique across all clients that the "
     "authorization server handles. "
     "Only used when `sasl.oauthbearer.method` is set to \"oidc\".",
     _UNSUPPORTED_OIDC},
    {_RK_GLOBAL, "sasl.oauthbearer.client.secret", _RK_C_STR,
     _RK(sasl.oauthbearer.client_secret),
     "Client secret only known to the application and the "
     "authorization server. This should be a sufficiently random string "
     "that is not guessable. "
     "Only used when `sasl.oauthbearer.method` is set to \"oidc\".",
     _UNSUPPORTED_OIDC},
    {_RK_GLOBAL, "sasl.oauthbearer.scope", _RK_C_STR,
     _RK(sasl.oauthbearer.scope),
     "Client use this to specify the scope of the access request to the "
     "broker. "
     "Only used when `sasl.oauthbearer.method` is set to \"oidc\".",
     _UNSUPPORTED_OIDC},
    {_RK_GLOBAL, "sasl.oauthbearer.extensions", _RK_C_STR,
     _RK(sasl.oauthbearer.extensions_str),
     "Allow additional information to be provided to the broker. "
     "Comma-separated list of key=value pairs. "
     "E.g., \"supportFeatureX=true,organizationId=sales-emea\"."
     "Only used when `sasl.oauthbearer.method` is set to \"oidc\".",
     _UNSUPPORTED_OIDC},
    {_RK_GLOBAL, "sasl.oauthbearer.token.endpoint.url", _RK_C_STR,
     _RK(sasl.oauthbearer.token_endpoint_url),
     "OAuth/OIDC issuer token endpoint HTTP(S) URI used to retrieve token. "
     "Only used when `sasl.oauthbearer.method` is set to \"oidc\".",
     _UNSUPPORTED_OIDC},

    /* Plugins */
    {_RK_GLOBAL, "plugin.library.paths", _RK_C_STR, _RK(plugin_paths),
     "List of plugin libraries to load (; separated). "
     "The library search path is platform dependent (see dlopen(3) for "
     "Unix and LoadLibrary() for Windows). If no filename extension is "
     "specified the platform-specific extension (such as .dll or .so) "
     "will be appended automatically.",
#if WITH_PLUGINS
     .set = rd_kafka_plugins_conf_set
#else
          .unsupported = "libdl/dlopen(3) not available at build time"
#endif
    },

    /* Interceptors are added through specific API and not exposed
     * as configuration properties.
     * The interceptor property must be defined after plugin.library.paths
     * so that the plugin libraries are properly loaded before
     * interceptors are configured when duplicating configuration objects.*/
    {_RK_GLOBAL, "interceptors", _RK_C_INTERNAL, _RK(interceptors),
     "Interceptors added through rd_kafka_conf_interceptor_add_..() "
     "and any configuration handled by interceptors.",
     .ctor = rd_kafka_conf_interceptor_ctor,
     .dtor = rd_kafka_conf_interceptor_dtor,
     .copy = rd_kafka_conf_interceptor_copy},

    /* Test mocks. */
    {_RK_GLOBAL | _RK_HIDDEN, "test.mock.num.brokers", _RK_C_INT,
     _RK(mock.broker_cnt),
     "Number of mock brokers to create. "
     "This will automatically overwrite `bootstrap.servers` with the "
     "mock broker list.",
     0, 10000, 0},
    {_RK_GLOBAL | _RK_HIDDEN, "test.mock.broker.rtt", _RK_C_INT,
     _RK(mock.broker_rtt), "Simulated mock broker latency in milliseconds.", 0,
     60 * 60 * 1000 /*1h*/, 0},

    /* Unit test interfaces.
     * These are not part of the public API and may change at any time.
     * Only to be used by the librdkafka tests. */
    {_RK_GLOBAL | _RK_HIDDEN, "ut_handle_ProduceResponse", _RK_C_PTR,
     _RK(ut.handle_ProduceResponse),
     "ProduceResponse handler: "
     "rd_kafka_resp_err_t (*cb) (rd_kafka_t *rk, "
     "int32_t brokerid, uint64_t msgid, rd_kafka_resp_err_t err)"},

    /* Global consumer group properties */
    {_RK_GLOBAL | _RK_CGRP | _RK_HIGH, "group.id", _RK_C_STR, _RK(group_id_str),
     "Client group id string. All clients sharing the same group.id "
     "belong to the same group."},
    {_RK_GLOBAL | _RK_CGRP | _RK_MED, "group.instance.id", _RK_C_STR,
     _RK(group_instance_id),
     "Enable static group membership. "
     "Static group members are able to leave and rejoin a group "
     "within the configured `session.timeout.ms` without prompting a "
     "group rebalance. This should be used in combination with a larger "
     "`session.timeout.ms` to avoid group rebalances caused by transient "
     "unavailability (e.g. process restarts). "
     "Requires broker version >= 2.3.0."},
    {_RK_GLOBAL | _RK_CGRP | _RK_MED, "partition.assignment.strategy",
     _RK_C_STR, _RK(partition_assignment_strategy),
     "The name of one or more partition assignment strategies. The "
     "elected group leader will use a strategy supported by all "
     "members of the group to assign partitions to group members. If "
     "there is more than one eligible strategy, preference is "
     "determined by the order of this list (strategies earlier in the "
     "list have higher priority). "
     "Cooperative and non-cooperative (eager) strategies must not be "
     "mixed. "
     "Available strategies: range, roundrobin, cooperative-sticky.",
     .sdef = "range,roundrobin"},
    {_RK_GLOBAL | _RK_CGRP | _RK_HIGH, "session.timeout.ms", _RK_C_INT,
     _RK(group_session_timeout_ms),
     "Client group session and failure detection timeout. "
     "The consumer sends periodic heartbeats (heartbeat.interval.ms) "
     "to indicate its liveness to the broker. If no hearts are "
     "received by the broker for a group member within the "
     "session timeout, the broker will remove the consumer from "
     "the group and trigger a rebalance. "
     "The allowed range is configured with the **broker** configuration "
     "properties `group.min.session.timeout.ms` and "
     "`group.max.session.timeout.ms`. "
     "Also see `max.poll.interval.ms`.",
     1, 3600 * 1000, 45 * 1000},
    {_RK_GLOBAL | _RK_CGRP, "heartbeat.interval.ms", _RK_C_INT,
     _RK(group_heartbeat_intvl_ms),
     "Group session keepalive heartbeat interval.", 1, 3600 * 1000, 3 * 1000},
    {_RK_GLOBAL | _RK_CGRP, "group.protocol.type", _RK_C_KSTR,
     _RK(group_protocol_type),
     "Group protocol type. NOTE: Currently, the only supported group "
     "protocol type is `consumer`.",
     .sdef = "consumer"},
    {_RK_GLOBAL | _RK_CGRP, "coordinator.query.interval.ms", _RK_C_INT,
     _RK(coord_query_intvl_ms),
     "How often to query for the current client group coordinator. "
     "If the currently assigned coordinator is down the configured "
     "query interval will be divided by ten to more quickly recover "
     "in case of coordinator reassignment.",
     1, 3600 * 1000, 10 * 60 * 1000},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_HIGH, "max.poll.interval.ms", _RK_C_INT,
     _RK(max_poll_interval_ms),
     "Maximum allowed time between calls to consume messages "
     "(e.g., rd_kafka_consumer_poll()) for high-level consumers. "
     "If this interval is exceeded the consumer is considered failed "
     "and the group will rebalance in order to reassign the "
     "partitions to another consumer group member. "
     "Warning: Offset commits may be not possible at this point. "
     "Note: It is recommended to set `enable.auto.offset.store=false` "
     "for long-time processing applications and then explicitly store "
     "offsets (using offsets_store()) *after* message processing, to "
     "make sure offsets are not auto-committed prior to processing "
     "has finished. "
     "The interval is checked two times per second. "
     "See KIP-62 for more information.",
     1, 86400 * 1000, 300000},

    /* Global consumer properties */
    {_RK_GLOBAL | _RK_CONSUMER | _RK_HIGH, "enable.auto.commit", _RK_C_BOOL,
     _RK(enable_auto_commit),
     "Automatically and periodically commit offsets in the background. "
     "Note: setting this to false does not prevent the consumer from "
     "fetching previously committed start offsets. To circumvent this "
     "behaviour set specific start offsets per partition in the call "
     "to assign().",
     0, 1, 1},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "auto.commit.interval.ms", _RK_C_INT,
     _RK(auto_commit_interval_ms),
     "The frequency in milliseconds that the consumer offsets "
     "are committed (written) to offset storage. (0 = disable). "
     "This setting is used by the high-level consumer.",
     0, 86400 * 1000, 5 * 1000},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_HIGH, "enable.auto.offset.store",
     _RK_C_BOOL, _RK(enable_auto_offset_store),
     "Automatically store offset of last message provided to "
     "application. "
     "The offset store is an in-memory store of the next offset to "
     "(auto-)commit for each partition.",
     0, 1, 1},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "queued.min.messages", _RK_C_INT,
     _RK(queued_min_msgs),
     "Minimum number of messages per topic+partition "
     "librdkafka tries to maintain in the local consumer queue.",
     1, 10000000, 100000},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "queued.max.messages.kbytes",
     _RK_C_INT, _RK(queued_max_msg_kbytes),
     "Maximum number of kilobytes of queued pre-fetched messages "
     "in the local consumer queue. "
     "If using the high-level consumer this setting applies to the "
     "single consumer queue, regardless of the number of partitions. "
     "When using the legacy simple consumer or when separate "
     "partition queues are used this setting applies per partition. "
     "This value may be overshot by fetch.message.max.bytes. "
     "This property has higher priority than queued.min.messages.",
     1, INT_MAX / 1024, 0x10000 /*64MB*/},
    {_RK_GLOBAL | _RK_CONSUMER, "fetch.wait.max.ms", _RK_C_INT,
     _RK(fetch_wait_max_ms),
     "Maximum time the broker may wait to fill the Fetch response "
     "with fetch.min.bytes of messages.",
     0, 300 * 1000, 500},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "fetch.message.max.bytes", _RK_C_INT,
     _RK(fetch_msg_max_bytes),
     "Initial maximum number of bytes per topic+partition to request when "
     "fetching messages from the broker. "
     "If the client encounters a message larger than this value "
     "it will gradually try to increase it until the "
     "entire message can be fetched.",
     1, 1000000000, 1024 * 1024},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "max.partition.fetch.bytes",
     _RK_C_ALIAS, .sdef = "fetch.message.max.bytes"},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "fetch.max.bytes", _RK_C_INT,
     _RK(fetch_max_bytes),
     "Maximum amount of data the broker shall return for a Fetch request. "
     "Messages are fetched in batches by the consumer and if the first "
     "message batch in the first non-empty partition of the Fetch request "
     "is larger than this value, then the message batch will still be "
     "returned to ensure the consumer can make progress. "
     "The maximum message batch size accepted by the broker is defined "
     "via `message.max.bytes` (broker config) or "
     "`max.message.bytes` (broker topic config). "
     "`fetch.max.bytes` is automatically adjusted upwards to be "
     "at least `message.max.bytes` (consumer config).",
     0, INT_MAX - 512, 50 * 1024 * 1024 /* 50MB */},
    {_RK_GLOBAL | _RK_CONSUMER, "fetch.min.bytes", _RK_C_INT,
     _RK(fetch_min_bytes),
     "Minimum number of bytes the broker responds with. "
     "If fetch.wait.max.ms expires the accumulated data will "
     "be sent to the client regardless of this setting.",
     1, 100000000, 1},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "fetch.error.backoff.ms", _RK_C_INT,
     _RK(fetch_error_backoff_ms),
     "How long to postpone the next fetch request for a "
     "topic+partition in case of a fetch error.",
     0, 300 * 1000, 500},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_DEPRECATED, "offset.store.method",
     _RK_C_S2I, _RK(offset_store_method),
     "Offset commit store method: "
     "'file' - DEPRECATED: local file store (offset.store.path, et.al), "
     "'broker' - broker commit store "
     "(requires Apache Kafka 0.8.2 or later on the broker).",
     .vdef = RD_KAFKA_OFFSET_METHOD_BROKER,
     .s2i  = {{RD_KAFKA_OFFSET_METHOD_NONE, "none"},
             {RD_KAFKA_OFFSET_METHOD_FILE, "file"},
             {RD_KAFKA_OFFSET_METHOD_BROKER, "broker"}}},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_HIGH, "isolation.level", _RK_C_S2I,
     _RK(isolation_level),
     "Controls how to read messages written transactionally: "
     "`read_committed` - only return transactional messages which have "
     "been committed. `read_uncommitted` - return all messages, even "
     "transactional messages which have been aborted.",
     .vdef = RD_KAFKA_READ_COMMITTED,
     .s2i  = {{RD_KAFKA_READ_UNCOMMITTED, "read_uncommitted"},
             {RD_KAFKA_READ_COMMITTED, "read_committed"}}},
    {_RK_GLOBAL | _RK_CONSUMER, "consume_cb", _RK_C_PTR, _RK(consume_cb),
     "Message consume callback (set with rd_kafka_conf_set_consume_cb())"},
    {_RK_GLOBAL | _RK_CONSUMER, "rebalance_cb", _RK_C_PTR, _RK(rebalance_cb),
     "Called after consumer group has been rebalanced "
     "(set with rd_kafka_conf_set_rebalance_cb())"},
    {_RK_GLOBAL | _RK_CONSUMER, "offset_commit_cb", _RK_C_PTR,
     _RK(offset_commit_cb),
     "Offset commit result propagation callback. "
     "(set with rd_kafka_conf_set_offset_commit_cb())"},
    {_RK_GLOBAL | _RK_CONSUMER, "enable.partition.eof", _RK_C_BOOL,
     _RK(enable_partition_eof),
     "Emit RD_KAFKA_RESP_ERR__PARTITION_EOF event whenever the "
     "consumer reaches the end of a partition.",
     0, 1, 0},
    {_RK_GLOBAL | _RK_CONSUMER | _RK_MED, "check.crcs", _RK_C_BOOL,
     _RK(check_crcs),
     "Verify CRC32 of consumed messages, ensuring no on-the-wire or "
     "on-disk corruption to the messages occurred. This check comes "
     "at slightly increased CPU usage.",
     0, 1, 0},
    {_RK_GLOBAL, "client.rack", _RK_C_KSTR, _RK(client_rack),
     "A rack identifier for this client. This can be any string value "
     "which indicates where this client is physically located. It "
     "corresponds with the broker config `broker.rack`.",
     .sdef = ""},

    /* Global producer properties */
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "transactional.id", _RK_C_STR,
     _RK(eos.transactional_id),
     "Enables the transactional producer. "
     "The transactional.id is used to identify the same transactional "
     "producer instance across process restarts. "
     "It allows the producer to guarantee that transactions corresponding "
     "to earlier instances of the same producer have been finalized "
     "prior to starting any new transactions, and that any "
     "zombie instances are fenced off. "
     "If no transactional.id is provided, then the producer is limited "
     "to idempotent delivery (if enable.idempotence is set). "
     "Requires broker version >= 0.11.0."},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "transaction.timeout.ms", _RK_C_INT,
     _RK(eos.transaction_timeout_ms),
     "The maximum amount of time in milliseconds that the transaction "
     "coordinator will wait for a transaction status update from the "
     "producer before proactively aborting the ongoing transaction. "
     "If this value is larger than the `transaction.max.timeout.ms` "
     "setting in the broker, the init_transactions() call will fail with "
     "ERR_INVALID_TRANSACTION_TIMEOUT. "
     "The transaction timeout automatically adjusts "
     "`message.timeout.ms` and `socket.timeout.ms`, unless explicitly "
     "configured in which case they must not exceed the "
     "transaction timeout (`socket.timeout.ms` must be at least 100ms "
     "lower than `transaction.timeout.ms`). "
     "This is also the default timeout value if no timeout (-1) is "
     "supplied to the transactional API methods.",
     1000, INT_MAX, 60000},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "enable.idempotence", _RK_C_BOOL,
     _RK(eos.idempotence),
     "When set to `true`, the producer will ensure that messages are "
     "successfully produced exactly once and in the original produce "
     "order. "
     "The following configuration properties are adjusted automatically "
     "(if not modified by the user) when idempotence is enabled: "
     "`max.in.flight.requests.per.connection=" RD_KAFKA_IDEMP_MAX_INFLIGHT_STR
     "` (must be less than or "
     "equal to " RD_KAFKA_IDEMP_MAX_INFLIGHT_STR "), `retries=INT32_MAX` "
     "(must be greater than 0), `acks=all`, `queuing.strategy=fifo`. "
     "Producer instantation will fail if user-supplied configuration "
     "is incompatible.",
     0, 1, 0},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_EXPERIMENTAL, "enable.gapless.guarantee",
     _RK_C_BOOL, _RK(eos.gapless),
     "When set to `true`, any error that could result in a gap "
     "in the produced message series when a batch of messages fails, "
     "will raise a fatal error (ERR__GAPLESS_GUARANTEE) and stop "
     "the producer. "
     "Messages failing due to `message.timeout.ms` are not covered "
     "by this guarantee. "
     "Requires `enable.idempotence=true`.",
     0, 1, 0},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "queue.buffering.max.messages",
     _RK_C_INT, _RK(queue_buffering_max_msgs),
     "Maximum number of messages allowed on the producer queue. "
     "This queue is shared by all topics and partitions. A value of 0 disables "
     "this limit.",
     0, INT_MAX, 100000},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "queue.buffering.max.kbytes",
     _RK_C_INT, _RK(queue_buffering_max_kbytes),
     "Maximum total message size sum allowed on the producer queue. "
     "This queue is shared by all topics and partitions. "
     "This property has higher priority than queue.buffering.max.messages.",
     1, INT_MAX, 0x100000 /*1GB*/},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "queue.buffering.max.ms", _RK_C_DBL,
     _RK(buffering_max_ms_dbl),
     "Delay in milliseconds to wait for messages in the producer queue "
     "to accumulate before constructing message batches (MessageSets) to "
     "transmit to brokers. "
     "A higher value allows larger and more effective "
     "(less overhead, improved compression) batches of messages to "
     "accumulate at the expense of increased message delivery latency.",
     .dmin = 0, .dmax = 900.0 * 1000.0, .ddef = 5.0},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "linger.ms", _RK_C_ALIAS,
     .sdef = "queue.buffering.max.ms"},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_HIGH, "message.send.max.retries",
     _RK_C_INT, _RK(max_retries),
     "How many times to retry sending a failing Message. "
     "**Note:** retrying may cause reordering unless "
     "`enable.idempotence` is set to true.",
     0, INT32_MAX, INT32_MAX},
    {_RK_GLOBAL | _RK_PRODUCER, "retries", _RK_C_ALIAS,
     .sdef = "message.send.max.retries"},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "retry.backoff.ms", _RK_C_INT,
     _RK(retry_backoff_ms),
     "The backoff time in milliseconds before retrying a protocol request.", 1,
     300 * 1000, 100},

    {_RK_GLOBAL | _RK_PRODUCER, "queue.buffering.backpressure.threshold",
     _RK_C_INT, _RK(queue_backpressure_thres),
     "The threshold of outstanding not yet transmitted broker requests "
     "needed to backpressure the producer's message accumulator. "
     "If the number of not yet transmitted requests equals or exceeds "
     "this number, produce request creation that would have otherwise "
     "been triggered (for example, in accordance with linger.ms) will be "
     "delayed. A lower number yields larger and more effective batches. "
     "A higher value can improve latency when using compression on slow "
     "machines.",
     1, 1000000, 1},

    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "compression.codec", _RK_C_S2I,
     _RK(compression_codec),
     "compression codec to use for compressing message sets. "
     "This is the default value for all topics, may be overridden by "
     "the topic configuration property `compression.codec`. ",
     .vdef = RD_KAFKA_COMPRESSION_NONE,
     .s2i  = {{RD_KAFKA_COMPRESSION_NONE, "none"},
             {RD_KAFKA_COMPRESSION_GZIP, "gzip", _UNSUPPORTED_ZLIB},
             {RD_KAFKA_COMPRESSION_SNAPPY, "snappy", _UNSUPPORTED_SNAPPY},
             {RD_KAFKA_COMPRESSION_KLZ4, "lz4"},
             {RD_KAFKA_COMPRESSION_ZSTD, "zstd", _UNSUPPORTED_ZSTD},
             {0}}},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "compression.type", _RK_C_ALIAS,
     .sdef = "compression.codec"},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "batch.num.messages", _RK_C_INT,
     _RK(batch_num_messages),
     "Maximum number of messages batched in one MessageSet. "
     "The total MessageSet size is also limited by batch.size and "
     "message.max.bytes.",
     1, 1000000, 10000},
    {_RK_GLOBAL | _RK_PRODUCER | _RK_MED, "batch.size", _RK_C_INT,
     _RK(batch_size),
     "Maximum size (in bytes) of all messages batched in one MessageSet, "
     "including protocol framing overhead. "
     "This limit is applied after the first message has been added "
     "to the batch, regardless of the first message's size, this is to "
     "ensure that messages that exceed batch.size are produced. "
     "The total MessageSet size is also limited by batch.num.messages and "
     "message.max.bytes.",
     1, INT_MAX, 1000000},
    {_RK_GLOBAL | _RK_PRODUCER, "delivery.report.only.error", _RK_C_BOOL,
     _RK(dr_err_only), "Only provide delivery reports for failed messages.", 0,
     1, 0},
    {_RK_GLOBAL | _RK_PRODUCER, "dr_cb", _RK_C_PTR, _RK(dr_cb),
     "Delivery report callback (set with rd_kafka_conf_set_dr_cb())"},
    {_RK_GLOBAL | _RK_PRODUCER, "dr_msg_cb", _RK_C_PTR, _RK(dr_msg_cb),
     "Delivery report callback (set with rd_kafka_conf_set_dr_msg_cb())"},
    {_RK_GLOBAL | _RK_PRODUCER, "sticky.partitioning.linger.ms", _RK_C_INT,
     _RK(sticky_partition_linger_ms),
     "Delay in milliseconds to wait to assign new sticky partitions for "
     "each topic. "
     "By default, set to double the time of linger.ms. To disable sticky "
     "behavior, set to 0. "
     "This behavior affects messages with the key NULL in all cases, and "
     "messages with key lengths of zero when the consistent_random "
     "partitioner is in use. "
     "These messages would otherwise be assigned randomly. "
     "A higher value allows for more effective batching of these "
     "messages.",
     0, 900000, 10},


    /*
     * Topic properties
     */

    /* Topic producer properties */
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "request.required.acks", _RK_C_INT,
     _RKT(required_acks),
     "This field indicates the number of acknowledgements the leader "
     "broker must receive from ISR brokers before responding to the "
     "request: "
     "*0*=Broker does not send any response/ack to client, "
     "*-1* or *all*=Broker will block until message is committed by all "
     "in sync replicas (ISRs). If there are less than "
     "`min.insync.replicas` (broker configuration) in the ISR set the "
     "produce request will fail.",
     -1, 1000, -1,
     .s2i =
         {
             {-1, "all"},
         }},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "acks", _RK_C_ALIAS,
     .sdef = "request.required.acks"},

    {_RK_TOPIC | _RK_PRODUCER | _RK_MED, "request.timeout.ms", _RK_C_INT,
     _RKT(request_timeout_ms),
     "The ack timeout of the producer request in milliseconds. "
     "This value is only enforced by the broker and relies "
     "on `request.required.acks` being != 0.",
     1, 900 * 1000, 30 * 1000},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "message.timeout.ms", _RK_C_INT,
     _RKT(message_timeout_ms),
     "Local message timeout. "
     "This value is only enforced locally and limits the time a "
     "produced message waits for successful delivery. "
     "A time of 0 is infinite. "
     "This is the maximum time librdkafka may use to deliver a message "
     "(including retries). Delivery error occurs when either the retry "
     "count or the message timeout are exceeded. "
     "The message timeout is automatically adjusted to "
     "`transaction.timeout.ms` if `transactional.id` is configured.",
     0, INT32_MAX, 300 * 1000},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "delivery.timeout.ms", _RK_C_ALIAS,
     .sdef = "message.timeout.ms"},
    {_RK_TOPIC | _RK_PRODUCER | _RK_DEPRECATED | _RK_EXPERIMENTAL,
     "queuing.strategy", _RK_C_S2I, _RKT(queuing_strategy),
     "Producer queuing strategy. FIFO preserves produce ordering, "
     "while LIFO prioritizes new messages.",
     .vdef = 0,
     .s2i  = {{RD_KAFKA_QUEUE_FIFO, "fifo"}, {RD_KAFKA_QUEUE_LIFO, "lifo"}}},
    {_RK_TOPIC | _RK_PRODUCER | _RK_DEPRECATED, "produce.offset.report",
     _RK_C_BOOL, _RKT(produce_offset_report), "No longer used.", 0, 1, 0},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "partitioner", _RK_C_STR,
     _RKT(partitioner_str),
     "Partitioner: "
     "`random` - random distribution, "
     "`consistent` - CRC32 hash of key (Empty and NULL keys are mapped to "
     "single partition), "
     "`consistent_random` - CRC32 hash of key (Empty and NULL keys are "
     "randomly partitioned), "
     "`murmur2` - Java Producer compatible Murmur2 hash of key (NULL keys are "
     "mapped to single partition), "
     "`murmur2_random` - Java Producer compatible Murmur2 hash of key "
     "(NULL keys are randomly partitioned. This is functionally equivalent "
     "to the default partitioner in the Java Producer.), "
     "`fnv1a` - FNV-1a hash of key (NULL keys are mapped to single partition), "
     "`fnv1a_random` - FNV-1a hash of key (NULL keys are randomly "
     "partitioned).",
     .sdef     = "consistent_random",
     .validate = rd_kafka_conf_validate_partitioner},
    {_RK_TOPIC | _RK_PRODUCER, "partitioner_cb", _RK_C_PTR, _RKT(partitioner),
     "Custom partitioner callback "
     "(set with rd_kafka_topic_conf_set_partitioner_cb())"},
    {_RK_TOPIC | _RK_PRODUCER | _RK_DEPRECATED | _RK_EXPERIMENTAL,
     "msg_order_cmp", _RK_C_PTR, _RKT(msg_order_cmp),
     "Message queue ordering comparator "
     "(set with rd_kafka_topic_conf_set_msg_order_cmp()). "
     "Also see `queuing.strategy`."},
    {_RK_TOPIC, "opaque", _RK_C_PTR, _RKT(opaque),
     "Application opaque (set with rd_kafka_topic_conf_set_opaque())"},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "compression.codec", _RK_C_S2I,
     _RKT(compression_codec),
     "Compression codec to use for compressing message sets. "
     "inherit = inherit global compression.codec configuration.",
     .vdef = RD_KAFKA_COMPRESSION_INHERIT,
     .s2i  = {{RD_KAFKA_COMPRESSION_NONE, "none"},
             {RD_KAFKA_COMPRESSION_GZIP, "gzip", _UNSUPPORTED_ZLIB},
             {RD_KAFKA_COMPRESSION_SNAPPY, "snappy", _UNSUPPORTED_SNAPPY},
             {RD_KAFKA_COMPRESSION_KLZ4, "lz4"},
             {RD_KAFKA_COMPRESSION_ZSTD, "zstd", _UNSUPPORTED_ZSTD},
             {RD_KAFKA_COMPRESSION_INHERIT, "inherit"},
             {0}}},
    {_RK_TOPIC | _RK_PRODUCER | _RK_HIGH, "compression.type", _RK_C_ALIAS,
     .sdef = "compression.codec"},
    {_RK_TOPIC | _RK_PRODUCER | _RK_MED, "compression.level", _RK_C_INT,
     _RKT(compression_level),
     "Compression level parameter for algorithm selected by configuration "
     "property `compression.codec`. Higher values will result in better "
     "compression at the cost of more CPU usage. Usable range is "
     "algorithm-dependent: [0-9] for gzip; [0-12] for lz4; only 0 for snappy; "
     "-1 = codec-dependent default compression level.",
     RD_KAFKA_COMPLEVEL_MIN, RD_KAFKA_COMPLEVEL_MAX,
     RD_KAFKA_COMPLEVEL_DEFAULT},


    /* Topic consumer properties */
    {_RK_TOPIC | _RK_CONSUMER | _RK_DEPRECATED, "auto.commit.enable",
     _RK_C_BOOL, _RKT(auto_commit),
     "[**LEGACY PROPERTY:** This property is used by the simple legacy "
     "consumer only. When using the high-level KafkaConsumer, the global "
     "`enable.auto.commit` property must be used instead]. "
     "If true, periodically commit offset of the last message handed "
     "to the application. This committed offset will be used when the "
     "process restarts to pick up where it left off. "
     "If false, the application will have to call "
     "`rd_kafka_offset_store()` to store an offset (optional). "
     "Offsets will be written to broker or local file according to "
     "offset.store.method.",
     0, 1, 1},
    {_RK_TOPIC | _RK_CONSUMER, "enable.auto.commit", _RK_C_ALIAS,
     .sdef = "auto.commit.enable"},
    {_RK_TOPIC | _RK_CONSUMER | _RK_HIGH, "auto.commit.interval.ms", _RK_C_INT,
     _RKT(auto_commit_interval_ms),
     "[**LEGACY PROPERTY:** This setting is used by the simple legacy "
     "consumer only. When using the high-level KafkaConsumer, the "
     "global `auto.commit.interval.ms` property must be used instead]. "
     "The frequency in milliseconds that the consumer offsets "
     "are committed (written) to offset storage.",
     10, 86400 * 1000, 60 * 1000},
    {_RK_TOPIC | _RK_CONSUMER | _RK_HIGH, "auto.offset.reset", _RK_C_S2I,
     _RKT(auto_offset_reset),
     "Action to take when there is no initial offset in offset store "
     "or the desired offset is out of range: "
     "'smallest','earliest' - automatically reset the offset to the smallest "
     "offset, "
     "'largest','latest' - automatically reset the offset to the largest "
     "offset, "
     "'error' - trigger an error (ERR__AUTO_OFFSET_RESET) which is "
     "retrieved by consuming messages and checking 'message->err'.",
     .vdef = RD_KAFKA_OFFSET_END,
     .s2i =
         {
             {RD_KAFKA_OFFSET_BEGINNING, "smallest"},
             {RD_KAFKA_OFFSET_BEGINNING, "earliest"},
             {RD_KAFKA_OFFSET_BEGINNING, "beginning"},
             {RD_KAFKA_OFFSET_END, "largest"},
             {RD_KAFKA_OFFSET_END, "latest"},
             {RD_KAFKA_OFFSET_END, "end"},
             {RD_KAFKA_OFFSET_INVALID, "error"},
         }},
    {_RK_TOPIC | _RK_CONSUMER | _RK_DEPRECATED, "offset.store.path", _RK_C_STR,
     _RKT(offset_store_path),
     "Path to local file for storing offsets. If the path is a directory "
     "a filename will be automatically generated in that directory based "
     "on the topic and partition. "
     "File-based offset storage will be removed in a future version.",
     .sdef = "."},

    {_RK_TOPIC | _RK_CONSUMER | _RK_DEPRECATED, "offset.store.sync.interval.ms",
     _RK_C_INT, _RKT(offset_store_sync_interval_ms),
     "fsync() interval for the offset file, in milliseconds. "
     "Use -1 to disable syncing, and 0 for immediate sync after "
     "each write. "
     "File-based offset storage will be removed in a future version.",
     -1, 86400 * 1000, -1},

    {_RK_TOPIC | _RK_CONSUMER | _RK_DEPRECATED, "offset.store.method",
     _RK_C_S2I, _RKT(offset_store_method),
     "Offset commit store method: "
     "'file' - DEPRECATED: local file store (offset.store.path, et.al), "
     "'broker' - broker commit store "
     "(requires \"group.id\" to be configured and "
     "Apache Kafka 0.8.2 or later on the broker.).",
     .vdef = RD_KAFKA_OFFSET_METHOD_BROKER,
     .s2i  = {{RD_KAFKA_OFFSET_METHOD_FILE, "file"},
             {RD_KAFKA_OFFSET_METHOD_BROKER, "broker"}}},

    {_RK_TOPIC | _RK_CONSUMER, "consume.callback.max.messages", _RK_C_INT,
     _RKT(consume_callback_max_msgs),
     "Maximum number of messages to dispatch in "
     "one `rd_kafka_consume_callback*()` call (0 = unlimited)",
     0, 1000000, 0},

    {0, /* End */}};

/**
 * @returns the property object for \p name in \p scope, or NULL if not found.
 * @remark does not work with interceptor configs.
 */
const struct rd_kafka_property *rd_kafka_conf_prop_find(int scope,
                                                        const char *name) {
        const struct rd_kafka_property *prop;

restart:
        for (prop = rd_kafka_properties; prop->name; prop++) {

                if (!(prop->scope & scope))
                        continue;

                if (strcmp(prop->name, name))
                        continue;

                if (prop->type == _RK_C_ALIAS) {
                        /* Caller supplied an alias, restart
                         * search for real name. */
                        name = prop->sdef;
                        goto restart;
                }

                return prop;
        }

        return NULL;
}

/**
 * @returns rd_true if property has been set/modified, else rd_false.
 *
 * @warning Asserts if the property does not exist.
 */
rd_bool_t rd_kafka_conf_is_modified(const rd_kafka_conf_t *conf,
                                    const char *name) {
        const struct rd_kafka_property *prop;

        if (!(prop = rd_kafka_conf_prop_find(_RK_GLOBAL, name)))
                RD_BUG("Configuration property \"%s\" does not exist", name);

        return rd_kafka_anyconf_is_modified(conf, prop);
}


/**
 * @returns true if property has been set/modified, else 0.
 *
 * @warning Asserts if the property does not exist.
 */
static rd_bool_t
rd_kafka_topic_conf_is_modified(const rd_kafka_topic_conf_t *conf,
                                const char *name) {
        const struct rd_kafka_property *prop;

        if (!(prop = rd_kafka_conf_prop_find(_RK_TOPIC, name)))
                RD_BUG("Topic configuration property \"%s\" does not exist",
                       name);

        return rd_kafka_anyconf_is_modified(conf, prop);
}



static rd_kafka_conf_res_t
rd_kafka_anyconf_set_prop0(int scope,
                           void *conf,
                           const struct rd_kafka_property *prop,
                           const char *istr,
                           int ival,
                           rd_kafka_conf_set_mode_t set_mode,
                           char *errstr,
                           size_t errstr_size) {
        rd_kafka_conf_res_t res;

#define _RK_PTR(TYPE, BASE, OFFSET) (TYPE)(void *)(((char *)(BASE)) + (OFFSET))

        /* Try interceptors first (only for GLOBAL config) */
        if (scope & _RK_GLOBAL) {
                if (prop->type == _RK_C_PTR || prop->type == _RK_C_INTERNAL)
                        res = RD_KAFKA_CONF_UNKNOWN;
                else
                        res = rd_kafka_interceptors_on_conf_set(
                            conf, prop->name, istr, errstr, errstr_size);
                if (res != RD_KAFKA_CONF_UNKNOWN)
                        return res;
        }


        if (prop->set) {
                /* Custom setter */

                res = prop->set(scope, conf, prop->name, istr,
                                _RK_PTR(void *, conf, prop->offset), set_mode,
                                errstr, errstr_size);

                if (res != RD_KAFKA_CONF_OK)
                        return res;

                /* FALLTHRU so that property value is set. */
        }

        switch (prop->type) {
        case _RK_C_STR: {
                char **str = _RK_PTR(char **, conf, prop->offset);
                if (*str)
                        rd_free(*str);
                if (istr)
                        *str = rd_strdup(istr);
                else
                        *str = prop->sdef ? rd_strdup(prop->sdef) : NULL;
                break;
        }
        case _RK_C_KSTR: {
                rd_kafkap_str_t **kstr =
                    _RK_PTR(rd_kafkap_str_t **, conf, prop->offset);
                if (*kstr)
                        rd_kafkap_str_destroy(*kstr);
                if (istr)
                        *kstr = rd_kafkap_str_new(istr, -1);
                else
                        *kstr = prop->sdef ? rd_kafkap_str_new(prop->sdef, -1)
                                           : NULL;
                break;
        }
        case _RK_C_PTR:
                *_RK_PTR(const void **, conf, prop->offset) = istr;
                break;
        case _RK_C_BOOL:
        case _RK_C_INT:
        case _RK_C_S2I:
        case _RK_C_S2F: {
                int *val = _RK_PTR(int *, conf, prop->offset);

                if (prop->type == _RK_C_S2F) {
                        switch (set_mode) {
                        case _RK_CONF_PROP_SET_REPLACE:
                                *val = ival;
                                break;
                        case _RK_CONF_PROP_SET_ADD:
                                *val |= ival;
                                break;
                        case _RK_CONF_PROP_SET_DEL:
                                *val &= ~ival;
                                break;
                        }
                } else {
                        /* Single assignment */
                        *val = ival;
                }
                break;
        }
        case _RK_C_DBL: {
                double *val = _RK_PTR(double *, conf, prop->offset);
                if (istr) {
                        char *endptr;
                        double new_val = strtod(istr, &endptr);
                        /* This is verified in set_prop() */
                        rd_assert(endptr != istr);
                        *val = new_val;
                } else
                        *val = prop->ddef;
                break;
        }

        case _RK_C_PATLIST: {
                /* Split comma-separated list into individual regex expressions
                 * that are verified and then append to the provided list. */
                rd_kafka_pattern_list_t **plist;

                plist = _RK_PTR(rd_kafka_pattern_list_t **, conf, prop->offset);

                if (*plist)
                        rd_kafka_pattern_list_destroy(*plist);

                if (istr) {
                        if (!(*plist = rd_kafka_pattern_list_new(
                                  istr, errstr, (int)errstr_size)))
                                return RD_KAFKA_CONF_INVALID;
                } else
                        *plist = NULL;

                break;
        }

        case _RK_C_INTERNAL:
                /* Probably handled by setter */
                break;

        default:
                rd_kafka_assert(NULL, !*"unknown conf type");
        }


        rd_kafka_anyconf_set_modified(conf, prop, 1 /*modified*/);
        return RD_KAFKA_CONF_OK;
}


/**
 * @brief Find s2i (string-to-int mapping) entry and return its array index,
 *        or -1 on miss.
 */
static int rd_kafka_conf_s2i_find(const struct rd_kafka_property *prop,
                                  const char *value) {
        int j;

        for (j = 0; j < (int)RD_ARRAYSIZE(prop->s2i); j++) {
                if (prop->s2i[j].str && !rd_strcasecmp(prop->s2i[j].str, value))
                        return j;
        }

        return -1;
}


/**
 * @brief Set configuration property.
 *
 * @param allow_specific Allow rd_kafka_*conf_set_..() to be set,
 *        such as rd_kafka_conf_set_log_cb().
 *        Should not be allowed from the conf_set() string interface.
 */
static rd_kafka_conf_res_t
rd_kafka_anyconf_set_prop(int scope,
                          void *conf,
                          const struct rd_kafka_property *prop,
                          const char *value,
                          int allow_specific,
                          char *errstr,
                          size_t errstr_size) {
        int ival;

        if (prop->unsupported) {
                rd_snprintf(errstr, errstr_size,
                            "Configuration property \"%s\" not supported "
                            "in this build: %s",
                            prop->name, prop->unsupported);
                return RD_KAFKA_CONF_INVALID;
        }

        switch (prop->type) {
        case _RK_C_STR:
                /* Left-trim string(likes) */
                if (value)
                        while (isspace((int)*value))
                                value++;

                /* FALLTHRU */
        case _RK_C_KSTR:
                if (prop->s2i[0].str) {
                        int match;

                        if (!value || (match = rd_kafka_conf_s2i_find(
                                           prop, value)) == -1) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid value for "
                                            "configuration property \"%s\": "
                                            "%s",
                                            prop->name, value);
                                return RD_KAFKA_CONF_INVALID;
                        }

                        /* Replace value string with canonical form */
                        value = prop->s2i[match].str;
                }
                /* FALLTHRU */
        case _RK_C_PATLIST:
                if (prop->validate &&
                    (!value || !prop->validate(prop, value, -1))) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value for "
                                    "configuration property \"%s\": %s",
                                    prop->name, value);
                        return RD_KAFKA_CONF_INVALID;
                }

                return rd_kafka_anyconf_set_prop0(scope, conf, prop, value, 0,
                                                  _RK_CONF_PROP_SET_REPLACE,
                                                  errstr, errstr_size);

        case _RK_C_PTR:
                /* Allow hidden internal unit test properties to
                 * be set from generic conf_set() interface. */
                if (!allow_specific && !(prop->scope & _RK_HIDDEN)) {
                        rd_snprintf(errstr, errstr_size,
                                    "Property \"%s\" must be set through "
                                    "dedicated .._set_..() function",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }
                return rd_kafka_anyconf_set_prop0(scope, conf, prop, value, 0,
                                                  _RK_CONF_PROP_SET_REPLACE,
                                                  errstr, errstr_size);

        case _RK_C_BOOL:
                if (!value) {
                        rd_snprintf(errstr, errstr_size,
                                    "Bool configuration property \"%s\" cannot "
                                    "be set to empty value",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }


                if (!rd_strcasecmp(value, "true") ||
                    !rd_strcasecmp(value, "t") || !strcmp(value, "1"))
                        ival = 1;
                else if (!rd_strcasecmp(value, "false") ||
                         !rd_strcasecmp(value, "f") || !strcmp(value, "0"))
                        ival = 0;
                else {
                        rd_snprintf(errstr, errstr_size,
                                    "Expected bool value for \"%s\": "
                                    "true or false",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }

                rd_kafka_anyconf_set_prop0(scope, conf, prop, value, ival,
                                           _RK_CONF_PROP_SET_REPLACE, errstr,
                                           errstr_size);
                return RD_KAFKA_CONF_OK;

        case _RK_C_INT: {
                const char *end;

                if (!value) {
                        rd_snprintf(errstr, errstr_size,
                                    "Integer configuration "
                                    "property \"%s\" cannot be set "
                                    "to empty value",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }

                ival = (int)strtol(value, (char **)&end, 0);
                if (end == value) {
                        /* Non numeric, check s2i for string mapping */
                        int match = rd_kafka_conf_s2i_find(prop, value);

                        if (match == -1) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid value for "
                                            "configuration property \"%s\"",
                                            prop->name);
                                return RD_KAFKA_CONF_INVALID;
                        }

                        if (prop->s2i[match].unsupported) {
                                rd_snprintf(errstr, errstr_size,
                                            "Unsupported value \"%s\" for "
                                            "configuration property \"%s\": %s",
                                            value, prop->name,
                                            prop->s2i[match].unsupported);
                                return RD_KAFKA_CONF_INVALID;
                        }

                        ival = prop->s2i[match].val;
                }

                if (ival < prop->vmin || ival > prop->vmax) {
                        rd_snprintf(errstr, errstr_size,
                                    "Configuration property \"%s\" value "
                                    "%i is outside allowed range %i..%i\n",
                                    prop->name, ival, prop->vmin, prop->vmax);
                        return RD_KAFKA_CONF_INVALID;
                }

                rd_kafka_anyconf_set_prop0(scope, conf, prop, value, ival,
                                           _RK_CONF_PROP_SET_REPLACE, errstr,
                                           errstr_size);
                return RD_KAFKA_CONF_OK;
        }

        case _RK_C_DBL: {
                const char *end;
                double dval;

                if (!value) {
                        rd_snprintf(errstr, errstr_size,
                                    "Float configuration "
                                    "property \"%s\" cannot be set "
                                    "to empty value",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }

                dval = strtod(value, (char **)&end);
                if (end == value) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value for "
                                    "configuration property \"%s\"",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }

                if (dval < prop->dmin || dval > prop->dmax) {
                        rd_snprintf(errstr, errstr_size,
                                    "Configuration property \"%s\" value "
                                    "%g is outside allowed range %g..%g\n",
                                    prop->name, dval, prop->dmin, prop->dmax);
                        return RD_KAFKA_CONF_INVALID;
                }

                rd_kafka_anyconf_set_prop0(scope, conf, prop, value, 0,
                                           _RK_CONF_PROP_SET_REPLACE, errstr,
                                           errstr_size);
                return RD_KAFKA_CONF_OK;
        }

        case _RK_C_S2I:
        case _RK_C_S2F: {
                int j;
                const char *next;

                if (!value) {
                        rd_snprintf(errstr, errstr_size,
                                    "Configuration "
                                    "property \"%s\" cannot be set "
                                    "to empty value",
                                    prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }

                next = value;
                while (next && *next) {
                        const char *s, *t;
                        rd_kafka_conf_set_mode_t set_mode =
                            _RK_CONF_PROP_SET_ADD; /* S2F */

                        s = next;

                        if (prop->type == _RK_C_S2F && (t = strchr(s, ','))) {
                                /* CSV flag field */
                                next = t + 1;
                        } else {
                                /* Single string */
                                t    = s + strlen(s);
                                next = NULL;
                        }


                        /* Left trim */
                        while (s < t && isspace((int)*s))
                                s++;

                        /* Right trim */
                        while (t > s && isspace((int)*t))
                                t--;

                        /* S2F: +/- prefix */
                        if (prop->type == _RK_C_S2F) {
                                if (*s == '+') {
                                        set_mode = _RK_CONF_PROP_SET_ADD;
                                        s++;
                                } else if (*s == '-') {
                                        set_mode = _RK_CONF_PROP_SET_DEL;
                                        s++;
                                }
                        }

                        /* Empty string? */
                        if (s == t)
                                continue;

                        /* Match string to s2i table entry */
                        for (j = 0; j < (int)RD_ARRAYSIZE(prop->s2i); j++) {
                                int new_val;

                                if (!prop->s2i[j].str)
                                        continue;

                                if (strlen(prop->s2i[j].str) ==
                                        (size_t)(t - s) &&
                                    !rd_strncasecmp(prop->s2i[j].str, s,
                                                    (int)(t - s)))
                                        new_val = prop->s2i[j].val;
                                else
                                        continue;

                                if (prop->s2i[j].unsupported) {
                                        rd_snprintf(
                                            errstr, errstr_size,
                                            "Unsupported value \"%.*s\" "
                                            "for configuration property "
                                            "\"%s\": %s",
                                            (int)(t - s), s, prop->name,
                                            prop->s2i[j].unsupported);
                                        return RD_KAFKA_CONF_INVALID;
                                }

                                rd_kafka_anyconf_set_prop0(
                                    scope, conf, prop, value, new_val, set_mode,
                                    errstr, errstr_size);

                                if (prop->type == _RK_C_S2F) {
                                        /* Flags: OR it in: do next */
                                        break;
                                } else {
                                        /* Single assignment */
                                        return RD_KAFKA_CONF_OK;
                                }
                        }

                        /* S2F: Good match: continue with next */
                        if (j < (int)RD_ARRAYSIZE(prop->s2i))
                                continue;

                        /* No match */
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value \"%.*s\" for "
                                    "configuration property \"%s\"",
                                    (int)(t - s), s, prop->name);
                        return RD_KAFKA_CONF_INVALID;
                }
                return RD_KAFKA_CONF_OK;
        }

        case _RK_C_INTERNAL:
                rd_snprintf(errstr, errstr_size,
                            "Internal property \"%s\" not settable",
                            prop->name);
                return RD_KAFKA_CONF_INVALID;

        case _RK_C_INVALID:
                rd_snprintf(errstr, errstr_size, "%s", prop->desc);
                return RD_KAFKA_CONF_INVALID;

        default:
                rd_kafka_assert(NULL, !*"unknown conf type");
        }

        /* not reachable */
        return RD_KAFKA_CONF_INVALID;
}



static void rd_kafka_defaultconf_set(int scope, void *conf) {
        const struct rd_kafka_property *prop;

        for (prop = rd_kafka_properties; prop->name; prop++) {
                if (!(prop->scope & scope))
                        continue;

                if (prop->type == _RK_C_ALIAS || prop->type == _RK_C_INVALID)
                        continue;

                if (prop->ctor)
                        prop->ctor(scope, conf);

                if (prop->sdef || prop->vdef || prop->pdef ||
                    !rd_dbl_zero(prop->ddef))
                        rd_kafka_anyconf_set_prop0(
                            scope, conf, prop,
                            prop->sdef ? prop->sdef : prop->pdef, prop->vdef,
                            _RK_CONF_PROP_SET_REPLACE, NULL, 0);
        }
}

rd_kafka_conf_t *rd_kafka_conf_new(void) {
        rd_kafka_conf_t *conf = rd_calloc(1, sizeof(*conf));
        rd_assert(RD_KAFKA_CONF_PROPS_IDX_MAX > sizeof(*conf) &&
                  *"Increase RD_KAFKA_CONF_PROPS_IDX_MAX");
        rd_kafka_defaultconf_set(_RK_GLOBAL, conf);
        rd_kafka_anyconf_clear_all_is_modified(conf);
        return conf;
}

rd_kafka_topic_conf_t *rd_kafka_topic_conf_new(void) {
        rd_kafka_topic_conf_t *tconf = rd_calloc(1, sizeof(*tconf));
        rd_assert(RD_KAFKA_CONF_PROPS_IDX_MAX > sizeof(*tconf) &&
                  *"Increase RD_KAFKA_CONF_PROPS_IDX_MAX");
        rd_kafka_defaultconf_set(_RK_TOPIC, tconf);
        rd_kafka_anyconf_clear_all_is_modified(tconf);
        return tconf;
}


static int rd_kafka_anyconf_set(int scope,
                                void *conf,
                                const char *name,
                                const char *value,
                                char *errstr,
                                size_t errstr_size) {
        char estmp[1];
        const struct rd_kafka_property *prop;
        rd_kafka_conf_res_t res;

        if (!errstr) {
                errstr      = estmp;
                errstr_size = 0;
        }

        if (value && !*value)
                value = NULL;

        /* Try interceptors first (only for GLOBAL config for now) */
        if (scope & _RK_GLOBAL) {
                res = rd_kafka_interceptors_on_conf_set(
                    (rd_kafka_conf_t *)conf, name, value, errstr, errstr_size);
                /* Handled (successfully or not) by interceptor. */
                if (res != RD_KAFKA_CONF_UNKNOWN)
                        return res;
        }

        /* Then global config */


        for (prop = rd_kafka_properties; prop->name; prop++) {

                if (!(prop->scope & scope))
                        continue;

                if (strcmp(prop->name, name))
                        continue;

                if (prop->type == _RK_C_ALIAS)
                        return rd_kafka_anyconf_set(scope, conf, prop->sdef,
                                                    value, errstr, errstr_size);

                return rd_kafka_anyconf_set_prop(scope, conf, prop, value,
                                                 0 /*don't allow specifics*/,
                                                 errstr, errstr_size);
        }

        rd_snprintf(errstr, errstr_size,
                    "No such configuration property: \"%s\"", name);

        return RD_KAFKA_CONF_UNKNOWN;
}


/**
 * @brief Set a rd_kafka_*_conf_set_...() specific property, such as
 *        rd_kafka_conf_set_error_cb().
 *
 * @warning Will not call interceptor's on_conf_set.
 * @warning Asserts if \p name is not known or value is incorrect.
 *
 * Implemented as a macro to have rd_assert() print the original function.
 */

#define rd_kafka_anyconf_set_internal(SCOPE, CONF, NAME, VALUE)                \
        do {                                                                   \
                const struct rd_kafka_property *_prop;                         \
                rd_kafka_conf_res_t _res;                                      \
                _prop = rd_kafka_conf_prop_find(SCOPE, NAME);                  \
                rd_assert(_prop && * "invalid property name");                 \
                _res = rd_kafka_anyconf_set_prop(                              \
                    SCOPE, CONF, _prop, (const void *)VALUE,                   \
                    1 /*allow-specifics*/, NULL, 0);                           \
                rd_assert(_res == RD_KAFKA_CONF_OK);                           \
        } while (0)


rd_kafka_conf_res_t rd_kafka_conf_set(rd_kafka_conf_t *conf,
                                      const char *name,
                                      const char *value,
                                      char *errstr,
                                      size_t errstr_size) {
        rd_kafka_conf_res_t res;

        res = rd_kafka_anyconf_set(_RK_GLOBAL, conf, name, value, errstr,
                                   errstr_size);
        if (res != RD_KAFKA_CONF_UNKNOWN)
                return res;

        /* Fallthru:
         * If the global property was unknown, try setting it on the
         * default topic config. */
        if (!conf->topic_conf) {
                /* Create topic config, might be over-written by application
                 * later. */
                rd_kafka_conf_set_default_topic_conf(conf,
                                                     rd_kafka_topic_conf_new());
        }

        return rd_kafka_topic_conf_set(conf->topic_conf, name, value, errstr,
                                       errstr_size);
}


rd_kafka_conf_res_t rd_kafka_topic_conf_set(rd_kafka_topic_conf_t *conf,
                                            const char *name,
                                            const char *value,
                                            char *errstr,
                                            size_t errstr_size) {
        if (!strncmp(name, "topic.", strlen("topic.")))
                name += strlen("topic.");

        return rd_kafka_anyconf_set(_RK_TOPIC, conf, name, value, errstr,
                                    errstr_size);
}


/**
 * @brief Overwrites the contents of \p str up until but not including
 *        the nul-term.
 */
void rd_kafka_desensitize_str(char *str) {
        size_t len;
        static const char redacted[] = "(REDACTED)";

#ifdef _WIN32
        len = strlen(str);
        SecureZeroMemory(str, len);
#else
        volatile char *volatile s;

        for (s = str; *s; s++)
                *s = '\0';

        len = (size_t)(s - str);
#endif

        if (len > sizeof(redacted))
                memcpy(str, redacted, sizeof(redacted));
}



/**
 * @brief Overwrite the value of \p prop, if sensitive.
 */
static RD_INLINE void
rd_kafka_anyconf_prop_desensitize(int scope,
                                  void *conf,
                                  const struct rd_kafka_property *prop) {
        if (likely(!(prop->scope & _RK_SENSITIVE)))
                return;

        switch (prop->type) {
        case _RK_C_STR: {
                char **str = _RK_PTR(char **, conf, prop->offset);
                if (*str)
                        rd_kafka_desensitize_str(*str);
                break;
        }

        case _RK_C_INTERNAL:
                /* This is typically a pointer to something, the
                 * _RK_SENSITIVE flag is set to get it redacted in
                 * ..dump_dbg(), but we don't have to desensitize
                 * anything here. */
                break;

        default:
                rd_assert(!*"BUG: Don't know how to desensitize prop type");
                break;
        }
}


/**
 * @brief Desensitize all sensitive properties in \p conf
 */
static void rd_kafka_anyconf_desensitize(int scope, void *conf) {
        const struct rd_kafka_property *prop;

        for (prop = rd_kafka_properties; prop->name; prop++) {
                if (!(prop->scope & scope))
                        continue;

                rd_kafka_anyconf_prop_desensitize(scope, conf, prop);
        }
}

/**
 * @brief Overwrite the values of sensitive properties
 */
void rd_kafka_conf_desensitize(rd_kafka_conf_t *conf) {
        if (conf->topic_conf)
                rd_kafka_anyconf_desensitize(_RK_TOPIC, conf->topic_conf);
        rd_kafka_anyconf_desensitize(_RK_GLOBAL, conf);
}

/**
 * @brief Overwrite the values of sensitive properties
 */
void rd_kafka_topic_conf_desensitize(rd_kafka_topic_conf_t *tconf) {
        rd_kafka_anyconf_desensitize(_RK_TOPIC, tconf);
}


static void rd_kafka_anyconf_clear(int scope,
                                   void *conf,
                                   const struct rd_kafka_property *prop) {

        rd_kafka_anyconf_prop_desensitize(scope, conf, prop);

        switch (prop->type) {
        case _RK_C_STR: {
                char **str = _RK_PTR(char **, conf, prop->offset);

                if (*str) {
                        if (prop->set) {
                                prop->set(scope, conf, prop->name, NULL, *str,
                                          _RK_CONF_PROP_SET_DEL, NULL, 0);
                                /* FALLTHRU */
                        }
                        rd_free(*str);
                        *str = NULL;
                }
        } break;

        case _RK_C_KSTR: {
                rd_kafkap_str_t **kstr =
                    _RK_PTR(rd_kafkap_str_t **, conf, prop->offset);
                if (*kstr) {
                        rd_kafkap_str_destroy(*kstr);
                        *kstr = NULL;
                }
        } break;

        case _RK_C_PATLIST: {
                rd_kafka_pattern_list_t **plist;
                plist = _RK_PTR(rd_kafka_pattern_list_t **, conf, prop->offset);
                if (*plist) {
                        rd_kafka_pattern_list_destroy(*plist);
                        *plist = NULL;
                }
        } break;

        case _RK_C_PTR:
                if (_RK_PTR(void *, conf, prop->offset) != NULL) {
                        if (!strcmp(prop->name, "default_topic_conf")) {
                                rd_kafka_topic_conf_t **tconf;

                                tconf = _RK_PTR(rd_kafka_topic_conf_t **, conf,
                                                prop->offset);
                                if (*tconf) {
                                        rd_kafka_topic_conf_destroy(*tconf);
                                        *tconf = NULL;
                                }
                        }
                }
                break;

        default:
                break;
        }

        if (prop->dtor)
                prop->dtor(scope, conf);
}

void rd_kafka_anyconf_destroy(int scope, void *conf) {
        const struct rd_kafka_property *prop;

        /* Call on_conf_destroy() interceptors */
        if (scope == _RK_GLOBAL)
                rd_kafka_interceptors_on_conf_destroy(conf);

        for (prop = rd_kafka_properties; prop->name; prop++) {
                if (!(prop->scope & scope))
                        continue;

                rd_kafka_anyconf_clear(scope, conf, prop);
        }
}


void rd_kafka_conf_destroy(rd_kafka_conf_t *conf) {
        rd_kafka_anyconf_destroy(_RK_GLOBAL, conf);
        // FIXME: partition_assignors
        rd_free(conf);
}

void rd_kafka_topic_conf_destroy(rd_kafka_topic_conf_t *topic_conf) {
        rd_kafka_anyconf_destroy(_RK_TOPIC, topic_conf);
        rd_free(topic_conf);
}



static void rd_kafka_anyconf_copy(int scope,
                                  void *dst,
                                  const void *src,
                                  size_t filter_cnt,
                                  const char **filter) {
        const struct rd_kafka_property *prop;

        for (prop = rd_kafka_properties; prop->name; prop++) {
                const char *val = NULL;
                int ival        = 0;
                char *valstr;
                size_t valsz;
                size_t fi;
                size_t nlen;

                if (!(prop->scope & scope))
                        continue;

                if (prop->type == _RK_C_ALIAS || prop->type == _RK_C_INVALID)
                        continue;

                /* Skip properties that have not been set,
                 * unless it is an internal one which requires
                 * extra logic, such as the interceptors. */
                if (!rd_kafka_anyconf_is_modified(src, prop) &&
                    prop->type != _RK_C_INTERNAL)
                        continue;

                /* Apply filter, if any. */
                nlen = strlen(prop->name);
                for (fi = 0; fi < filter_cnt; fi++) {
                        size_t flen = strlen(filter[fi]);
                        if (nlen >= flen &&
                            !strncmp(filter[fi], prop->name, flen))
                                break;
                }
                if (fi < filter_cnt)
                        continue; /* Filter matched */

                switch (prop->type) {
                case _RK_C_STR:
                case _RK_C_PTR:
                        val = *_RK_PTR(const char **, src, prop->offset);

                        if (!strcmp(prop->name, "default_topic_conf") && val)
                                val = (void *)rd_kafka_topic_conf_dup(
                                    (const rd_kafka_topic_conf_t *)(void *)val);
                        break;
                case _RK_C_KSTR: {
                        rd_kafkap_str_t **kstr =
                            _RK_PTR(rd_kafkap_str_t **, src, prop->offset);
                        if (*kstr)
                                val = (*kstr)->str;
                        break;
                }

                case _RK_C_BOOL:
                case _RK_C_INT:
                case _RK_C_S2I:
                case _RK_C_S2F:
                        ival = *_RK_PTR(const int *, src, prop->offset);

                        /* Get string representation of configuration value. */
                        valsz = 0;
                        rd_kafka_anyconf_get0(src, prop, NULL, &valsz);
                        valstr = rd_alloca(valsz);
                        rd_kafka_anyconf_get0(src, prop, valstr, &valsz);
                        val = valstr;
                        break;
                case _RK_C_DBL:
                        /* Get string representation of configuration value. */
                        valsz = 0;
                        rd_kafka_anyconf_get0(src, prop, NULL, &valsz);
                        valstr = rd_alloca(valsz);
                        rd_kafka_anyconf_get0(src, prop, valstr, &valsz);
                        val = valstr;
                        break;
                case _RK_C_PATLIST: {
                        const rd_kafka_pattern_list_t **plist;
                        plist = _RK_PTR(const rd_kafka_pattern_list_t **, src,
                                        prop->offset);
                        if (*plist)
                                val = (*plist)->rkpl_orig;
                        break;
                }
                case _RK_C_INTERNAL:
                        /* Handled by ->copy() below. */
                        break;
                default:
                        continue;
                }

                if (prop->copy)
                        prop->copy(scope, dst, src,
                                   _RK_PTR(void *, dst, prop->offset),
                                   _RK_PTR(const void *, src, prop->offset),
                                   filter_cnt, filter);

                rd_kafka_anyconf_set_prop0(scope, dst, prop, val, ival,
                                           _RK_CONF_PROP_SET_REPLACE, NULL, 0);
        }
}


rd_kafka_conf_t *rd_kafka_conf_dup(const rd_kafka_conf_t *conf) {
        rd_kafka_conf_t *new = rd_kafka_conf_new();

        rd_kafka_interceptors_on_conf_dup(new, conf, 0, NULL);

        rd_kafka_anyconf_copy(_RK_GLOBAL, new, conf, 0, NULL);

        return new;
}

rd_kafka_conf_t *rd_kafka_conf_dup_filter(const rd_kafka_conf_t *conf,
                                          size_t filter_cnt,
                                          const char **filter) {
        rd_kafka_conf_t *new = rd_kafka_conf_new();

        rd_kafka_interceptors_on_conf_dup(new, conf, filter_cnt, filter);

        rd_kafka_anyconf_copy(_RK_GLOBAL, new, conf, filter_cnt, filter);

        return new;
}


rd_kafka_topic_conf_t *
rd_kafka_topic_conf_dup(const rd_kafka_topic_conf_t *conf) {
        rd_kafka_topic_conf_t *new = rd_kafka_topic_conf_new();

        rd_kafka_anyconf_copy(_RK_TOPIC, new, conf, 0, NULL);

        return new;
}

rd_kafka_topic_conf_t *rd_kafka_default_topic_conf_dup(rd_kafka_t *rk) {
        if (rk->rk_conf.topic_conf)
                return rd_kafka_topic_conf_dup(rk->rk_conf.topic_conf);
        else
                return rd_kafka_topic_conf_new();
}

void rd_kafka_conf_set_events(rd_kafka_conf_t *conf, int events) {
        char tmp[32];
        rd_snprintf(tmp, sizeof(tmp), "%d", events);
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "enabled_events", tmp);
}

void rd_kafka_conf_set_background_event_cb(
    rd_kafka_conf_t *conf,
    void (*event_cb)(rd_kafka_t *rk, rd_kafka_event_t *rkev, void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "background_event_cb",
                                      event_cb);
}


void rd_kafka_conf_set_dr_cb(rd_kafka_conf_t *conf,
                             void (*dr_cb)(rd_kafka_t *rk,
                                           void *payload,
                                           size_t len,
                                           rd_kafka_resp_err_t err,
                                           void *opaque,
                                           void *msg_opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "dr_cb", dr_cb);
}


void rd_kafka_conf_set_dr_msg_cb(
    rd_kafka_conf_t *conf,
    void (*dr_msg_cb)(rd_kafka_t *rk,
                      const rd_kafka_message_t *rkmessage,
                      void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "dr_msg_cb", dr_msg_cb);
}


void rd_kafka_conf_set_consume_cb(
    rd_kafka_conf_t *conf,
    void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "consume_cb",
                                      consume_cb);
}

void rd_kafka_conf_set_rebalance_cb(
    rd_kafka_conf_t *conf,
    void (*rebalance_cb)(rd_kafka_t *rk,
                         rd_kafka_resp_err_t err,
                         rd_kafka_topic_partition_list_t *partitions,
                         void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "rebalance_cb",
                                      rebalance_cb);
}

void rd_kafka_conf_set_offset_commit_cb(
    rd_kafka_conf_t *conf,
    void (*offset_commit_cb)(rd_kafka_t *rk,
                             rd_kafka_resp_err_t err,
                             rd_kafka_topic_partition_list_t *offsets,
                             void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "offset_commit_cb",
                                      offset_commit_cb);
}



void rd_kafka_conf_set_error_cb(rd_kafka_conf_t *conf,
                                void (*error_cb)(rd_kafka_t *rk,
                                                 int err,
                                                 const char *reason,
                                                 void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "error_cb", error_cb);
}


void rd_kafka_conf_set_throttle_cb(rd_kafka_conf_t *conf,
                                   void (*throttle_cb)(rd_kafka_t *rk,
                                                       const char *broker_name,
                                                       int32_t broker_id,
                                                       int throttle_time_ms,
                                                       void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "throttle_cb",
                                      throttle_cb);
}


void rd_kafka_conf_set_log_cb(rd_kafka_conf_t *conf,
                              void (*log_cb)(const rd_kafka_t *rk,
                                             int level,
                                             const char *fac,
                                             const char *buf)) {
#if !WITH_SYSLOG
        if (log_cb == rd_kafka_log_syslog)
                rd_assert(!*"syslog support not enabled in this build");
#endif
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "log_cb", log_cb);
}


void rd_kafka_conf_set_stats_cb(rd_kafka_conf_t *conf,
                                int (*stats_cb)(rd_kafka_t *rk,
                                                char *json,
                                                size_t json_len,
                                                void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "stats_cb", stats_cb);
}

void rd_kafka_conf_set_oauthbearer_token_refresh_cb(
    rd_kafka_conf_t *conf,
    void (*oauthbearer_token_refresh_cb)(rd_kafka_t *rk,
                                         const char *oauthbearer_config,
                                         void *opaque)) {
#if WITH_SASL_OAUTHBEARER
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf,
                                      "oauthbearer_token_refresh_cb",
                                      oauthbearer_token_refresh_cb);
#endif
}

void rd_kafka_conf_enable_sasl_queue(rd_kafka_conf_t *conf, int enable) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "enable_sasl_queue",
                                      (enable ? "true" : "false"));
}

void rd_kafka_conf_set_socket_cb(
    rd_kafka_conf_t *conf,
    int (*socket_cb)(int domain, int type, int protocol, void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "socket_cb", socket_cb);
}

void rd_kafka_conf_set_connect_cb(rd_kafka_conf_t *conf,
                                  int (*connect_cb)(int sockfd,
                                                    const struct sockaddr *addr,
                                                    int addrlen,
                                                    const char *id,
                                                    void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "connect_cb",
                                      connect_cb);
}

void rd_kafka_conf_set_closesocket_cb(rd_kafka_conf_t *conf,
                                      int (*closesocket_cb)(int sockfd,
                                                            void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "closesocket_cb",
                                      closesocket_cb);
}



#ifndef _WIN32
void rd_kafka_conf_set_open_cb(rd_kafka_conf_t *conf,
                               int (*open_cb)(const char *pathname,
                                              int flags,
                                              mode_t mode,
                                              void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "open_cb", open_cb);
}
#endif

void rd_kafka_conf_set_resolve_cb(
    rd_kafka_conf_t *conf,
    int (*resolve_cb)(const char *node,
                      const char *service,
                      const struct addrinfo *hints,
                      struct addrinfo **res,
                      void *opaque)) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "resolve_cb",
                                      resolve_cb);
}

rd_kafka_conf_res_t rd_kafka_conf_set_ssl_cert_verify_cb(
    rd_kafka_conf_t *conf,
    int (*ssl_cert_verify_cb)(rd_kafka_t *rk,
                              const char *broker_name,
                              int32_t broker_id,
                              int *x509_set_error,
                              int depth,
                              const char *buf,
                              size_t size,
                              char *errstr,
                              size_t errstr_size,
                              void *opaque)) {
#if !WITH_SSL
        return RD_KAFKA_CONF_INVALID;
#else
        rd_kafka_anyconf_set_internal(
            _RK_GLOBAL, conf, "ssl.certificate.verify_cb", ssl_cert_verify_cb);
        return RD_KAFKA_CONF_OK;
#endif
}


void rd_kafka_conf_set_opaque(rd_kafka_conf_t *conf, void *opaque) {
        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "opaque", opaque);
}


void rd_kafka_conf_set_engine_callback_data(rd_kafka_conf_t *conf,
                                            void *callback_data) {
        rd_kafka_anyconf_set_internal(
            _RK_GLOBAL, conf, "ssl_engine_callback_data", callback_data);
}


void rd_kafka_conf_set_default_topic_conf(rd_kafka_conf_t *conf,
                                          rd_kafka_topic_conf_t *tconf) {
        if (conf->topic_conf) {
                if (rd_kafka_anyconf_is_any_modified(conf->topic_conf))
                        conf->warn.default_topic_conf_overwritten = rd_true;
                rd_kafka_topic_conf_destroy(conf->topic_conf);
        }

        rd_kafka_anyconf_set_internal(_RK_GLOBAL, conf, "default_topic_conf",
                                      tconf);
}

rd_kafka_topic_conf_t *
rd_kafka_conf_get_default_topic_conf(rd_kafka_conf_t *conf) {
        return conf->topic_conf;
}


void rd_kafka_topic_conf_set_partitioner_cb(
    rd_kafka_topic_conf_t *topic_conf,
    int32_t (*partitioner)(const rd_kafka_topic_t *rkt,
                           const void *keydata,
                           size_t keylen,
                           int32_t partition_cnt,
                           void *rkt_opaque,
                           void *msg_opaque)) {
        rd_kafka_anyconf_set_internal(_RK_TOPIC, topic_conf, "partitioner_cb",
                                      partitioner);
}

void rd_kafka_topic_conf_set_msg_order_cmp(
    rd_kafka_topic_conf_t *topic_conf,
    int (*msg_order_cmp)(const rd_kafka_message_t *a,
                         const rd_kafka_message_t *b)) {
        rd_kafka_anyconf_set_internal(_RK_TOPIC, topic_conf, "msg_order_cmp",
                                      msg_order_cmp);
}

void rd_kafka_topic_conf_set_opaque(rd_kafka_topic_conf_t *topic_conf,
                                    void *opaque) {
        rd_kafka_anyconf_set_internal(_RK_TOPIC, topic_conf, "opaque", opaque);
}



/**
 * @brief Convert flags \p ival to csv-string using S2F property \p prop.
 *
 * This function has two modes: size query and write.
 * To query for needed size call with dest==NULL,
 * to write to buffer of size dest_size call with dest!=NULL.
 *
 * An \p ival of -1 means all.
 *
 * @param include_unsupported Include flag values that are unsupported
 *                            due to missing dependencies at build time.
 *
 * @returns the number of bytes written to \p dest (if not NULL), else the
 *          total number of bytes needed.
 *
 */
static size_t rd_kafka_conf_flags2str(char *dest,
                                      size_t dest_size,
                                      const char *delim,
                                      const struct rd_kafka_property *prop,
                                      int ival,
                                      rd_bool_t include_unsupported) {
        size_t of = 0;
        int j;

        if (dest && dest_size > 0)
                *dest = '\0';

        /* Phase 1: scan for set flags, accumulate needed size.
         * Phase 2: write to dest */
        for (j = 0; j < (int)RD_ARRAYSIZE(prop->s2i) && prop->s2i[j].str; j++) {
                if (prop->type == _RK_C_S2F && ival != -1 &&
                    (ival & prop->s2i[j].val) != prop->s2i[j].val)
                        continue;
                else if (prop->type == _RK_C_S2I && ival != -1 &&
                         prop->s2i[j].val != ival)
                        continue;
                else if (prop->s2i[j].unsupported && !include_unsupported)
                        continue;

                if (!dest)
                        of += strlen(prop->s2i[j].str) + (of > 0 ? 1 : 0);
                else {
                        size_t r;
                        r = rd_snprintf(dest + of, dest_size - of, "%s%s",
                                        of > 0 ? delim : "", prop->s2i[j].str);
                        if (r > dest_size - of) {
                                r = dest_size - of;
                                break;
                        }
                        of += r;
                }
        }

        return of + 1 /*nul*/;
}


/**
 * Return "original"(re-created) configuration value string
 */
static rd_kafka_conf_res_t
rd_kafka_anyconf_get0(const void *conf,
                      const struct rd_kafka_property *prop,
                      char *dest,
                      size_t *dest_size) {
        char tmp[22];
        const char *val = NULL;
        size_t val_len  = 0;
        int j;

        switch (prop->type) {
        case _RK_C_STR:
                val = *_RK_PTR(const char **, conf, prop->offset);
                break;

        case _RK_C_KSTR: {
                const rd_kafkap_str_t **kstr =
                    _RK_PTR(const rd_kafkap_str_t **, conf, prop->offset);
                if (*kstr)
                        val = (*kstr)->str;
                break;
        }

        case _RK_C_PTR:
                val = *_RK_PTR(const void **, conf, prop->offset);
                if (val) {
                        rd_snprintf(tmp, sizeof(tmp), "%p", (void *)val);
                        val = tmp;
                }
                break;

        case _RK_C_BOOL:
                val = (*_RK_PTR(int *, conf, prop->offset) ? "true" : "false");
                break;

        case _RK_C_INT:
                rd_snprintf(tmp, sizeof(tmp), "%i",
                            *_RK_PTR(int *, conf, prop->offset));
                val = tmp;
                break;

        case _RK_C_DBL:
                rd_snprintf(tmp, sizeof(tmp), "%g",
                            *_RK_PTR(double *, conf, prop->offset));
                val = tmp;
                break;

        case _RK_C_S2I:
                for (j = 0; j < (int)RD_ARRAYSIZE(prop->s2i); j++) {
                        if (prop->s2i[j].val ==
                            *_RK_PTR(int *, conf, prop->offset)) {
                                val = prop->s2i[j].str;
                                break;
                        }
                }
                break;

        case _RK_C_S2F: {
                const int ival = *_RK_PTR(const int *, conf, prop->offset);

                val_len = rd_kafka_conf_flags2str(dest, dest ? *dest_size : 0,
                                                  ",", prop, ival,
                                                  rd_false /*only supported*/);
                if (dest) {
                        val_len = 0;
                        val     = dest;
                        dest    = NULL;
                }
                break;
        }

        case _RK_C_PATLIST: {
                const rd_kafka_pattern_list_t **plist;
                plist = _RK_PTR(const rd_kafka_pattern_list_t **, conf,
                                prop->offset);
                if (*plist)
                        val = (*plist)->rkpl_orig;
                break;
        }

        default:
                break;
        }

        if (val_len) {
                *dest_size = val_len + 1;
                return RD_KAFKA_CONF_OK;
        }

        if (!val)
                return RD_KAFKA_CONF_INVALID;

        val_len = strlen(val);

        if (dest) {
                size_t use_len = RD_MIN(val_len, (*dest_size) - 1);
                memcpy(dest, val, use_len);
                dest[use_len] = '\0';
        }

        /* Return needed size */
        *dest_size = val_len + 1;

        return RD_KAFKA_CONF_OK;
}


static rd_kafka_conf_res_t rd_kafka_anyconf_get(int scope,
                                                const void *conf,
                                                const char *name,
                                                char *dest,
                                                size_t *dest_size) {
        const struct rd_kafka_property *prop;

        for (prop = rd_kafka_properties; prop->name; prop++) {

                if (!(prop->scope & scope) || strcmp(prop->name, name))
                        continue;

                if (prop->type == _RK_C_ALIAS)
                        return rd_kafka_anyconf_get(scope, conf, prop->sdef,
                                                    dest, dest_size);

                if (rd_kafka_anyconf_get0(conf, prop, dest, dest_size) ==
                    RD_KAFKA_CONF_OK)
                        return RD_KAFKA_CONF_OK;
        }

        return RD_KAFKA_CONF_UNKNOWN;
}

rd_kafka_conf_res_t rd_kafka_topic_conf_get(const rd_kafka_topic_conf_t *conf,
                                            const char *name,
                                            char *dest,
                                            size_t *dest_size) {
        return rd_kafka_anyconf_get(_RK_TOPIC, conf, name, dest, dest_size);
}

rd_kafka_conf_res_t rd_kafka_conf_get(const rd_kafka_conf_t *conf,
                                      const char *name,
                                      char *dest,
                                      size_t *dest_size) {
        rd_kafka_conf_res_t res;
        res = rd_kafka_anyconf_get(_RK_GLOBAL, conf, name, dest, dest_size);
        if (res != RD_KAFKA_CONF_UNKNOWN || !conf->topic_conf)
                return res;

        /* Fallthru:
         * If the global property was unknown, try getting it from the
         * default topic config, if any. */
        return rd_kafka_topic_conf_get(conf->topic_conf, name, dest, dest_size);
}


static const char **rd_kafka_anyconf_dump(int scope,
                                          const void *conf,
                                          size_t *cntp,
                                          rd_bool_t only_modified,
                                          rd_bool_t redact_sensitive) {
        const struct rd_kafka_property *prop;
        char **arr;
        int cnt = 0;

        arr = rd_calloc(sizeof(char *), RD_ARRAYSIZE(rd_kafka_properties) * 2);

        for (prop = rd_kafka_properties; prop->name; prop++) {
                char *val = NULL;
                size_t val_size;

                if (!(prop->scope & scope))
                        continue;

                if (only_modified && !rd_kafka_anyconf_is_modified(conf, prop))
                        continue;

                /* Skip aliases, show original property instead.
                 * Skip invalids. */
                if (prop->type == _RK_C_ALIAS || prop->type == _RK_C_INVALID)
                        continue;

                if (redact_sensitive && (prop->scope & _RK_SENSITIVE)) {
                        val = rd_strdup("[redacted]");
                } else {
                        /* Query value size */
                        if (rd_kafka_anyconf_get0(conf, prop, NULL,
                                                  &val_size) !=
                            RD_KAFKA_CONF_OK)
                                continue;

                        /* Get value */
                        val = rd_malloc(val_size);
                        rd_kafka_anyconf_get0(conf, prop, val, &val_size);
                }

                arr[cnt++] = rd_strdup(prop->name);
                arr[cnt++] = val;
        }

        *cntp = cnt;

        return (const char **)arr;
}


const char **rd_kafka_conf_dump(rd_kafka_conf_t *conf, size_t *cntp) {
        return rd_kafka_anyconf_dump(_RK_GLOBAL, conf, cntp, rd_false /*all*/,
                                     rd_false /*don't redact*/);
}

const char **rd_kafka_topic_conf_dump(rd_kafka_topic_conf_t *conf,
                                      size_t *cntp) {
        return rd_kafka_anyconf_dump(_RK_TOPIC, conf, cntp, rd_false /*all*/,
                                     rd_false /*don't redact*/);
}

void rd_kafka_conf_dump_free(const char **arr, size_t cnt) {
        char **_arr = (char **)arr;
        unsigned int i;

        for (i = 0; i < cnt; i++)
                if (_arr[i])
                        rd_free(_arr[i]);

        rd_free(_arr);
}



/**
 * @brief Dump configured properties to debug log.
 */
void rd_kafka_anyconf_dump_dbg(rd_kafka_t *rk,
                               int scope,
                               const void *conf,
                               const char *description) {
        const char **arr;
        size_t cnt;
        size_t i;

        arr =
            rd_kafka_anyconf_dump(scope, conf, &cnt, rd_true /*modified only*/,
                                  rd_true /*redact sensitive*/);
        if (cnt > 0)
                rd_kafka_dbg(rk, CONF, "CONF", "%s:", description);
        for (i = 0; i < cnt; i += 2)
                rd_kafka_dbg(rk, CONF, "CONF", "  %s = %s", arr[i], arr[i + 1]);

        rd_kafka_conf_dump_free(arr, cnt);
}

void rd_kafka_conf_properties_show(FILE *fp) {
        const struct rd_kafka_property *prop0;
        int last = 0;
        int j;
        char tmp[512];
        const char *dash80 =
            "----------------------------------------"
            "----------------------------------------";

        for (prop0 = rd_kafka_properties; prop0->name; prop0++) {
                const char *typeinfo = "";
                const char *importance;
                const struct rd_kafka_property *prop = prop0;

                /* Skip hidden properties. */
                if (prop->scope & _RK_HIDDEN)
                        continue;

                /* Skip invalid properties. */
                if (prop->type == _RK_C_INVALID)
                        continue;

                if (!(prop->scope & last)) {
                        fprintf(fp, "%s## %s configuration properties\n\n",
                                last ? "\n\n" : "",
                                prop->scope == _RK_GLOBAL ? "Global" : "Topic");

                        fprintf(fp,
                                "%-40s | %3s | %-15s | %13s | %-10s | %-25s\n"
                                "%.*s-|-%.*s-|-%.*s-|-%.*s:|-%.*s-| -%.*s\n",
                                "Property", "C/P", "Range", "Default",
                                "Importance", "Description", 40, dash80, 3,
                                dash80, 15, dash80, 13, dash80, 10, dash80, 25,
                                dash80);

                        last = prop->scope & (_RK_GLOBAL | _RK_TOPIC);
                }

                fprintf(fp, "%-40s | ", prop->name);

                /* For aliases, use the aliased property from here on
                 * so that the alias property shows up with proper
                 * ranges, defaults, etc. */
                if (prop->type == _RK_C_ALIAS) {
                        prop = rd_kafka_conf_prop_find(prop->scope, prop->sdef);
                        rd_assert(prop && *"BUG: "
                                  "alias points to unknown config property");
                }

                fprintf(fp, "%3s | ",
                        (!(prop->scope & _RK_PRODUCER) ==
                                 !(prop->scope & _RK_CONSUMER)
                             ? " * "
                             : ((prop->scope & _RK_PRODUCER) ? " P " : " C ")));

                switch (prop->type) {
                case _RK_C_STR:
                case _RK_C_KSTR:
                        typeinfo = "string";
                case _RK_C_PATLIST:
                        if (prop->type == _RK_C_PATLIST)
                                typeinfo = "pattern list";
                        if (prop->s2i[0].str) {
                                rd_kafka_conf_flags2str(
                                    tmp, sizeof(tmp), ", ", prop, -1,
                                    rd_true /*include unsupported*/);
                                fprintf(fp, "%-15s | %13s", tmp,
                                        prop->sdef ? prop->sdef : "");
                        } else {
                                fprintf(fp, "%-15s | %13s", "",
                                        prop->sdef ? prop->sdef : "");
                        }
                        break;
                case _RK_C_BOOL:
                        typeinfo = "boolean";
                        fprintf(fp, "%-15s | %13s", "true, false",
                                prop->vdef ? "true" : "false");
                        break;
                case _RK_C_INT:
                        typeinfo = "integer";
                        rd_snprintf(tmp, sizeof(tmp), "%d .. %d", prop->vmin,
                                    prop->vmax);
                        fprintf(fp, "%-15s | %13i", tmp, prop->vdef);
                        break;
                case _RK_C_DBL:
                        typeinfo = "float"; /* more user-friendly than double */
                        rd_snprintf(tmp, sizeof(tmp), "%g .. %g", prop->dmin,
                                    prop->dmax);
                        fprintf(fp, "%-15s | %13g", tmp, prop->ddef);
                        break;
                case _RK_C_S2I:
                        typeinfo = "enum value";
                        rd_kafka_conf_flags2str(
                            tmp, sizeof(tmp), ", ", prop, -1,
                            rd_true /*include unsupported*/);
                        fprintf(fp, "%-15s | ", tmp);

                        for (j = 0; j < (int)RD_ARRAYSIZE(prop->s2i); j++) {
                                if (prop->s2i[j].val == prop->vdef) {
                                        fprintf(fp, "%13s", prop->s2i[j].str);
                                        break;
                                }
                        }
                        if (j == RD_ARRAYSIZE(prop->s2i))
                                fprintf(fp, "%13s", " ");
                        break;

                case _RK_C_S2F:
                        typeinfo = "CSV flags";
                        /* Dont duplicate builtin.features value in
                         * both Range and Default */
                        if (!strcmp(prop->name, "builtin.features"))
                                *tmp = '\0';
                        else
                                rd_kafka_conf_flags2str(
                                    tmp, sizeof(tmp), ", ", prop, -1,
                                    rd_true /*include unsupported*/);
                        fprintf(fp, "%-15s | ", tmp);
                        rd_kafka_conf_flags2str(
                            tmp, sizeof(tmp), ", ", prop, prop->vdef,
                            rd_true /*include unsupported*/);
                        fprintf(fp, "%13s", tmp);

                        break;
                case _RK_C_PTR:
                case _RK_C_INTERNAL:
                        typeinfo = "see dedicated API";
                        /* FALLTHRU */
                default:
                        fprintf(fp, "%-15s | %-13s", "", " ");
                        break;
                }

                if (prop->scope & _RK_HIGH)
                        importance = "high";
                else if (prop->scope & _RK_MED)
                        importance = "medium";
                else
                        importance = "low";

                fprintf(fp, " | %-10s | ", importance);

                if (prop->scope & _RK_EXPERIMENTAL)
                        fprintf(fp,
                                "**EXPERIMENTAL**: "
                                "subject to change or removal. ");

                if (prop->scope & _RK_DEPRECATED)
                        fprintf(fp, "**DEPRECATED** ");

                /* If the original property is an alias, prefix the
                 * description saying so. */
                if (prop0->type == _RK_C_ALIAS)
                        fprintf(fp, "Alias for `%s`: ", prop0->sdef);

                fprintf(fp, "%s <br>*Type: %s*\n", prop->desc, typeinfo);
        }
        fprintf(fp, "\n");
        fprintf(fp, "### C/P legend: C = Consumer, P = Producer, * = both\n");
}



/**
 * @name Configuration value methods
 *
 * @remark This generic interface will eventually replace the config property
 *         used above.
 * @{
 */


/**
 * @brief Set up an INT confval.
 *
 * @oaram name Property name, must be a const static string (will not be copied)
 */
void rd_kafka_confval_init_int(rd_kafka_confval_t *confval,
                               const char *name,
                               int vmin,
                               int vmax,
                               int vdef) {
        confval->name       = name;
        confval->is_enabled = 1;
        confval->valuetype  = RD_KAFKA_CONFVAL_INT;
        confval->u.INT.vmin = vmin;
        confval->u.INT.vmax = vmax;
        confval->u.INT.vdef = vdef;
        confval->u.INT.v    = vdef;
}

/**
 * @brief Set up a PTR confval.
 *
 * @oaram name Property name, must be a const static string (will not be copied)
 */
void rd_kafka_confval_init_ptr(rd_kafka_confval_t *confval, const char *name) {
        confval->name       = name;
        confval->is_enabled = 1;
        confval->valuetype  = RD_KAFKA_CONFVAL_PTR;
        confval->u.PTR      = NULL;
}

/**
 * @brief Set up but disable an intval, attempt to set this confval will fail.
 *
 * @oaram name Property name, must be a const static string (will not be copied)
 */
void rd_kafka_confval_disable(rd_kafka_confval_t *confval, const char *name) {
        confval->name       = name;
        confval->is_enabled = 0;
}

/**
 * @brief Set confval's value to \p valuep, verifying the passed
 *        \p valuetype matches (or can be cast to) \p confval's type.
 *
 * @param dispname is the display name for the configuration value and is
 *        included in error strings.
 * @param valuep is a pointer to the value, or NULL to revert to default.
 *
 * @returns RD_KAFKA_RESP_ERR_NO_ERROR if the new value was set, or
 *          RD_KAFKA_RESP_ERR__INVALID_ARG if the value was of incorrect type,
 *          out of range, or otherwise not a valid value.
 */
rd_kafka_resp_err_t rd_kafka_confval_set_type(rd_kafka_confval_t *confval,
                                              rd_kafka_confval_type_t valuetype,
                                              const void *valuep,
                                              char *errstr,
                                              size_t errstr_size) {

        if (!confval->is_enabled) {
                rd_snprintf(errstr, errstr_size,
                            "\"%s\" is not supported for this operation",
                            confval->name);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        switch (confval->valuetype) {
        case RD_KAFKA_CONFVAL_INT: {
                int v;
                const char *end;

                if (!valuep) {
                        /* Revert to default */
                        confval->u.INT.v = confval->u.INT.vdef;
                        confval->is_set  = 0;
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                }

                switch (valuetype) {
                case RD_KAFKA_CONFVAL_INT:
                        v = *(const int *)valuep;
                        break;
                case RD_KAFKA_CONFVAL_STR:
                        v = (int)strtol((const char *)valuep, (char **)&end, 0);
                        if (end == (const char *)valuep) {
                                rd_snprintf(errstr, errstr_size,
                                            "Invalid value type for \"%s\": "
                                            "expecting integer",
                                            confval->name);
                                return RD_KAFKA_RESP_ERR__INVALID_TYPE;
                        }
                        break;
                default:
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value type for \"%s\": "
                                    "expecting integer",
                                    confval->name);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }


                if ((confval->u.INT.vmin || confval->u.INT.vmax) &&
                    (v < confval->u.INT.vmin || v > confval->u.INT.vmax)) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value type for \"%s\": "
                                    "expecting integer in range %d..%d",
                                    confval->name, confval->u.INT.vmin,
                                    confval->u.INT.vmax);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }

                confval->u.INT.v = v;
                confval->is_set  = 1;
        } break;

        case RD_KAFKA_CONFVAL_STR: {
                size_t vlen;
                const char *v = (const char *)valuep;

                if (!valuep) {
                        confval->is_set = 0;
                        if (confval->u.STR.vdef)
                                confval->u.STR.v =
                                    rd_strdup(confval->u.STR.vdef);
                        else
                                confval->u.STR.v = NULL;
                }

                if (valuetype != RD_KAFKA_CONFVAL_STR) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value type for \"%s\": "
                                    "expecting string",
                                    confval->name);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }

                vlen = strlen(v);
                if ((confval->u.STR.minlen || confval->u.STR.maxlen) &&
                    (vlen < confval->u.STR.minlen ||
                     vlen > confval->u.STR.maxlen)) {
                        rd_snprintf(errstr, errstr_size,
                                    "Invalid value for \"%s\": "
                                    "expecting string with length "
                                    "%" PRIusz "..%" PRIusz,
                                    confval->name, confval->u.STR.minlen,
                                    confval->u.STR.maxlen);
                        return RD_KAFKA_RESP_ERR__INVALID_ARG;
                }

                if (confval->u.STR.v)
                        rd_free(confval->u.STR.v);

                confval->u.STR.v = rd_strdup(v);
        } break;

        case RD_KAFKA_CONFVAL_PTR:
                confval->u.PTR = (void *)valuep;
                break;

        default:
                RD_NOTREACHED();
                return RD_KAFKA_RESP_ERR__NOENT;
        }

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


int rd_kafka_confval_get_int(const rd_kafka_confval_t *confval) {
        rd_assert(confval->valuetype == RD_KAFKA_CONFVAL_INT);
        return confval->u.INT.v;
}


const char *rd_kafka_confval_get_str(const rd_kafka_confval_t *confval) {
        rd_assert(confval->valuetype == RD_KAFKA_CONFVAL_STR);
        return confval->u.STR.v;
}

void *rd_kafka_confval_get_ptr(const rd_kafka_confval_t *confval) {
        rd_assert(confval->valuetype == RD_KAFKA_CONFVAL_PTR);
        return confval->u.PTR;
}


#define _is_alphanum(C)                                                        \
        (((C) >= 'a' && (C) <= 'z') || ((C) >= 'A' && (C) <= 'Z') ||           \
         ((C) >= '0' && (C) <= '9'))

/**
 * @returns true if the string is KIP-511 safe, else false.
 */
static rd_bool_t rd_kafka_sw_str_is_safe(const char *str) {
        const char *s;

        if (!*str)
                return rd_true;

        for (s = str; *s; s++) {
                int c = (int)*s;

                if (unlikely(!(_is_alphanum(c) || c == '-' || c == '.')))
                        return rd_false;
        }

        /* Verify that the string begins and ends with a-zA-Z0-9 */
        if (!_is_alphanum(*str))
                return rd_false;
        if (!_is_alphanum(*(s - 1)))
                return rd_false;

        return rd_true;
}


/**
 * @brief Sanitize KIP-511 software name/version strings in-place,
 *        replacing unaccepted characters with "-".
 *
 * @warning The \p str is modified in-place.
 */
static void rd_kafka_sw_str_sanitize_inplace(char *str) {
        char *s = str, *d = str;

        /* Strip any leading non-alphanums */
        while (!_is_alphanum(*s))
                s++;

        for (; *s; s++) {
                int c = (int)*s;

                if (unlikely(!(_is_alphanum(c) || c == '-' || c == '.')))
                        *d = '-';
                else
                        *d = *s;
                d++;
        }

        *d = '\0';

        /* Strip any trailing non-alphanums */
        for (d = d - 1; d >= str && !_is_alphanum(*d); d--)
                *d = '\0';
}

#undef _is_alphanum


/**
 * @brief Create a staggered array of key-value pairs from
 *        an array of "key=value" strings (typically from rd_string_split()).
 *
 * The output array will have element 0 being key0 and element 1 being
 * value0. Element 2 being key1 and element 3 being value1, and so on.
 * E.g.:
 *  input   { "key0=value0", "key1=value1" } incnt=2
 *  returns { "key0", "value0", "key1", "value1" } cntp=4
 *
 * @returns NULL on error (no '=' separator), or a newly allocated array
 *          on success. The array count is returned in \p cntp.
 *          The returned pointer must be freed with rd_free().
 */
char **rd_kafka_conf_kv_split(const char **input, size_t incnt, size_t *cntp) {
        size_t i;
        char **out, *p;
        size_t lens   = 0;
        size_t outcnt = 0;

        /* First calculate total length needed for key-value strings. */
        for (i = 0; i < incnt; i++) {
                const char *t = strchr(input[i], '=');

                /* No "=", or "=" at beginning of string. */
                if (!t || t == input[i])
                        return NULL;

                /* Length of key, '=' (will be \0), value, and \0 */
                lens += strlen(input[i]) + 1;
        }

        /* Allocate array along with elements in one go */
        out = rd_malloc((sizeof(*out) * incnt * 2) + lens);
        p   = (char *)(&out[incnt * 2]);

        for (i = 0; i < incnt; i++) {
                const char *t   = strchr(input[i], '=');
                size_t namelen  = (size_t)(t - input[i]);
                size_t valuelen = strlen(t + 1);

                /* Copy name */
                out[outcnt++] = p;
                memcpy(p, input[i], namelen);
                p += namelen;
                *(p++) = '\0';

                /* Copy value */
                out[outcnt++] = p;
                memcpy(p, t + 1, valuelen + 1);
                p += valuelen;
                *(p++) = '\0';
        }


        *cntp = outcnt;
        return out;
}


/**
 * @brief Verify configuration \p conf is
 *        correct/non-conflicting and finalize the configuration
 *        settings for use.
 *
 * @returns an error string if configuration is incorrect, else NULL.
 */
const char *rd_kafka_conf_finalize(rd_kafka_type_t cltype,
                                   rd_kafka_conf_t *conf) {
        const char *errstr;

        if (!conf->sw_name)
                rd_kafka_conf_set(conf, "client.software.name", "librdkafka",
                                  NULL, 0);
        if (!conf->sw_version)
                rd_kafka_conf_set(conf, "client.software.version",
                                  rd_kafka_version_str(), NULL, 0);

        /* The client.software.name and .version are sent to the broker
         * with the ApiVersionRequest starting with AK 2.4.0 (KIP-511).
         * These strings need to be sanitized or the broker will reject them,
         * so modify them in-place here. */
        rd_assert(conf->sw_name && conf->sw_version);
        rd_kafka_sw_str_sanitize_inplace(conf->sw_name);
        rd_kafka_sw_str_sanitize_inplace(conf->sw_version);

        /* Verify mandatory configuration */
        if (!conf->socket_cb)
                return "Mandatory config property `socket_cb` not set";

        if (!conf->open_cb)
                return "Mandatory config property `open_cb` not set";

#if WITH_SSL
        if (conf->ssl.keystore_location && !conf->ssl.keystore_password)
                return "`ssl.keystore.password` is mandatory when "
                       "`ssl.keystore.location` is set";
        if (conf->ssl.ca && (conf->ssl.ca_location || conf->ssl.ca_pem))
                return "`ssl.ca.location` or `ssl.ca.pem`, and memory-based "
                       "set_ssl_cert(CERT_CA) are mutually exclusive.";
#ifdef __APPLE__
        else if (!conf->ssl.ca && !conf->ssl.ca_location && !conf->ssl.ca_pem)
                /* Default ssl.ca.location to 'probe' on OSX */
                rd_kafka_conf_set(conf, "ssl.ca.location", "probe", NULL, 0);
#endif
#endif

#if WITH_SASL_OAUTHBEARER
        if (!rd_strcasecmp(conf->sasl.mechanisms, "OAUTHBEARER")) {
                if (conf->sasl.enable_oauthbearer_unsecure_jwt &&
                    conf->sasl.oauthbearer.token_refresh_cb)
                        return "`enable.sasl.oauthbearer.unsecure.jwt` and "
                               "`oauthbearer_token_refresh_cb` are "
                               "mutually exclusive";

                if (conf->sasl.enable_oauthbearer_unsecure_jwt &&
                    conf->sasl.oauthbearer.method ==
                        RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC)
                        return "`enable.sasl.oauthbearer.unsecure.jwt` and "
                               "`sasl.oauthbearer.method=oidc` are "
                               "mutually exclusive";

                if (conf->sasl.oauthbearer.method ==
                    RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC) {
                        if (!conf->sasl.oauthbearer.client_id)
                                return "`sasl.oauthbearer.client.id` is "
                                       "mandatory when "
                                       "`sasl.oauthbearer.method=oidc` is set";

                        if (!conf->sasl.oauthbearer.client_secret) {
                                return "`sasl.oauthbearer.client.secret` is "
                                       "mandatory when "
                                       "`sasl.oauthbearer.method=oidc` is set";
                        }

                        if (!conf->sasl.oauthbearer.token_endpoint_url) {
                                return "`sasl.oauthbearer.token.endpoint.url` "
                                       "is mandatory when "
                                       "`sasl.oauthbearer.method=oidc` is set";
                        }
                }

                /* Enable background thread for the builtin OIDC handler,
                 * unless a refresh callback has been set. */
                if (conf->sasl.oauthbearer.method ==
                        RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC &&
                    !conf->sasl.oauthbearer.token_refresh_cb) {
                        conf->enabled_events |= RD_KAFKA_EVENT_BACKGROUND;
                        conf->sasl.enable_callback_queue = 1;
                }
        }

#endif

        if (cltype == RD_KAFKA_CONSUMER) {

                /* Automatically adjust `fetch.max.bytes` to be >=
                 * `message.max.bytes` and <= `queued.max.message.kbytes`
                 * unless set by user. */
                if (rd_kafka_conf_is_modified(conf, "fetch.max.bytes")) {
                        if (conf->fetch_max_bytes < conf->max_msg_size)
                                return "`fetch.max.bytes` must be >= "
                                       "`message.max.bytes`";
                } else {
                        conf->fetch_max_bytes =
                            RD_MAX(RD_MIN(conf->fetch_max_bytes,
                                          conf->queued_max_msg_kbytes * 1024),
                                   conf->max_msg_size);
                }

                /* Automatically adjust 'receive.message.max.bytes' to
                 * be 512 bytes larger than 'fetch.max.bytes' to have enough
                 * room for protocol framing (including topic name), unless
                 * set by user. */
                if (rd_kafka_conf_is_modified(conf,
                                              "receive.message.max.bytes")) {
                        if (conf->fetch_max_bytes + 512 >
                            conf->recv_max_msg_size)
                                return "`receive.message.max.bytes` must be >= "
                                       "`fetch.max.bytes` + 512";
                } else {
                        conf->recv_max_msg_size =
                            RD_MAX(conf->recv_max_msg_size,
                                   conf->fetch_max_bytes + 512);
                }

                if (conf->max_poll_interval_ms < conf->group_session_timeout_ms)
                        return "`max.poll.interval.ms`must be >= "
                               "`session.timeout.ms`";

                /* Simplifies rd_kafka_is_idempotent() which is producer-only */
                conf->eos.idempotence = 0;

        } else if (cltype == RD_KAFKA_PRODUCER) {
                if (conf->eos.transactional_id) {
                        if (!conf->eos.idempotence) {
                                /* Auto enable idempotence unless
                                 * explicitly disabled */
                                if (rd_kafka_conf_is_modified(
                                        conf, "enable.idempotence"))
                                        return "`transactional.id` requires "
                                               "`enable.idempotence=true`";

                                conf->eos.idempotence = rd_true;
                        }

                        /* Make sure at least one request can be sent
                         * before the transaction times out. */
                        if (!rd_kafka_conf_is_modified(conf,
                                                       "socket.timeout.ms"))
                                conf->socket_timeout_ms = RD_MAX(
                                    conf->eos.transaction_timeout_ms - 100,
                                    900);
                        else if (conf->eos.transaction_timeout_ms + 100 <
                                 conf->socket_timeout_ms)
                                return "`socket.timeout.ms` must be set <= "
                                       "`transaction.timeout.ms` + 100";
                }

                if (conf->eos.idempotence) {
                        /* Adjust configuration values for idempotent producer*/

                        if (rd_kafka_conf_is_modified(conf, "max.in.flight")) {
                                if (conf->max_inflight >
                                    RD_KAFKA_IDEMP_MAX_INFLIGHT)
                                        return "`max.in.flight` must be "
                                               "set "
                                               "<="
                                               " " RD_KAFKA_IDEMP_MAX_INFLIGHT_STR
                                               " when `enable.idempotence` "
                                               "is true";
                        } else {
                                conf->max_inflight =
                                    RD_MIN(conf->max_inflight,
                                           RD_KAFKA_IDEMP_MAX_INFLIGHT);
                        }


                        if (rd_kafka_conf_is_modified(conf, "retries")) {
                                if (conf->max_retries < 1)
                                        return "`retries` must be set >= 1 "
                                               "when `enable.idempotence` is "
                                               "true";
                        } else {
                                conf->max_retries = INT32_MAX;
                        }


                        if (rd_kafka_conf_is_modified(
                                conf,
                                "queue.buffering.backpressure.threshold") &&
                            conf->queue_backpressure_thres > 1)
                                return "`queue.buffering.backpressure."
                                       "threshold` "
                                       "must be set to 1 when "
                                       "`enable.idempotence` is true";
                        else
                                conf->queue_backpressure_thres = 1;

                        /* acks=all and queuing.strategy are set
                         * in topic_conf_finalize() */

                } else {
                        if (conf->eos.gapless &&
                            rd_kafka_conf_is_modified(
                                conf, "enable.gapless.guarantee"))
                                return "`enable.gapless.guarantee` requires "
                                       "`enable.idempotence` to be enabled";
                }

                if (!rd_kafka_conf_is_modified(conf,
                                               "sticky.partitioning.linger.ms"))
                        conf->sticky_partition_linger_ms = (int)RD_MIN(
                            900000, (rd_ts_t)(2 * conf->buffering_max_ms_dbl));
        }


        if (!rd_kafka_conf_is_modified(conf, "metadata.max.age.ms") &&
            conf->metadata_refresh_interval_ms > 0)
                conf->metadata_max_age_ms =
                    conf->metadata_refresh_interval_ms * 3;

        if (conf->reconnect_backoff_max_ms < conf->reconnect_backoff_ms)
                return "`reconnect.backoff.max.ms` must be >= "
                       "`reconnect.max.ms`";

        if (conf->sparse_connections) {
                /* Set sparse connection random selection interval to
                 * 10 < reconnect.backoff.ms / 2 < 1000. */
                conf->sparse_connect_intvl =
                    RD_MAX(11, RD_MIN(conf->reconnect_backoff_ms / 2, 1000));
        }

        if (!rd_kafka_conf_is_modified(conf, "connections.max.idle.ms") &&
            conf->brokerlist && rd_strcasestr(conf->brokerlist, "azure")) {
                /* Issue #3109:
                 * Default connections.max.idle.ms to <4 minutes on Azure. */
                conf->connections_max_idle_ms = (4 * 60 - 10) * 1000;
        }

        if (!rd_kafka_conf_is_modified(conf, "allow.auto.create.topics")) {
                /* Consumer: Do not allow auto create by default.
                 * Producer: Allow auto create by default. */
                if (cltype == RD_KAFKA_CONSUMER)
                        conf->allow_auto_create_topics = rd_false;
                else if (cltype == RD_KAFKA_PRODUCER)
                        conf->allow_auto_create_topics = rd_true;
        }

        /* Finalize and verify the default.topic.config */
        if (conf->topic_conf) {

                if (cltype == RD_KAFKA_PRODUCER) {
                        rd_kafka_topic_conf_t *tconf = conf->topic_conf;

                        if (tconf->message_timeout_ms != 0 &&
                            (double)tconf->message_timeout_ms <=
                                conf->buffering_max_ms_dbl) {
                                if (rd_kafka_conf_is_modified(conf,
                                                              "linger.ms"))
                                        return "`message.timeout.ms` must be "
                                               "greater than `linger.ms`";
                                else /* Auto adjust linger.ms to be lower
                                      * than message.timeout.ms */
                                        conf->buffering_max_ms_dbl =
                                            (double)tconf->message_timeout_ms -
                                            0.1;
                        }
                }

                errstr = rd_kafka_topic_conf_finalize(cltype, conf,
                                                      conf->topic_conf);
                if (errstr)
                        return errstr;
        }

        /* Convert double linger.ms to internal int microseconds after
         * finalizing default_topic_conf since it may
         * update buffering_max_ms_dbl. */
        conf->buffering_max_us = (rd_ts_t)(conf->buffering_max_ms_dbl * 1000);


        return NULL;
}


/**
 * @brief Verify topic configuration \p tconf is
 *        correct/non-conflicting and finalize the configuration
 *        settings for use.
 *
 * @returns an error string if configuration is incorrect, else NULL.
 */
const char *rd_kafka_topic_conf_finalize(rd_kafka_type_t cltype,
                                         const rd_kafka_conf_t *conf,
                                         rd_kafka_topic_conf_t *tconf) {

        if (cltype != RD_KAFKA_PRODUCER)
                return NULL;

        if (conf->eos.idempotence) {
                /* Ensure acks=all */
                if (rd_kafka_topic_conf_is_modified(tconf, "acks")) {
                        if (tconf->required_acks != -1)
                                return "`acks` must be set to `all` when "
                                       "`enable.idempotence` is true";
                } else {
                        tconf->required_acks = -1; /* all */
                }

                /* Ensure FIFO queueing */
                if (rd_kafka_topic_conf_is_modified(tconf,
                                                    "queuing.strategy")) {
                        if (tconf->queuing_strategy != RD_KAFKA_QUEUE_FIFO)
                                return "`queuing.strategy` must be set to "
                                       "`fifo` when `enable.idempotence` is "
                                       "true";
                } else {
                        tconf->queuing_strategy = RD_KAFKA_QUEUE_FIFO;
                }

                /* Ensure message.timeout.ms <= transaction.timeout.ms */
                if (conf->eos.transactional_id) {
                        if (!rd_kafka_topic_conf_is_modified(
                                tconf, "message.timeout.ms"))
                                tconf->message_timeout_ms =
                                    conf->eos.transaction_timeout_ms;
                        else if (tconf->message_timeout_ms >
                                 conf->eos.transaction_timeout_ms)
                                return "`message.timeout.ms` must be set <= "
                                       "`transaction.timeout.ms`";
                }
        }

        if (tconf->message_timeout_ms != 0 &&
            (double)tconf->message_timeout_ms <= conf->buffering_max_ms_dbl &&
            rd_kafka_conf_is_modified(conf, "linger.ms"))
                return "`message.timeout.ms` must be greater than `linger.ms`";

        return NULL;
}


/**
 * @brief Log warnings for set deprecated or experimental
 *        configuration properties.
 * @returns the number of warnings logged.
 */
static int rd_kafka_anyconf_warn_deprecated(rd_kafka_t *rk,
                                            rd_kafka_conf_scope_t scope,
                                            const void *conf) {
        const struct rd_kafka_property *prop;
        int warn_type =
            rk->rk_type == RD_KAFKA_PRODUCER ? _RK_CONSUMER : _RK_PRODUCER;
        int warn_on = _RK_DEPRECATED | _RK_EXPERIMENTAL | warn_type;

        int cnt = 0;

        for (prop = rd_kafka_properties; prop->name; prop++) {
                int match = prop->scope & warn_on;

                if (likely(!(prop->scope & scope) || !match))
                        continue;

                if (likely(!rd_kafka_anyconf_is_modified(conf, prop)))
                        continue;

                if (match != warn_type)
                        rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                                     "Configuration property %s is %s%s%s: %s",
                                     prop->name,
                                     match & _RK_DEPRECATED ? "deprecated" : "",
                                     match == warn_on ? " and " : "",
                                     match & _RK_EXPERIMENTAL ? "experimental"
                                                              : "",
                                     prop->desc);

                if (match & warn_type)
                        rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                                     "Configuration property %s "
                                     "is a %s property and will be ignored by "
                                     "this %s instance",
                                     prop->name,
                                     warn_type == _RK_PRODUCER ? "producer"
                                                               : "consumer",
                                     warn_type == _RK_PRODUCER ? "consumer"
                                                               : "producer");

                cnt++;
        }

        return cnt;
}


/**
 * @brief Log configuration warnings (deprecated configuration properties,
 *        unrecommended combinations, etc).
 *
 * @returns the number of warnings logged.
 *
 * @locality any
 * @locks none
 */
int rd_kafka_conf_warn(rd_kafka_t *rk) {
        int cnt = 0;

        cnt = rd_kafka_anyconf_warn_deprecated(rk, _RK_GLOBAL, &rk->rk_conf);
        if (rk->rk_conf.topic_conf)
                cnt += rd_kafka_anyconf_warn_deprecated(rk, _RK_TOPIC,
                                                        rk->rk_conf.topic_conf);

        if (rk->rk_conf.warn.default_topic_conf_overwritten)
                rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                             "Topic configuration properties set in the "
                             "global configuration were overwritten by "
                             "explicitly setting a default_topic_conf: "
                             "recommend not using set_default_topic_conf");

        /* Additional warnings */
        if (rk->rk_type == RD_KAFKA_CONSUMER) {
                if (rk->rk_conf.fetch_wait_max_ms + 1000 >
                    rk->rk_conf.socket_timeout_ms)
                        rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                                     "Configuration property "
                                     "`fetch.wait.max.ms` (%d) should be "
                                     "set lower than `socket.timeout.ms` (%d) "
                                     "by at least 1000ms to avoid blocking "
                                     "and timing out sub-sequent requests",
                                     rk->rk_conf.fetch_wait_max_ms,
                                     rk->rk_conf.socket_timeout_ms);
        }

        if (rd_kafka_conf_is_modified(&rk->rk_conf, "sasl.mechanisms") &&
            !(rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_SSL ||
              rk->rk_conf.security_protocol == RD_KAFKA_PROTO_SASL_PLAINTEXT)) {
                rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                             "Configuration property `sasl.mechanism` set to "
                             "`%s` but `security.protocol` is not configured "
                             "for SASL: recommend setting "
                             "`security.protocol` to SASL_SSL or "
                             "SASL_PLAINTEXT",
                             rk->rk_conf.sasl.mechanisms);
        }

        if (rd_kafka_conf_is_modified(&rk->rk_conf, "sasl.username") &&
            !(!strncmp(rk->rk_conf.sasl.mechanisms, "SCRAM", 5) ||
              !strcmp(rk->rk_conf.sasl.mechanisms, "PLAIN")))
                rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                             "Configuration property `sasl.username` only "
                             "applies when `sasl.mechanism` is set to "
                             "PLAIN or SCRAM-SHA-..");

        if (rd_kafka_conf_is_modified(&rk->rk_conf, "client.software.name") &&
            !rd_kafka_sw_str_is_safe(rk->rk_conf.sw_name))
                rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                             "Configuration property `client.software.name` "
                             "may only contain 'a-zA-Z0-9.-', other characters "
                             "will be replaced with '-'");

        if (rd_kafka_conf_is_modified(&rk->rk_conf,
                                      "client.software.version") &&
            !rd_kafka_sw_str_is_safe(rk->rk_conf.sw_version))
                rd_kafka_log(rk, LOG_WARNING, "CONFWARN",
                             "Configuration property `client.software.verison` "
                             "may only contain 'a-zA-Z0-9.-', other characters "
                             "will be replaced with '-'");

        if (rd_atomic32_get(&rk->rk_broker_cnt) == 0)
                rd_kafka_log(rk, LOG_NOTICE, "CONFWARN",
                             "No `bootstrap.servers` configured: "
                             "client will not be able to connect "
                             "to Kafka cluster");

        return cnt;
}


const rd_kafka_conf_t *rd_kafka_conf(rd_kafka_t *rk) {
        return &rk->rk_conf;
}


/**
 * @brief Unittests
 */
int unittest_conf(void) {
        rd_kafka_conf_t *conf;
        rd_kafka_topic_conf_t *tconf;
        rd_kafka_conf_res_t res, res2;
        char errstr[128];
        int iteration;
        const struct rd_kafka_property *prop;
        char readval[512];
        size_t readlen;
        const char *errstr2;

        conf  = rd_kafka_conf_new();
        tconf = rd_kafka_topic_conf_new();

        res = rd_kafka_conf_set(conf, "unknown.thing", "foo", errstr,
                                sizeof(errstr));
        RD_UT_ASSERT(res == RD_KAFKA_CONF_UNKNOWN, "fail");
        RD_UT_ASSERT(*errstr, "fail");

        for (iteration = 0; iteration < 5; iteration++) {
                int cnt;


                /* Iterations:
                 *  0 - Check is_modified
                 *  1 - Set every other config property, read back and verify.
                 *  2 - Check is_modified.
                 *  3 - Set all config properties, read back and verify.
                 *  4 - Check is_modified. */
                for (prop = rd_kafka_properties, cnt = 0; prop->name;
                     prop++, cnt++) {
                        const char *val;
                        char tmp[64];
                        int odd    = cnt & 1;
                        int do_set = iteration == 3 || (iteration == 1 && odd);
                        rd_bool_t is_modified;
                        int exp_is_modified =
                            !prop->unsupported &&
                            (iteration >= 3 ||
                             (iteration > 0 && (do_set || odd)));

                        readlen = sizeof(readval);

                        /* Avoid some special configs */
                        if (!strcmp(prop->name, "plugin.library.paths") ||
                            !strcmp(prop->name, "builtin.features"))
                                continue;

                        switch (prop->type) {
                        case _RK_C_STR:
                        case _RK_C_KSTR:
                        case _RK_C_PATLIST:
                                if (prop->sdef)
                                        val = prop->sdef;
                                else
                                        val = "test";
                                break;

                        case _RK_C_BOOL:
                                val = "true";
                                break;

                        case _RK_C_INT:
                                rd_snprintf(tmp, sizeof(tmp), "%d", prop->vdef);
                                val = tmp;
                                break;

                        case _RK_C_DBL:
                                rd_snprintf(tmp, sizeof(tmp), "%g", prop->ddef);
                                val = tmp;
                                break;

                        case _RK_C_S2F:
                        case _RK_C_S2I:
                                val = prop->s2i[0].str;
                                break;

                        case _RK_C_PTR:
                        case _RK_C_ALIAS:
                        case _RK_C_INVALID:
                        case _RK_C_INTERNAL:
                        default:
                                continue;
                        }


                        if (prop->scope & _RK_GLOBAL) {
                                if (do_set)
                                        res = rd_kafka_conf_set(
                                            conf, prop->name, val, errstr,
                                            sizeof(errstr));

                                res2 = rd_kafka_conf_get(conf, prop->name,
                                                         readval, &readlen);

                                is_modified =
                                    rd_kafka_conf_is_modified(conf, prop->name);


                        } else if (prop->scope & _RK_TOPIC) {
                                if (do_set)
                                        res = rd_kafka_topic_conf_set(
                                            tconf, prop->name, val, errstr,
                                            sizeof(errstr));

                                res2 = rd_kafka_topic_conf_get(
                                    tconf, prop->name, readval, &readlen);

                                is_modified = rd_kafka_topic_conf_is_modified(
                                    tconf, prop->name);

                        } else {
                                RD_NOTREACHED();
                        }



                        if (do_set && prop->unsupported) {
                                RD_UT_ASSERT(res == RD_KAFKA_CONF_INVALID,
                                             "conf_set %s should've failed "
                                             "with CONF_INVALID, not %d: %s",
                                             prop->name, res, errstr);

                        } else if (do_set) {
                                RD_UT_ASSERT(res == RD_KAFKA_CONF_OK,
                                             "conf_set %s failed: %d: %s",
                                             prop->name, res, errstr);
                                RD_UT_ASSERT(res2 == RD_KAFKA_CONF_OK,
                                             "conf_get %s failed: %d",
                                             prop->name, res2);

                                RD_UT_ASSERT(!strcmp(readval, val),
                                             "conf_get %s "
                                             "returned \"%s\": "
                                             "expected \"%s\"",
                                             prop->name, readval, val);

                                RD_UT_ASSERT(is_modified,
                                             "Property %s was set but "
                                             "is_modified=%d",
                                             prop->name, is_modified);
                        }

                        assert(is_modified == exp_is_modified);
                        RD_UT_ASSERT(is_modified == exp_is_modified,
                                     "Property %s is_modified=%d, "
                                     "exp_is_modified=%d "
                                     "(iter %d, odd %d, do_set %d)",
                                     prop->name, is_modified, exp_is_modified,
                                     iteration, odd, do_set);
                }
        }

        /* Set an alias and make sure is_modified() works for it. */
        res = rd_kafka_conf_set(conf, "max.in.flight", "19", NULL, 0);
        RD_UT_ASSERT(res == RD_KAFKA_CONF_OK, "%d", res);

        RD_UT_ASSERT(rd_kafka_conf_is_modified(conf, "max.in.flight") ==
                         rd_true,
                     "fail");
        RD_UT_ASSERT(rd_kafka_conf_is_modified(
                         conf, "max.in.flight.requests.per.connection") ==
                         rd_true,
                     "fail");

        rd_kafka_conf_destroy(conf);
        rd_kafka_topic_conf_destroy(tconf);


        /* Verify that software.client.* string-safing works */
        conf = rd_kafka_conf_new();
        res  = rd_kafka_conf_set(conf, "client.software.name",
                                " .~aba. va! !.~~", NULL, 0);
        RD_UT_ASSERT(res == RD_KAFKA_CONF_OK, "%d", res);
        res = rd_kafka_conf_set(conf, "client.software.version",
                                "!1.2.3.4.5!!! a", NULL, 0);
        RD_UT_ASSERT(res == RD_KAFKA_CONF_OK, "%d", res);

        errstr2 = rd_kafka_conf_finalize(RD_KAFKA_PRODUCER, conf);
        RD_UT_ASSERT(!errstr2, "conf_finalize() failed: %s", errstr2);

        readlen = sizeof(readval);
        res2 =
            rd_kafka_conf_get(conf, "client.software.name", readval, &readlen);
        RD_UT_ASSERT(res2 == RD_KAFKA_CONF_OK, "%d", res2);
        RD_UT_ASSERT(!strcmp(readval, "aba.-va"),
                     "client.software.* safification failed: \"%s\"", readval);
        RD_UT_SAY("Safified client.software.name=\"%s\"", readval);

        readlen = sizeof(readval);
        res2    = rd_kafka_conf_get(conf, "client.software.version", readval,
                                 &readlen);
        RD_UT_ASSERT(res2 == RD_KAFKA_CONF_OK, "%d", res2);
        RD_UT_ASSERT(!strcmp(readval, "1.2.3.4.5----a"),
                     "client.software.* safification failed: \"%s\"", readval);
        RD_UT_SAY("Safified client.software.version=\"%s\"", readval);

        rd_kafka_conf_destroy(conf);

        RD_UT_PASS();
}

/**@}*/
