/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/sso_token_utils.h>
#include <aws/cal/ecc.h>
#include <aws/common/environment.h>
#include <aws/common/string.h>

/* aws ecc identity which contains the data needed to sign a Sigv4a AWS request */
struct aws_ecc_identity {
    struct aws_string *access_key_id;
    struct aws_string *session_token;
    struct aws_ecc_key_pair *ecc_key;
};

/* aws credentials identity which contains the data needed to sign an authenticated AWS request */
struct aws_credentials_identity {
    struct aws_string *access_key_id;
    struct aws_string *secret_access_key;
    struct aws_string *session_token;
};

/* aws_token identity contains only a token to represent token only identities like a bearer token. */
struct aws_token_identity {
    struct aws_string *token;
};

enum aws_identity_type {
    AWS_CREDENTIALS_IDENTITY,
    TOKEN_IDENTITY,
    ANONYMOUS_IDENTITY,
    ECC_IDENTITY,
};

/*
 * A structure that wraps the different types of credentials that the customer can provider to establish their
 * identity.
 */
struct aws_credentials {
    struct aws_allocator *allocator;

    struct aws_atomic_var ref_count;
    /*
     * A timepoint, in seconds since epoch, at which the credentials should no longer be used because they
     * will have expired.
     *
     *
     * The primary purpose of this value is to allow providers to communicate to the caching provider any
     * additional constraints on how the sourced credentials should be used (STS).  After refreshing the cached
     * credentials, the caching provider uses the following calculation to determine the next requery time:
     *
     *   next_requery_time = now + cached_expiration_config;
     *   if (cached_creds->expiration_timepoint_seconds < next_requery_time) {
     *       next_requery_time = cached_creds->expiration_timepoint_seconds;
     *
     *  The cached provider may, at its discretion, use a smaller requery time to avoid edge-case scenarios where
     *  credential expiration becomes a race condition.
     *
     * The following leaf providers always set this value to UINT64_MAX (indefinite):
     *    static
     *    environment
     *    imds
     *    profile_config*
     *
     *  * - profile_config may invoke sts which will use a non-max value
     *
     *  The following leaf providers set this value to a sensible timepoint:
     *    sts - value is based on current time + options->duration_seconds
     *
     */
    uint64_t expiration_timepoint_seconds;

    enum aws_identity_type identity_type;
    union {
        struct aws_credentials_identity credentials_identity;
        struct aws_token_identity token_identity;
        struct aws_ecc_identity ecc_identity;
    } identity;
};

/*
 * Credentials API implementations
 */
struct aws_credentials *aws_credentials_new(
    struct aws_allocator *allocator,
    struct aws_byte_cursor access_key_id_cursor,
    struct aws_byte_cursor secret_access_key_cursor,
    struct aws_byte_cursor session_token_cursor,
    uint64_t expiration_timepoint_seconds) {

    if (access_key_id_cursor.ptr == NULL || access_key_id_cursor.len == 0) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (secret_access_key_cursor.ptr == NULL || secret_access_key_cursor.len == 0) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_credentials *credentials = aws_mem_acquire(allocator, sizeof(struct aws_credentials));
    if (credentials == NULL) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*credentials);

    credentials->allocator = allocator;
    aws_atomic_init_int(&credentials->ref_count, 1);
    credentials->identity_type = AWS_CREDENTIALS_IDENTITY;
    struct aws_credentials_identity *credentials_identity = &credentials->identity.credentials_identity;
    credentials_identity->access_key_id =
        aws_string_new_from_array(allocator, access_key_id_cursor.ptr, access_key_id_cursor.len);
    if (credentials_identity->access_key_id == NULL) {
        goto error;
    }

    credentials_identity->secret_access_key =
        aws_string_new_from_array(allocator, secret_access_key_cursor.ptr, secret_access_key_cursor.len);
    if (credentials_identity->secret_access_key == NULL) {
        goto error;
    }

    if (session_token_cursor.ptr != NULL && session_token_cursor.len > 0) {
        credentials_identity->session_token =
            aws_string_new_from_array(allocator, session_token_cursor.ptr, session_token_cursor.len);
        if (credentials_identity->session_token == NULL) {
            goto error;
        }
    }

    credentials->expiration_timepoint_seconds = expiration_timepoint_seconds;

    return credentials;

error:

    aws_credentials_release(credentials);

    return NULL;
}

struct aws_credentials *aws_credentials_new_anonymous(struct aws_allocator *allocator) {

    struct aws_credentials *credentials = aws_mem_calloc(allocator, 1, sizeof(struct aws_credentials));

    credentials->allocator = allocator;
    credentials->identity_type = ANONYMOUS_IDENTITY;
    aws_atomic_init_int(&credentials->ref_count, 1);

    credentials->expiration_timepoint_seconds = UINT64_MAX;

    return credentials;
}

static void s_aws_credentials_destroy(struct aws_credentials *credentials) {
    if (credentials == NULL) {
        return;
    }
    switch (credentials->identity_type) {
        case AWS_CREDENTIALS_IDENTITY:
            aws_string_destroy(credentials->identity.credentials_identity.access_key_id);
            aws_string_destroy_secure(credentials->identity.credentials_identity.secret_access_key);
            aws_string_destroy_secure(credentials->identity.credentials_identity.session_token);
            break;
        case ECC_IDENTITY:
            aws_string_destroy(credentials->identity.ecc_identity.access_key_id);
            aws_string_destroy_secure(credentials->identity.ecc_identity.session_token);
            aws_ecc_key_pair_release(credentials->identity.ecc_identity.ecc_key);
            break;
        case TOKEN_IDENTITY:
            aws_string_destroy_secure(credentials->identity.token_identity.token);
            break;
        case ANONYMOUS_IDENTITY:
            break;
    }

    aws_mem_release(credentials->allocator, credentials);
}

void aws_credentials_acquire(const struct aws_credentials *credentials) {
    if (credentials == NULL) {
        return;
    }

    aws_atomic_fetch_add((struct aws_atomic_var *)&credentials->ref_count, 1);
}

void aws_credentials_release(const struct aws_credentials *credentials) {
    if (credentials == NULL) {
        return;
    }

    size_t old_value = aws_atomic_fetch_sub((struct aws_atomic_var *)&credentials->ref_count, 1);
    if (old_value == 1) {
        s_aws_credentials_destroy((struct aws_credentials *)credentials);
    }
}

static struct aws_byte_cursor s_empty_token_cursor = {
    .ptr = NULL,
    .len = 0,
};

struct aws_byte_cursor aws_credentials_get_access_key_id(const struct aws_credentials *credentials) {
    switch (credentials->identity_type) {
        case AWS_CREDENTIALS_IDENTITY:
            if (credentials->identity.credentials_identity.access_key_id != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.credentials_identity.access_key_id);
            }
            break;
        case ECC_IDENTITY:
            if (credentials->identity.ecc_identity.access_key_id != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.ecc_identity.access_key_id);
            }
            break;
        default:
            break;
    }
    return s_empty_token_cursor;
}

struct aws_byte_cursor aws_credentials_get_secret_access_key(const struct aws_credentials *credentials) {
    switch (credentials->identity_type) {
        case AWS_CREDENTIALS_IDENTITY:
            if (credentials->identity.credentials_identity.secret_access_key != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.credentials_identity.secret_access_key);
            }
            break;
        default:
            break;
    }
    return s_empty_token_cursor;
}

struct aws_byte_cursor aws_credentials_get_session_token(const struct aws_credentials *credentials) {
    switch (credentials->identity_type) {
        case AWS_CREDENTIALS_IDENTITY:
            if (credentials->identity.credentials_identity.session_token != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.credentials_identity.session_token);
            }
            break;
        case ECC_IDENTITY:
            if (credentials->identity.ecc_identity.session_token != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.ecc_identity.session_token);
            }
            break;
        default:
            break;
    }
    return s_empty_token_cursor;
}

struct aws_byte_cursor aws_credentials_get_token(const struct aws_credentials *credentials) {
    switch (credentials->identity_type) {
        case TOKEN_IDENTITY:
            if (credentials->identity.token_identity.token != NULL) {
                return aws_byte_cursor_from_string(credentials->identity.token_identity.token);
            }
            break;
        default:
            break;
    }
    return s_empty_token_cursor;
}

uint64_t aws_credentials_get_expiration_timepoint_seconds(const struct aws_credentials *credentials) {
    return credentials->expiration_timepoint_seconds;
}

struct aws_ecc_key_pair *aws_credentials_get_ecc_key_pair(const struct aws_credentials *credentials) {
    if (credentials->identity_type == ECC_IDENTITY) {
        return credentials->identity.ecc_identity.ecc_key;
    }
    return NULL;
}

bool aws_credentials_is_anonymous(const struct aws_credentials *credentials) {
    AWS_PRECONDITION(credentials);
    return credentials->identity_type == ANONYMOUS_IDENTITY;
}

struct aws_credentials *aws_credentials_new_from_string(
    struct aws_allocator *allocator,
    const struct aws_string *access_key_id,
    const struct aws_string *secret_access_key,
    const struct aws_string *session_token,
    uint64_t expiration_timepoint_seconds) {
    struct aws_byte_cursor access_key_cursor = aws_byte_cursor_from_string(access_key_id);
    struct aws_byte_cursor secret_access_key_cursor = aws_byte_cursor_from_string(secret_access_key);
    struct aws_byte_cursor session_token_cursor;
    AWS_ZERO_STRUCT(session_token_cursor);

    if (session_token) {
        session_token_cursor = aws_byte_cursor_from_string(session_token);
    }

    return aws_credentials_new(
        allocator, access_key_cursor, secret_access_key_cursor, session_token_cursor, expiration_timepoint_seconds);
}

struct aws_credentials *aws_credentials_new_ecc(
    struct aws_allocator *allocator,
    struct aws_byte_cursor access_key_id,
    struct aws_ecc_key_pair *ecc_key,
    struct aws_byte_cursor session_token,
    uint64_t expiration_timepoint_in_seconds) {

    if (access_key_id.len == 0 || ecc_key == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_GENERAL, "Provided credentials do not have a valid access_key_id or ecc_key");
        return NULL;
    }

    struct aws_credentials *credentials = aws_mem_calloc(allocator, 1, sizeof(struct aws_credentials));
    if (credentials == NULL) {
        return NULL;
    }

    credentials->allocator = allocator;
    credentials->expiration_timepoint_seconds = expiration_timepoint_in_seconds;
    aws_atomic_init_int(&credentials->ref_count, 1);
    aws_ecc_key_pair_acquire(ecc_key);
    credentials->identity_type = ECC_IDENTITY;
    credentials->identity.ecc_identity.ecc_key = ecc_key;

    credentials->identity.ecc_identity.access_key_id =
        aws_string_new_from_array(allocator, access_key_id.ptr, access_key_id.len);
    if (credentials->identity.ecc_identity.access_key_id == NULL) {
        goto on_error;
    }

    if (session_token.ptr != NULL && session_token.len > 0) {
        credentials->identity.ecc_identity.session_token =
            aws_string_new_from_array(allocator, session_token.ptr, session_token.len);
        if (credentials->identity.ecc_identity.session_token == NULL) {
            goto on_error;
        }
    }

    return credentials;

on_error:

    s_aws_credentials_destroy(credentials);

    return NULL;
}

struct aws_credentials *aws_credentials_new_ecc_from_aws_credentials(
    struct aws_allocator *allocator,
    const struct aws_credentials *credentials) {

    struct aws_ecc_key_pair *ecc_key = aws_ecc_key_pair_new_ecdsa_p256_key_from_aws_credentials(allocator, credentials);

    if (ecc_key == NULL) {
        return NULL;
    }

    struct aws_credentials *ecc_credentials = aws_credentials_new_ecc(
        allocator,
        aws_credentials_get_access_key_id(credentials),
        ecc_key,
        aws_credentials_get_session_token(credentials),
        aws_credentials_get_expiration_timepoint_seconds(credentials));

    aws_ecc_key_pair_release(ecc_key);

    return ecc_credentials;
}

struct aws_credentials *aws_credentials_new_token(
    struct aws_allocator *allocator,
    struct aws_byte_cursor token,
    uint64_t expiration_timepoint_in_seconds) {
    if (token.ptr == NULL || token.len == 0) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }
    struct aws_credentials *credentials = aws_mem_calloc(allocator, 1, sizeof(struct aws_credentials));

    credentials->allocator = allocator;
    aws_atomic_init_int(&credentials->ref_count, 1);
    credentials->identity_type = TOKEN_IDENTITY;
    struct aws_token_identity *token_identity = &credentials->identity.token_identity;
    token_identity->token = aws_string_new_from_array(allocator, token.ptr, token.len);
    credentials->expiration_timepoint_seconds = expiration_timepoint_in_seconds;
    return credentials;
}

/*
 * global credentials provider APIs
 */

void aws_credentials_provider_destroy(struct aws_credentials_provider *provider) {
    if (provider != NULL) {
        provider->vtable->destroy(provider);
    }
}

struct aws_credentials_provider *aws_credentials_provider_release(struct aws_credentials_provider *provider) {
    if (provider == NULL) {
        return NULL;
    }

    size_t old_value = aws_atomic_fetch_sub(&provider->ref_count, 1);
    if (old_value == 1) {
        aws_credentials_provider_destroy(provider);
    }

    return NULL;
}

struct aws_credentials_provider *aws_credentials_provider_acquire(struct aws_credentials_provider *provider) {
    if (provider == NULL) {
        return NULL;
    }

    aws_atomic_fetch_add(&provider->ref_count, 1);

    return provider;
}

int aws_credentials_provider_get_credentials(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    AWS_ASSERT(provider->vtable->get_credentials);

    return provider->vtable->get_credentials(provider, callback, user_data);
}
