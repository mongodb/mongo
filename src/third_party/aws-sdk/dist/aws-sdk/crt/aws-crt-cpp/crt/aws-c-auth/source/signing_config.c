/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signing_config.h>

#include <aws/auth/credentials.h>

const struct aws_byte_cursor g_aws_signed_body_value_empty_sha256 =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

const struct aws_byte_cursor g_aws_signed_body_value_unsigned_payload =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("UNSIGNED-PAYLOAD");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_unsigned_payload_trailer =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-UNSIGNED-PAYLOAD-TRAILER");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_payload =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-AWS4-HMAC-SHA256-PAYLOAD");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_payload_trailer =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-AWS4-HMAC-SHA256-PAYLOAD-TRAILER");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_ecdsa_p256_sha256_payload =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-AWS4-ECDSA-P256-SHA256-PAYLOAD");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_ecdsa_p256_sha256_payload_trailer =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-AWS4-ECDSA-P256-SHA256-PAYLOAD-TRAILER");

const struct aws_byte_cursor g_aws_signed_body_value_streaming_aws4_hmac_sha256_events =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("STREAMING-AWS4-HMAC-SHA256-EVENTS");

const char *aws_signing_algorithm_to_string(enum aws_signing_algorithm algorithm) {
    switch (algorithm) {
        case AWS_SIGNING_ALGORITHM_V4:
            return "SigV4";

        case AWS_SIGNING_ALGORITHM_V4_S3EXPRESS:
            return "SigV4S3Express";

        case AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC:
            return "SigV4Asymmetric";

        default:
            break;
    }

    return "Unknown";
}

int aws_validate_aws_signing_config_aws(const struct aws_signing_config_aws *config) {
    if (config == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "AWS signing config is null");
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    if (config->signature_type == AWS_ST_HTTP_REQUEST_EVENT && config->algorithm != AWS_SIGNING_ALGORITHM_V4) {
        /*
         * Not supported yet.
         *
         * Need to determine if the Transcribe service supports Sigv4a and how to test it.
         * Transcribe's examples are insufficient.
         */
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "(id=%p) Event signing is only supported for Sigv4 yet", (void *)config);
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    if (config->signature_type != AWS_ST_HTTP_REQUEST_HEADERS &&
        config->signature_type != AWS_ST_HTTP_REQUEST_QUERY_PARAMS) {
        /*
         * If we're not signing the full request then it's critical that the credentials we're using are the same
         * credentials used on the original request.  If we're using a provider to fetch credentials then that is
         * not guaranteed.  For now, force users to always pass in credentials when signing events or chunks.
         *
         * The correct long-term solution would be to add a way to pass the credentials used in the initial
         * signing back to the user in the completion callback.  Then the user could supply those credentials
         * to all subsequent chunk/event signings.  The fact that we don't do that yet doesn't invalidate this check.
         */
        if (config->credentials == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_SIGNING,
                "(id=%p) Chunk/event signing config must contain explicit credentials",
                (void *)config);
            return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
        }
    }

    if (config->region.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "(id=%p) Signing config is missing a region identifier", (void *)config);
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    if (config->service.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "(id=%p) Signing config is missing a service identifier", (void *)config);
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    switch (config->algorithm) {
        case AWS_SIGNING_ALGORITHM_V4:
            if (config->credentials == NULL && config->credentials_provider == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_SIGNING,
                    "(id=%p) Sigv4 signing config is missing a credentials provider or credentials",
                    (void *)config);
                return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
            }

            if (config->credentials != NULL && !aws_credentials_is_anonymous(config->credentials)) {
                if (aws_credentials_get_access_key_id(config->credentials).len == 0 ||
                    aws_credentials_get_secret_access_key(config->credentials).len == 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_AUTH_SIGNING,
                        "(id=%p) Sigv4 signing configured with invalid credentials",
                        (void *)config);
                    return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CREDENTIALS);
                }
            }
            break;
        case AWS_SIGNING_ALGORITHM_V4_S3EXPRESS:
            if (config->credentials == NULL && config->credentials_provider == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_SIGNING,
                    "(id=%p) Sigv4 S3 Express signing config is missing a credentials provider or credentials",
                    (void *)config);
                return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
            }

            if (config->credentials != NULL) {
                if (aws_credentials_is_anonymous(config->credentials) ||
                    aws_credentials_get_access_key_id(config->credentials).len == 0 ||
                    aws_credentials_get_secret_access_key(config->credentials).len == 0 ||
                    aws_credentials_get_session_token(config->credentials).len == 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_AUTH_SIGNING,
                        "(id=%p) Sigv4 S3 Express signing configured with invalid credentials",
                        (void *)config);
                    return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CREDENTIALS);
                }
            }
            break;

        case AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC:
            if (config->credentials == NULL && config->credentials_provider == NULL) {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_SIGNING,
                    "(id=%p) Sigv4 asymmetric signing config is missing a credentials provider or credentials",
                    (void *)config);
                return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
            }
            break;

        default:
            return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    return AWS_OP_SUCCESS;
}
