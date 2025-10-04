#ifndef AWS_AUTH_SIGNING_CONFIG_H
#define AWS_AUTH_SIGNING_CONFIG_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/auth.h>

#include <aws/common/byte_buf.h>
#include <aws/common/date_time.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_credentials;

typedef bool(aws_should_sign_header_fn)(const struct aws_byte_cursor *name, void *userdata);

/**
 * A primitive RTTI indicator for signing configuration structs
 *
 * There must be one entry per config structure type and it's a fatal error
 * to put the wrong value in the "config_type" member of your config structure.
 */
enum aws_signing_config_type { AWS_SIGNING_CONFIG_AWS = 1 };

/**
 * All signing configuration structs must match this by having
 * the config_type member as the first member.
 */
struct aws_signing_config_base {
    enum aws_signing_config_type config_type;
};

/**
 * What version of the AWS signing process should we use.
 */
enum aws_signing_algorithm {
    AWS_SIGNING_ALGORITHM_V4,
    AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC,
    AWS_SIGNING_ALGORITHM_V4_S3EXPRESS,
};

/**
 * What sort of signature should be computed from the signable?
 */
enum aws_signature_type {
    /**
     * A signature for a full http request should be computed, with header updates applied to the signing result.
     */
    AWS_ST_HTTP_REQUEST_HEADERS,

    /**
     * A signature for a full http request should be computed, with query param updates applied to the signing result.
     */
    AWS_ST_HTTP_REQUEST_QUERY_PARAMS,

    /**
     * Compute a signature for a payload chunk. The signable's input stream should be the chunk data and the
     * signable should contain the most recent signature value (either the original http request or the most recent
     * chunk) in the "previous-signature" property.
     */
    AWS_ST_HTTP_REQUEST_CHUNK,

    /**
     * Compute a signature for an event stream event. The signable's input stream should be the encoded event-stream
     * message (headers + payload), the signable should contain the most recent signature value (either the original
     * http request or the most recent event) in the "previous-signature" property.
     *
     * This option is only supported for Sigv4 for now.
     */
    AWS_ST_HTTP_REQUEST_EVENT,

    /**
     * Compute a signature for an http request via it's already-computed canonical request.  Only the authorization
     * signature header is added to the signing result.
     */
    AWS_ST_CANONICAL_REQUEST_HEADERS,

    /**
     * Compute a signature for an http request via it's already-computed canonical request.  Only the authorization
     * signature query param is added to the signing result.
     */
    AWS_ST_CANONICAL_REQUEST_QUERY_PARAMS,

    /**
     * Compute a signature for the trailing headers.
     * the signable should contain the most recent signature value (either the original http request or the most recent
     * chunk) in the "previous-signature" property.
     */
    AWS_ST_HTTP_REQUEST_TRAILING_HEADERS
};

/**
 * The SHA-256 of an empty string:
 * 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_empty_sha256;

/**
 * 'UNSIGNED-PAYLOAD'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_unsigned_payload;

/**
 * 'STREAMING-UNSIGNED-PAYLOAD-TRAILER'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_streaming_unsigned_payload_trailer;

/**
 * 'STREAMING-AWS4-HMAC-SHA256-PAYLOAD'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_payload;

/**
 * 'STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_payload_trailer;

/**
 * 'STREAMING-AWS4-ECDSA-P256-SHA256-PAYLOAD'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_ecdsa_p256_sha256_payload;

/**
 * 'STREAMING-AWS4-ECDSA-P256-SHA256-PAYLOAD-TRAILER'
 * For use with `aws_signing_config_aws.signed_body_value`.
 */
AWS_AUTH_API extern const struct aws_byte_cursor
    g_aws_signed_body_value_streaming_aws4_ecdsa_p256_sha256_payload_trailer;

/**
 * 'STREAMING-AWS4-HMAC-SHA256-EVENTS'
 * For use with `aws_signing_config_aws.signed_body_value`.
 *
 * Event signing is only supported for Sigv4 for now.
 */
AWS_AUTH_API extern const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_events;

/**
 * Controls if signing adds a header containing the canonical request's body value
 */
enum aws_signed_body_header_type {
    /**
     * Do not add a header
     */
    AWS_SBHT_NONE,

    /**
     * Add the "x-amz-content-sha256" header with the canonical request's body value
     */
    AWS_SBHT_X_AMZ_CONTENT_SHA256,
};

/**
 * A configuration structure for use in AWS-related signing.  Currently covers sigv4 only, but is not required to.
 */
struct aws_signing_config_aws {

    /**
     * What kind of config structure is this?
     */
    enum aws_signing_config_type config_type;

    /**
     * What signing algorithm to use.
     */
    enum aws_signing_algorithm algorithm;

    /**
     * What sort of signature should be computed?
     */
    enum aws_signature_type signature_type;

    /*
     * Region-related configuration
     *   (1) If Sigv4, the region to sign against
     *   (2) If Sigv4a, the value of the X-amzn-region-set header (added in signing)
     */
    struct aws_byte_cursor region;

    /**
     * name of service to sign a request for
     */
    struct aws_byte_cursor service;

    /**
     * Raw date to use during the signing process.
     */
    struct aws_date_time date;

    /**
     * Optional function to control which headers are a part of the canonical request.
     * Skipping auth-required headers will result in an unusable signature.  Headers injected by the signing process
     * are not skippable.
     *
     * This function does not override the internal check function (x-amzn-trace-id, user-agent), but rather
     * supplements it.  In particular, a header will get signed if and only if it returns true to both
     * the internal check (skips x-amzn-trace-id, user-agent) and this function (if defined).
     */
    aws_should_sign_header_fn *should_sign_header;
    void *should_sign_header_ud;

    /*
     * Put all flags in here at the end.  If this grows, stay aware of bit-space overflow and ABI compatibilty.
     */
    struct {
        /**
         * We assume the uri will be encoded once in preparation for transmission.  Certain services
         * do not decode before checking signature, requiring us to actually double-encode the uri in the canonical
         * request in order to pass a signature check.
         */
        uint32_t use_double_uri_encode : 1;

        /**
         * Controls whether or not the uri paths should be normalized when building the canonical request
         */
        uint32_t should_normalize_uri_path : 1;

        /**
         * Controls whether "X-Amz-Security-Token" is omitted from the canonical request.
         * "X-Amz-Security-Token" is added during signing, as a header or
         * query param, when credentials have a session token.
         * If false (the default), this parameter is included in the canonical request.
         * If true, this parameter is still added, but omitted from the canonical request.
         */
        uint32_t omit_session_token : 1;
    } flags;

    /**
     * Optional string to use as the canonical request's body value.
     * If string is empty, a value will be calculated from the payload during signing.
     * Typically, this is the SHA-256 of the (request/chunk/event) payload, written as lowercase hex.
     * If this has been precalculated, it can be set here. Special values used by certain services can also be set
     * (e.g. "UNSIGNED-PAYLOAD" "STREAMING-AWS4-HMAC-SHA256-PAYLOAD" "STREAMING-AWS4-HMAC-SHA256-EVENTS").
     */
    struct aws_byte_cursor signed_body_value;

    /**
     * Controls what body "hash" header, if any, should be added to the canonical request and the signed request:
     *   AWS_SBHT_NONE - no header should be added
     *   AWS_SBHT_X_AMZ_CONTENT_SHA256 - the body "hash" should be added in the X-Amz-Content-Sha256 header
     */
    enum aws_signed_body_header_type signed_body_header;

    /*
     * Signing key control:
     *
     *   If "credentials" is valid:
     *      use it
     *   Else if "credentials_provider" is valid
     *      query credentials from the provider
     *      If sigv4a is being used
     *          use the ecc-based credentials derived from the query result
     *      Else
     *          use the query result
     *   Else
     *      fail
     *
     */

    /*
     * AWS Credentials to sign with.  If Sigv4a is the algorithm and the credentials supplied are not ecc-based,
     * a temporary ecc-based credentials object will be built and used instead.
     */
    const struct aws_credentials *credentials;

    /*
     * AWS credentials provider to fetch credentials from.  If the signing algorithm is asymmetric sigv4, then the
     * ecc-based credentials will be derived from the fetched credentials.
     */
    struct aws_credentials_provider *credentials_provider;

    /**
     * If non-zero and the signing transform is query param, then signing will add X-Amz-Expires to the query
     * string, equal to the value specified here.  If this value is zero or if header signing is being used then
     * this parameter has no effect.
     */
    uint64_t expiration_in_seconds;
};

AWS_EXTERN_C_BEGIN

/**
 * Returns a c-string that describes the supplied signing algorithm
 *
 * @param algorithm signing algorithm to get a friendly string name for
 *
 * @return friendly string name of the supplied algorithm, or "Unknown" if the algorithm is not recognized
 */
AWS_AUTH_API
const char *aws_signing_algorithm_to_string(enum aws_signing_algorithm algorithm);

/**
 * Checks a signing configuration for invalid settings combinations.
 *
 * @param config signing configuration to validate
 *
 * @return - AWS_OP_SUCCESS if the configuration is valid, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_validate_aws_signing_config_aws(const struct aws_signing_config_aws *config);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_AUTH_SIGNING_CONFIG_H */
