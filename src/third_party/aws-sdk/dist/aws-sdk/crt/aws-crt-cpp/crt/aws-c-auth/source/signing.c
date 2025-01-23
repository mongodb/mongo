/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signing.h>

#include <aws/auth/credentials.h>
#include <aws/auth/private/aws_signing.h>
#include <aws/io/uri.h>

/*
 * Aws signing implementation
 */

static int s_aws_last_error_or_unknown(void) {
    int last_error = aws_last_error();
    if (last_error == AWS_ERROR_SUCCESS) {
        last_error = AWS_ERROR_UNKNOWN;
    }

    return last_error;
}

static void s_perform_signing(struct aws_signing_state_aws *state) {
    struct aws_signing_result *result = NULL;

    if (state->error_code != AWS_ERROR_SUCCESS) {
        goto done;
    }

    if (aws_credentials_is_anonymous(state->config.credentials)) {
        result = &state->result;
        goto done;
    }

    if (aws_signing_build_canonical_request(state)) {
        state->error_code = s_aws_last_error_or_unknown();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING,
            "(id=%p) Signing failed to build canonical request via algorithm %s, error %d(%s)",
            (void *)state->signable,
            aws_signing_algorithm_to_string(state->config.algorithm),
            state->error_code,
            aws_error_debug_str(state->error_code));
        goto done;
    }

    AWS_LOGF_INFO(
        AWS_LS_AUTH_SIGNING,
        "(id=%p) Signing successfully built canonical request for algorithm %s, with contents \n" PRInSTR "\n",
        (void *)state->signable,
        aws_signing_algorithm_to_string(state->config.algorithm),
        AWS_BYTE_BUF_PRI(state->canonical_request));

    if (aws_signing_build_string_to_sign(state)) {
        state->error_code = s_aws_last_error_or_unknown();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING,
            "(id=%p) Signing failed to build string-to-sign via algorithm %s, error %d(%s)",
            (void *)state->signable,
            aws_signing_algorithm_to_string(state->config.algorithm),
            state->error_code,
            aws_error_debug_str(state->error_code));
        goto done;
    }

    AWS_LOGF_INFO(
        AWS_LS_AUTH_SIGNING,
        "(id=%p) Signing successfully built string-to-sign via algorithm %s, with contents \n" PRInSTR "\n",
        (void *)state->signable,
        aws_signing_algorithm_to_string(state->config.algorithm),
        AWS_BYTE_BUF_PRI(state->string_to_sign));

    if (aws_signing_build_authorization_value(state)) {
        state->error_code = s_aws_last_error_or_unknown();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING,
            "(id=%p) Signing failed to build final authorization value via algorithm %s",
            (void *)state->signable,
            aws_signing_algorithm_to_string(state->config.algorithm));
        goto done;
    }

    result = &state->result;

done:

    state->on_complete(result, state->error_code, state->userdata);
    aws_signing_state_destroy(state);
}

static void s_aws_signing_on_get_credentials(struct aws_credentials *credentials, int error_code, void *user_data) {
    struct aws_signing_state_aws *state = user_data;

    if (!credentials) {
        if (error_code == AWS_ERROR_SUCCESS) {
            error_code = AWS_ERROR_UNKNOWN;
        }

        /* Log the credentials sourcing error */
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING,
            "(id=%p) Credentials Provider failed to source credentials with error %d(%s)",
            (void *)state->signable,
            error_code,
            aws_error_debug_str(error_code));

        state->error_code = AWS_AUTH_SIGNING_NO_CREDENTIALS;
    } else {
        if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC &&
            !aws_credentials_is_anonymous(credentials)) {

            state->config.credentials = aws_credentials_new_ecc_from_aws_credentials(state->allocator, credentials);
            if (state->config.credentials == NULL) {
                state->error_code = AWS_AUTH_SIGNING_NO_CREDENTIALS;
            }
        } else {
            state->config.credentials = credentials;
            aws_credentials_acquire(credentials);
        }
    }

    s_perform_signing(state);
}

int aws_sign_request_aws(
    struct aws_allocator *allocator,
    const struct aws_signable *signable,
    const struct aws_signing_config_base *base_config,
    aws_signing_complete_fn *on_complete,
    void *userdata) {

    AWS_PRECONDITION(base_config);

    if (base_config->config_type != AWS_SIGNING_CONFIG_AWS) {
        return aws_raise_error(AWS_AUTH_SIGNING_MISMATCHED_CONFIGURATION);
    }

    const struct aws_signing_config_aws *config = (void *)base_config;

    struct aws_signing_state_aws *signing_state =
        aws_signing_state_new(allocator, config, signable, on_complete, userdata);
    if (!signing_state) {
        return AWS_OP_ERR;
    }

    if (signing_state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
        if (signing_state->config.credentials != NULL &&
            !aws_credentials_is_anonymous(signing_state->config.credentials)) {
            /*
             * If these are regular credentials, try to derive ecc-based ones
             */
            if (aws_credentials_get_ecc_key_pair(signing_state->config.credentials) == NULL) {
                struct aws_credentials *ecc_credentials =
                    aws_credentials_new_ecc_from_aws_credentials(allocator, signing_state->config.credentials);
                aws_credentials_release(signing_state->config.credentials);
                signing_state->config.credentials = ecc_credentials;
                if (signing_state->config.credentials == NULL) {
                    goto on_error;
                }
            }
        }
    }

    bool can_sign_immediately = signing_state->config.credentials != NULL;

    if (can_sign_immediately) {
        s_perform_signing(signing_state);
    } else {
        if (aws_credentials_provider_get_credentials(
                signing_state->config.credentials_provider, s_aws_signing_on_get_credentials, signing_state)) {
            goto on_error;
        }
    }

    return AWS_OP_SUCCESS;

on_error:

    aws_signing_state_destroy(signing_state);
    return AWS_OP_ERR;
}
