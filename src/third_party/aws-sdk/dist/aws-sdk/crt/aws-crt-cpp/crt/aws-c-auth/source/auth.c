/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/auth/auth.h>

#include <aws/auth/private/aws_signing.h>

#include <aws/cal/cal.h>

#include <aws/http/http.h>

#include <aws/sdkutils/sdkutils.h>

#include <aws/common/error.h>
#include <aws/common/json.h>

#define AWS_DEFINE_ERROR_INFO_AUTH(CODE, STR) AWS_DEFINE_ERROR_INFO(CODE, STR, "aws-c-auth")

/* clang-format off */
static struct aws_error_info s_errors[] = {
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_UNSUPPORTED_ALGORITHM,
        "Attempt to sign an http request with an unsupported version of the AWS signing protocol"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_MISMATCHED_CONFIGURATION,
        "Attempt to sign an http request with a signing configuration unrecognized by the invoked signer"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_NO_CREDENTIALS,
        "Attempt to sign an http request without credentials"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_ILLEGAL_REQUEST_QUERY_PARAM,
        "Attempt to sign an http request that includes a query param that signing may add"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_ILLEGAL_REQUEST_HEADER,
        "Attempt to sign an http request that includes a header that signing may add"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_INVALID_CONFIGURATION,
        "Attempt to sign an http request with an invalid signing configuration"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_INVALID_ENVIRONMENT,
        "Required environment variables could not be sourced from process environment"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_INVALID_DELEGATE,
        "Valid credentials could not be sourced from the provided vtable"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_PROFILE_SOURCE_FAILURE,
        "Valid credentials could not be sourced by a profile provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_IMDS_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the IMDS provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_STS_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the STS provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE,
        "Unsuccessful status code returned from credentials-fetching http request"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_PROVIDER_PARSER_UNEXPECTED_RESPONSE,
        "Invalid response document encountered while querying credentials via http"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_ECS_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the ECS provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_X509_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the X509 provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_PROCESS_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the process provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_STS_WEB_IDENTITY_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the sts web identity provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_UNSUPPORTED_SIGNATURE_TYPE,
        "Attempt to sign using an unusupported signature type"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_MISSING_PREVIOUS_SIGNATURE,
        "Attempt to sign a streaming item without supplying a previous signature"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGNING_INVALID_CREDENTIALS,
        "Attempt to perform a signing operation with invalid credentials"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CANONICAL_REQUEST_MISMATCH,
        "Expected canonical request did not match the computed canonical request"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SIGV4A_SIGNATURE_VALIDATION_FAILURE,
        "The supplied sigv4a signature was not a valid signature for the hashed string to sign"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_COGNITO_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the cognito provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_DELEGATE_FAILURE,
        "Valid credentials could not be sourced by the delegate provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SSO_TOKEN_PROVIDER_SOURCE_FAILURE,
        "Valid token could not be sourced by the sso token provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SSO_TOKEN_INVALID,
        "Token sourced by the sso token provider is invalid."),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_SSO_TOKEN_EXPIRED,
        "Token sourced by the sso token provider is expired."),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE,
        "Valid credentials could not be sourced by the sso credentials provider"),
    AWS_DEFINE_ERROR_INFO_AUTH(
        AWS_AUTH_IMDS_CLIENT_SOURCE_FAILURE,
        "Failed to source the IMDS resource"),
            AWS_DEFINE_ERROR_INFO_AUTH(
    AWS_AUTH_PROFILE_STS_CREDENTIALS_PROVIDER_CYCLE_FAILURE,
        "Failed to resolve credentials because the profile contains a cycle in the assumeRole chain."),
        AWS_DEFINE_ERROR_INFO_AUTH(
    AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_TOKEN_FILE_PATH,
        "Failed to read the ECS token file specified in the AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE environment variable."),
        AWS_DEFINE_ERROR_INFO_AUTH(
    AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_HOST,
        "Failed to establish connection. The specified host is not allowed. It must be a loopback address, ECS/EKS container host, or use HTTPS."),
};
/* clang-format on */

static struct aws_error_info_list s_error_list = {
    .error_list = s_errors,
    .count = sizeof(s_errors) / sizeof(struct aws_error_info),
};

static struct aws_log_subject_info s_auth_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_AUTH_GENERAL,
        "AuthGeneral",
        "Subject for aws-c-auth logging that defies categorization."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_AUTH_PROFILE, "AuthProfile", "Subject for config profile related logging."),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "AuthCredentialsProvider",
        "Subject for credentials provider related logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_AUTH_SIGNING, "AuthSigning", "Subject for AWS request signing logging."),
};

static struct aws_log_subject_info_list s_auth_log_subject_list = {
    .subject_list = s_auth_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_auth_log_subject_infos),
};

static bool s_library_initialized = false;
static struct aws_allocator *s_library_allocator = NULL;

void aws_auth_library_init(struct aws_allocator *allocator) {
    if (s_library_initialized) {
        return;
    }

    if (allocator) {
        s_library_allocator = allocator;
    } else {
        s_library_allocator = aws_default_allocator();
    }

    aws_sdkutils_library_init(s_library_allocator);
    aws_cal_library_init(s_library_allocator);
    aws_http_library_init(s_library_allocator);

    aws_register_error_info(&s_error_list);
    aws_register_log_subject_info_list(&s_auth_log_subject_list);

    AWS_FATAL_ASSERT(aws_signing_init_signing_tables(allocator) == AWS_OP_SUCCESS);
    s_library_initialized = true;
}

void aws_auth_library_clean_up(void) {
    if (!s_library_initialized) {
        return;
    }

    s_library_initialized = false;

    aws_signing_clean_up_signing_tables();
    aws_unregister_log_subject_info_list(&s_auth_log_subject_list);
    aws_unregister_error_info(&s_error_list);
    aws_http_library_clean_up();
    aws_cal_library_clean_up();
    aws_sdkutils_library_clean_up();
    s_library_allocator = NULL;
}
