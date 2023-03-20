/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2014-2018 Magnus Edenhill
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

#ifndef _RDKAFKA_CONF_H_
#define _RDKAFKA_CONF_H_

#include "rdlist.h"
#include "rdkafka_cert.h"

#if WITH_SSL && OPENSSL_VERSION_NUMBER >= 0x10100000 &&                        \
    !defined(OPENSSL_IS_BORINGSSL)
#define WITH_SSL_ENGINE 1
/* Deprecated in OpenSSL 3 */
#include <openssl/engine.h>
#endif /* WITH_SSL && OPENSSL_VERSION_NUMBER >= 0x10100000 */

/**
 * Forward declarations
 */
struct rd_kafka_transport_s;


/**
 * MessageSet compression codecs
 */
typedef enum {
        RD_KAFKA_COMPRESSION_NONE,
        RD_KAFKA_COMPRESSION_GZIP   = RD_KAFKA_MSG_ATTR_GZIP,
        RD_KAFKA_COMPRESSION_SNAPPY = RD_KAFKA_MSG_ATTR_SNAPPY,
        RD_KAFKA_COMPRESSION_KLZ4    = RD_KAFKA_MSG_ATTR_KLZ4,
        RD_KAFKA_COMPRESSION_ZSTD   = RD_KAFKA_MSG_ATTR_ZSTD,
        RD_KAFKA_COMPRESSION_INHERIT, /* Inherit setting from global conf */
        RD_KAFKA_COMPRESSION_NUM
} rd_kafka_compression_t;

static RD_INLINE RD_UNUSED const char *
rd_kafka_compression2str(rd_kafka_compression_t compr) {
        static const char *names[RD_KAFKA_COMPRESSION_NUM] = {
            [RD_KAFKA_COMPRESSION_NONE]    = "none",
            [RD_KAFKA_COMPRESSION_GZIP]    = "gzip",
            [RD_KAFKA_COMPRESSION_SNAPPY]  = "snappy",
            [RD_KAFKA_COMPRESSION_KLZ4]     = "lz4",
            [RD_KAFKA_COMPRESSION_ZSTD]    = "zstd",
            [RD_KAFKA_COMPRESSION_INHERIT] = "inherit"};
        static RD_TLS char ret[32];

        if ((int)compr < 0 || compr >= RD_KAFKA_COMPRESSION_NUM) {
                rd_snprintf(ret, sizeof(ret), "codec0x%x?", (int)compr);
                return ret;
        }

        return names[compr];
}

/**
 * MessageSet compression levels
 */
typedef enum {
        RD_KAFKA_COMPLEVEL_DEFAULT    = -1,
        RD_KAFKA_COMPLEVEL_MIN        = -1,
        RD_KAFKA_COMPLEVEL_GZIP_MAX   = 9,
        RD_KAFKA_COMPLEVEL_KLZ4_MAX    = 12,
        RD_KAFKA_COMPLEVEL_SNAPPY_MAX = 0,
        RD_KAFKA_COMPLEVEL_ZSTD_MAX   = 22,
        RD_KAFKA_COMPLEVEL_MAX        = 12
} rd_kafka_complevel_t;

typedef enum {
        RD_KAFKA_PROTO_PLAINTEXT,
        RD_KAFKA_PROTO_SSL,
        RD_KAFKA_PROTO_SASL_PLAINTEXT,
        RD_KAFKA_PROTO_SASL_SSL,
        RD_KAFKA_PROTO_NUM,
} rd_kafka_secproto_t;


typedef enum {
        RD_KAFKA_CONFIGURED,
        RD_KAFKA_LEARNED,
        RD_KAFKA_INTERNAL,
        RD_KAFKA_LOGICAL
} rd_kafka_confsource_t;

static RD_INLINE RD_UNUSED const char *
rd_kafka_confsource2str(rd_kafka_confsource_t source) {
        static const char *names[] = {"configured", "learned", "internal",
                                      "logical"};

        return names[source];
}


typedef enum {
        _RK_GLOBAL       = 0x1,
        _RK_PRODUCER     = 0x2,
        _RK_CONSUMER     = 0x4,
        _RK_TOPIC        = 0x8,
        _RK_CGRP         = 0x10,
        _RK_DEPRECATED   = 0x20,
        _RK_HIDDEN       = 0x40,
        _RK_HIGH         = 0x80,  /* High Importance */
        _RK_MED          = 0x100, /* Medium Importance */
        _RK_EXPERIMENTAL = 0x200, /* Experimental (unsupported) property */
        _RK_SENSITIVE    = 0x400  /* The configuration property's value
                                   * might contain sensitive information. */
} rd_kafka_conf_scope_t;

/**< While the client groups is a generic concept, it is currently
 *   only implemented for consumers in librdkafka. */
#define _RK_CGRP _RK_CONSUMER

typedef enum {
        _RK_CONF_PROP_SET_REPLACE, /* Replace current value (default) */
        _RK_CONF_PROP_SET_ADD,     /* Add value (S2F) */
        _RK_CONF_PROP_SET_DEL      /* Remove value (S2F) */
} rd_kafka_conf_set_mode_t;



typedef enum {
        RD_KAFKA_OFFSET_METHOD_NONE,
        RD_KAFKA_OFFSET_METHOD_FILE,
        RD_KAFKA_OFFSET_METHOD_BROKER
} rd_kafka_offset_method_t;

typedef enum {
        RD_KAFKA_SASL_OAUTHBEARER_METHOD_DEFAULT,
        RD_KAFKA_SASL_OAUTHBEARER_METHOD_OIDC
} rd_kafka_oauthbearer_method_t;

typedef enum {
        RD_KAFKA_SSL_ENDPOINT_ID_NONE,
        RD_KAFKA_SSL_ENDPOINT_ID_HTTPS, /**< RFC2818 */
} rd_kafka_ssl_endpoint_id_t;

/* Increase in steps of 64 as needed.
 * This must be larger than sizeof(rd_kafka_[topic_]conf_t) */
#define RD_KAFKA_CONF_PROPS_IDX_MAX (64 * 33)

/**
 * @struct rd_kafka_anyconf_t
 * @brief The anyconf header must be the first field in the
 *        rd_kafka_conf_t and rd_kafka_topic_conf_t structs.
 *        It provides a way to track which property has been modified.
 */
struct rd_kafka_anyconf_hdr {
        uint64_t modified[RD_KAFKA_CONF_PROPS_IDX_MAX / 64];
};


/**
 * Optional configuration struct passed to rd_kafka_new*().
 *
 * The struct is populated ted through string properties
 * by calling rd_kafka_conf_set().
 *
 */
struct rd_kafka_conf_s {
        struct rd_kafka_anyconf_hdr hdr; /**< Must be first field */

        /*
         * Generic configuration
         */
        int enabled_events;
        int max_msg_size;
        int msg_copy_max_size;
        int recv_max_msg_size;
        int max_inflight;
        int metadata_request_timeout_ms;
        int metadata_refresh_interval_ms;
        int metadata_refresh_fast_cnt;
        int metadata_refresh_fast_interval_ms;
        int metadata_refresh_sparse;
        int metadata_max_age_ms;
        int metadata_propagation_max_ms;
        int debug;
        int broker_addr_ttl;
        int broker_addr_family;
        int socket_timeout_ms;
        int socket_blocking_max_ms;
        int socket_sndbuf_size;
        int socket_rcvbuf_size;
        int socket_keepalive;
        int socket_nagle_disable;
        int socket_max_fails;
        char *client_id_str;
        char *brokerlist;
        int stats_interval_ms;
        int term_sig;
        int reconnect_backoff_ms;
        int reconnect_backoff_max_ms;
        int reconnect_jitter_ms;
        int socket_connection_setup_timeout_ms;
        int connections_max_idle_ms;
        int sparse_connections;
        int sparse_connect_intvl;
        int api_version_request;
        int api_version_request_timeout_ms;
        int api_version_fallback_ms;
        char *broker_version_fallback;
        rd_kafka_secproto_t security_protocol;

        struct {
#if WITH_SSL
                SSL_CTX *ctx;
#endif
                char *cipher_suites;
                char *curves_list;
                char *sigalgs_list;
                char *key_location;
                char *key_pem;
                rd_kafka_cert_t *key;
                char *key_password;
                char *cert_location;
                char *cert_pem;
                rd_kafka_cert_t *cert;
                char *ca_location;
                char *ca_pem;
                rd_kafka_cert_t *ca;
                /** CSV list of Windows certificate stores */
                char *ca_cert_stores;
                char *crl_location;
#if WITH_SSL && OPENSSL_VERSION_NUMBER >= 0x10100000
                ENGINE *engine;
#endif
                char *engine_location;
                char *engine_id;
                void *engine_callback_data;
                char *providers;
                rd_list_t loaded_providers; /**< (SSL_PROVIDER*) */
                char *keystore_location;
                char *keystore_password;
                int endpoint_identification;
                int enable_verify;
                int (*cert_verify_cb)(rd_kafka_t *rk,
                                      const char *broker_name,
                                      int32_t broker_id,
                                      int *x509_error,
                                      int depth,
                                      const char *buf,
                                      size_t size,
                                      char *errstr,
                                      size_t errstr_size,
                                      void *opaque);
        } ssl;

        struct {
                const struct rd_kafka_sasl_provider *provider;
                char *principal;
                char *mechanisms;
                char *service_name;
                char *kinit_cmd;
                char *keytab;
                int relogin_min_time;
                /** Protects .username and .password access after client
                 *  instance has been created (see sasl_set_credentials()). */
                mtx_t lock;
                char *username;
                char *password;
#if WITH_SASL_SCRAM
                /* SCRAM EVP-wrapped hash function
                 * (return value from EVP_shaX()) */
                const void /*EVP_MD*/ *scram_evp;
                /* SCRAM direct hash function (e.g., SHA256()) */
                unsigned char *(*scram_H)(const unsigned char *d,
                                          size_t n,
                                          unsigned char *md);
                /* Hash size */
                size_t scram_H_size;
#endif
                char *oauthbearer_config;
                int enable_oauthbearer_unsecure_jwt;
                int enable_callback_queue;
                struct {
                        rd_kafka_oauthbearer_method_t method;
                        char *token_endpoint_url;
                        char *client_id;
                        char *client_secret;
                        char *scope;
                        char *extensions_str;
                        /* SASL/OAUTHBEARER token refresh event callback */
                        void (*token_refresh_cb)(rd_kafka_t *rk,
                                                 const char *oauthbearer_config,
                                                 void *opaque);
                } oauthbearer;
        } sasl;

        char *plugin_paths;
#if WITH_PLUGINS
        rd_list_t plugins;
#endif

        /* Interceptors */
        struct {
                /* rd_kafka_interceptor_method_t lists */
                rd_list_t on_conf_set;            /* on_conf_set interceptors
                                                   * (not copied on conf_dup()) */
                rd_list_t on_conf_dup;            /* .. (not copied) */
                rd_list_t on_conf_destroy;        /* .. (not copied) */
                rd_list_t on_new;                 /* .. (copied) */
                rd_list_t on_destroy;             /* .. (copied) */
                rd_list_t on_send;                /* .. (copied) */
                rd_list_t on_acknowledgement;     /* .. (copied) */
                rd_list_t on_consume;             /* .. (copied) */
                rd_list_t on_commit;              /* .. (copied) */
                rd_list_t on_request_sent;        /* .. (copied) */
                rd_list_t on_response_received;   /* .. (copied) */
                rd_list_t on_thread_start;        /* .. (copied) */
                rd_list_t on_thread_exit;         /* .. (copied) */
                rd_list_t on_broker_state_change; /* .. (copied) */

                /* rd_strtup_t list */
                rd_list_t config; /* Configuration name=val's
                                   * handled by interceptors. */
        } interceptors;

        /* Client group configuration */
        int coord_query_intvl_ms;
        int max_poll_interval_ms;

        int builtin_features;
        /*
         * Consumer configuration
         */
        int check_crcs;
        int queued_min_msgs;
        int queued_max_msg_kbytes;
        int64_t queued_max_msg_bytes;
        int fetch_wait_max_ms;
        int fetch_msg_max_bytes;
        int fetch_max_bytes;
        int fetch_min_bytes;
        int fetch_error_backoff_ms;
        char *group_id_str;
        char *group_instance_id;
        int allow_auto_create_topics;

        rd_kafka_pattern_list_t *topic_blacklist;
        struct rd_kafka_topic_conf_s *topic_conf; /* Default topic config
                                                   * for automatically
                                                   * subscribed topics. */
        int enable_auto_commit;
        int enable_auto_offset_store;
        int auto_commit_interval_ms;
        int group_session_timeout_ms;
        int group_heartbeat_intvl_ms;
        rd_kafkap_str_t *group_protocol_type;
        char *partition_assignment_strategy;
        rd_list_t partition_assignors;
        int enabled_assignor_cnt;

        void (*rebalance_cb)(rd_kafka_t *rk,
                             rd_kafka_resp_err_t err,
                             rd_kafka_topic_partition_list_t *partitions,
                             void *opaque);

        void (*offset_commit_cb)(rd_kafka_t *rk,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 void *opaque);

        rd_kafka_offset_method_t offset_store_method;

        rd_kafka_isolation_level_t isolation_level;

        int enable_partition_eof;

        rd_kafkap_str_t *client_rack;

        /*
         * Producer configuration
         */
        struct {
                /*
                 * Idempotence
                 */
                int idempotence;   /**< Enable Idempotent Producer */
                rd_bool_t gapless; /**< Raise fatal error if
                                    *   gapless guarantee can't be
                                    *   satisfied. */
                /*
                 * Transactions
                 */
                char *transactional_id;     /**< Transactional Id */
                int transaction_timeout_ms; /**< Transaction timeout */
        } eos;
        int queue_buffering_max_msgs;
        int queue_buffering_max_kbytes;
        double buffering_max_ms_dbl; /**< This is the configured value */
        rd_ts_t buffering_max_us;    /**< This is the value used in the code */
        int queue_backpressure_thres;
        int max_retries;
        int retry_backoff_ms;
        int batch_num_messages;
        int batch_size;
        rd_kafka_compression_t compression_codec;
        int dr_err_only;
        int sticky_partition_linger_ms;

        /* Message delivery report callback.
         * Called once for each produced message, either on
         * successful and acknowledged delivery to the broker in which
         * case 'err' is 0, or if the message could not be delivered
         * in which case 'err' is non-zero (use rd_kafka_err2str()
         * to obtain a human-readable error reason).
         *
         * If the message was produced with neither RD_KAFKA_MSG_F_FREE
         * or RD_KAFKA_MSG_F_COPY set then 'payload' is the original
         * pointer provided to rd_kafka_produce().
         * rdkafka will not perform any further actions on 'payload'
         * at this point and the application may rd_free the payload data
         * at this point.
         *
         * 'opaque' is 'conf.opaque', while 'msg_opaque' is
         * the opaque pointer provided in the rd_kafka_produce() call.
         */
        void (*dr_cb)(rd_kafka_t *rk,
                      void *payload,
                      size_t len,
                      rd_kafka_resp_err_t err,
                      void *opaque,
                      void *msg_opaque);

        void (*dr_msg_cb)(rd_kafka_t *rk,
                          const rd_kafka_message_t *rkmessage,
                          void *opaque);

        /* Consume callback */
        void (*consume_cb)(rd_kafka_message_t *rkmessage, void *opaque);

        /* Log callback */
        void (*log_cb)(const rd_kafka_t *rk,
                       int level,
                       const char *fac,
                       const char *buf);
        int log_level;
        int log_queue;
        int log_thread_name;
        int log_connection_close;

        /* PRNG seeding */
        int enable_random_seed;

        /* Error callback */
        void (*error_cb)(rd_kafka_t *rk,
                         int err,
                         const char *reason,
                         void *opaque);

        /* Throttle callback */
        void (*throttle_cb)(rd_kafka_t *rk,
                            const char *broker_name,
                            int32_t broker_id,
                            int throttle_time_ms,
                            void *opaque);

        /* Stats callback */
        int (*stats_cb)(rd_kafka_t *rk,
                        char *json,
                        size_t json_len,
                        void *opaque);

        /* Socket creation callback */
        int (*socket_cb)(int domain, int type, int protocol, void *opaque);

        /* Connect callback */
        int (*connect_cb)(int sockfd,
                          const struct sockaddr *addr,
                          int addrlen,
                          const char *id,
                          void *opaque);

        /* Close socket callback */
        int (*closesocket_cb)(int sockfd, void *opaque);

        /* File open callback */
        int (*open_cb)(const char *pathname,
                       int flags,
                       mode_t mode,
                       void *opaque);

        /* Address resolution callback */
        int (*resolve_cb)(const char *node,
                          const char *service,
                          const struct addrinfo *hints,
                          struct addrinfo **res,
                          void *opaque);

        /* Background queue event callback */
        void (*background_event_cb)(rd_kafka_t *rk,
                                    rd_kafka_event_t *rkev,
                                    void *opaque);


        /* Opaque passed to callbacks. */
        void *opaque;

        /* For use with value-less properties. */
        int dummy;


        /* Admin client defaults */
        struct {
                int request_timeout_ms; /* AdminOptions.request_timeout */
        } admin;


        /*
         * Test mocks
         */
        struct {
                int broker_cnt; /**< Number of mock brokers */
                int broker_rtt; /**< Broker RTT */
        } mock;

        /*
         * Unit test pluggable interfaces
         */
        struct {
                /**< Inject errors in ProduceResponse handler */
                rd_kafka_resp_err_t (*handle_ProduceResponse)(
                    rd_kafka_t *rk,
                    int32_t brokerid,
                    uint64_t msgid,
                    rd_kafka_resp_err_t err);
        } ut;

        char *sw_name;    /**< Software/client name */
        char *sw_version; /**< Software/client version */

        struct {
                /** Properties on (implicit pass-thru) default_topic_conf were
                 *  overwritten by passing an explicit default_topic_conf. */
                rd_bool_t default_topic_conf_overwritten;
        } warn;
};

int rd_kafka_socket_cb_linux(int domain, int type, int protocol, void *opaque);
int rd_kafka_socket_cb_generic(int domain,
                               int type,
                               int protocol,
                               void *opaque);
#ifndef _WIN32
int rd_kafka_open_cb_linux(const char *pathname,
                           int flags,
                           mode_t mode,
                           void *opaque);
#endif
int rd_kafka_open_cb_generic(const char *pathname,
                             int flags,
                             mode_t mode,
                             void *opaque);



struct rd_kafka_topic_conf_s {
        struct rd_kafka_anyconf_hdr hdr; /**< Must be first field */

        int required_acks;
        int32_t request_timeout_ms;
        int message_timeout_ms;

        int32_t (*partitioner)(const rd_kafka_topic_t *rkt,
                               const void *keydata,
                               size_t keylen,
                               int32_t partition_cnt,
                               void *rkt_opaque,
                               void *msg_opaque);
        char *partitioner_str;

        rd_bool_t random_partitioner; /**< rd_true - random
                                       *  rd_false - sticky */

        int queuing_strategy; /* RD_KAFKA_QUEUE_FIFO|LIFO */
        int (*msg_order_cmp)(const void *a, const void *b);

        rd_kafka_compression_t compression_codec;
        rd_kafka_complevel_t compression_level;
        int produce_offset_report;

        int consume_callback_max_msgs;
        int auto_commit;
        int auto_commit_interval_ms;
        int auto_offset_reset;
        char *offset_store_path;
        int offset_store_sync_interval_ms;

        rd_kafka_offset_method_t offset_store_method;

        /* Application provided opaque pointer (this is rkt_opaque) */
        void *opaque;
};


char **rd_kafka_conf_kv_split(const char **input, size_t incnt, size_t *cntp);

void rd_kafka_anyconf_destroy(int scope, void *conf);

rd_bool_t rd_kafka_conf_is_modified(const rd_kafka_conf_t *conf,
                                    const char *name);

void rd_kafka_desensitize_str(char *str);

void rd_kafka_conf_desensitize(rd_kafka_conf_t *conf);
void rd_kafka_topic_conf_desensitize(rd_kafka_topic_conf_t *tconf);

const char *rd_kafka_conf_finalize(rd_kafka_type_t cltype,
                                   rd_kafka_conf_t *conf);
const char *rd_kafka_topic_conf_finalize(rd_kafka_type_t cltype,
                                         const rd_kafka_conf_t *conf,
                                         rd_kafka_topic_conf_t *tconf);


int rd_kafka_conf_warn(rd_kafka_t *rk);

void rd_kafka_anyconf_dump_dbg(rd_kafka_t *rk,
                               int scope,
                               const void *conf,
                               const char *description);

#include "rdkafka_confval.h"

int unittest_conf(void);

#endif /* _RDKAFKA_CONF_H_ */
