#ifndef AWS_S3EXPRESS_CREDENTIALS_PROVIDER_IMPL_H
#define AWS_S3EXPRESS_CREDENTIALS_PROVIDER_IMPL_H

#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/s3/s3_client.h>
#include <aws/s3/s3express_credentials_provider.h>

struct aws_cache;

/**
 * Everything in the session should ONLY be accessed with lock HELD
 */
struct aws_s3express_session {
    struct aws_allocator *allocator;
    /* The hash key for the table storing creator and session. */
    struct aws_string *hash_key;

    /* The s3express credentials cached for the session */
    struct aws_credentials *s3express_credentials;

    /* Pointer to the creator if the session is in process creating */
    struct aws_s3express_session_creator *creator;

    /* The region and host of the session */
    struct aws_string *region;
    struct aws_string *host;
    bool inactive;

    /* Only used for mock tests */
    struct aws_s3express_credentials_provider_impl *impl;
};

struct aws_s3express_credentials_provider_impl {
    struct aws_s3_client *client;

    /* Internal Refcount to make sure the provider out lives all the context. */
    struct aws_ref_count internal_ref;

    struct aws_task *bg_refresh_task;
    struct aws_event_loop *bg_event_loop;

    const struct aws_credentials *default_original_credentials;
    struct aws_credentials_provider *default_original_credentials_provider;

    struct {
        /* Protected by the impl lock */
        struct aws_mutex lock;
        /**
         * Store the session creators in process.
         * `struct aws_string *` as Key. `struct aws_s3express_session_creator *` as Value
         */
        struct aws_hash_table session_creator_table;
        /**
         * An LRU cache to store all the sessions.
         * `struct aws_string *` as Key. `struct aws_s3express_session *` as Value
         **/
        struct aws_cache *cache;
        bool destroying;
    } synced_data;

    struct {
        /* Overrides for testing purpose. */

        struct aws_uri *endpoint_override;
        uint64_t bg_refresh_secs_override;

        bool (*s3express_session_is_valid_override)(struct aws_s3express_session *session, uint64_t now_seconds);
        bool (*s3express_session_about_to_expire_override)(struct aws_s3express_session *session, uint64_t now_seconds);

        /* The callback to be invoked before the real meta request finished callback for provider */
        aws_s3_meta_request_finish_fn *meta_request_finished_overhead;
    } mock_test;
};

/**
 * Configuration options for the default S3 Express credentials provider
 */
struct aws_s3express_credentials_provider_default_options {
    /**
     * The S3 client to fetch credentials.
     * Note, the client is not owned by the provider, user should keep the s3 client outlive the provider. */
    struct aws_s3_client *client;

    /* Optional callback for shutdown complete of the provider */
    aws_simple_completion_callback *shutdown_complete_callback;
    void *shutdown_user_data;

    struct {
        uint64_t bg_refresh_secs_override;
    } mock_test;
};

AWS_EXTERN_C_BEGIN
/**
 * Create the default S3 Express credentials provider.
 *
 * @param allocator
 * @return
 */
AWS_S3_API
struct aws_s3express_credentials_provider *aws_s3express_credentials_provider_new_default(
    struct aws_allocator *allocator,
    const struct aws_s3express_credentials_provider_default_options *options);

/**
 * Encode the hash key to be [host_value][hash_of_credentials]
 * hash_of_credentials is the sha256 of [access_key][secret_access_key]
 */
AWS_S3_API
struct aws_string *aws_encode_s3express_hash_key_new(
    struct aws_allocator *allocator,
    const struct aws_credentials *original_credentials,
    struct aws_byte_cursor host_value);

AWS_EXTERN_C_END
#endif /* AWS_S3EXPRESS_CREDENTIALS_PROVIDER_IMPL_H */
