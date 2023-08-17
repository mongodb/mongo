/*
 * Copyright 2020-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mongocrypt-endpoint-private.h"

#include "mongocrypt-private.h"

void _mongocrypt_endpoint_destroy(_mongocrypt_endpoint_t *endpoint) {
    if (!endpoint) {
        return;
    }
    bson_free(endpoint->original);
    bson_free(endpoint->protocol);
    bson_free(endpoint->host);
    bson_free(endpoint->port);
    bson_free(endpoint->domain);
    bson_free(endpoint->subdomain);
    bson_free(endpoint->path);
    bson_free(endpoint->query);
    bson_free(endpoint->host_and_port);
    bson_free(endpoint);
}

/* Parses a subset of URIs of the form:
 * [protocol://][host[:port]][path][?query]
 */
_mongocrypt_endpoint_t *_mongocrypt_endpoint_new(const char *endpoint_raw,
                                                 int32_t len,
                                                 _mongocrypt_endpoint_parse_opts_t *opts,
                                                 mongocrypt_status_t *status) {
    _mongocrypt_endpoint_t *endpoint;
    bool ok = false;
    char *pos;
    char *prev;
    char *colon;
    char *qmark;
    char *slash;
    char *host_start;
    char *host_end;

    /* opts is checked where it is used below, to allow a more precise error */

    endpoint = bson_malloc0(sizeof(_mongocrypt_endpoint_t));
    _mongocrypt_status_reset(status);
    BSON_ASSERT(endpoint);
    if (!_mongocrypt_validate_and_copy_string(endpoint_raw, len, &endpoint->original)) {
        CLIENT_ERR("Invalid endpoint");
        goto fail;
    }

    /* Parse optional protocol. */
    pos = strstr(endpoint->original, "://");
    if (pos) {
        endpoint->protocol = bson_strndup(endpoint->original, (size_t)(pos - endpoint->original));
        pos += 3;
    } else {
        pos = endpoint->original;
    }
    host_start = pos;

    /* Parse subdomain. */
    prev = pos;
    pos = strstr(pos, ".");
    if (pos) {
        BSON_ASSERT(pos >= prev);
        endpoint->subdomain = bson_strndup(prev, (size_t)(pos - prev));
        pos += 1;
    } else {
        if (!opts || !opts->allow_empty_subdomain) {
            CLIENT_ERR("Invalid endpoint, expected dot separator in host, but got: %s", endpoint->original);
            goto fail;
        }
        /* OK, reset pos to the start of the host. */
        pos = prev;
    }

    /* Parse domain. */
    prev = pos;
    colon = strstr(pos, ":");
    qmark = strstr(pos, "?");
    slash = strstr(pos, "/");
    if (colon) {
        host_end = colon;
    } else if (slash) {
        host_end = slash;
    } else if (qmark) {
        host_end = qmark;
    } else {
        host_end = NULL;
    }

    if (host_end) {
        BSON_ASSERT(host_end >= prev);
        endpoint->domain = bson_strndup(prev, (size_t)(host_end - prev));
        BSON_ASSERT(host_end >= host_start);
        endpoint->host = bson_strndup(host_start, (size_t)(host_end - host_start));
    } else {
        endpoint->domain = bson_strdup(prev);
        endpoint->host = bson_strdup(host_start);
    }

    /* Parse optional port */
    if (colon) {
        prev = colon + 1;
        qmark = strstr(pos, "?");
        slash = strstr(pos, "/");
        if (slash) {
            endpoint->port = bson_strndup(prev, (size_t)(slash - prev));
        } else if (qmark) {
            BSON_ASSERT(qmark >= prev);
            endpoint->port = bson_strndup(prev, (size_t)(qmark - prev));
        } else {
            endpoint->port = bson_strdup(prev);
        }
    }

    /* Parse optional path */
    if (slash) {
        size_t path_len;

        prev = slash + 1;
        qmark = strstr(prev, "?");
        if (qmark) {
            endpoint->path = bson_strndup(prev, (size_t)(qmark - prev));
        } else {
            endpoint->path = bson_strdup(prev);
        }

        path_len = strlen(endpoint->path);
        /* Clear a trailing slash if it exists. */
        if (path_len > 0 && endpoint->path[path_len - 1] == '/') {
            endpoint->path[path_len - 1] = '\0';
        }
    }

    /* Parse optional query */
    if (qmark) {
        endpoint->query = bson_strdup(qmark + 1);
    }

    if (endpoint->port) {
        endpoint->host_and_port = bson_strdup_printf("%s:%s", endpoint->host, endpoint->port);
    } else {
        endpoint->host_and_port = bson_strdup(endpoint->host);
    }

    ok = true;
fail:
    if (!ok) {
        _mongocrypt_endpoint_destroy(endpoint);
        return NULL;
    }
    return endpoint;
}

_mongocrypt_endpoint_t *_mongocrypt_endpoint_copy(_mongocrypt_endpoint_t *src) {
    _mongocrypt_endpoint_t *endpoint;

    if (!src) {
        return NULL;
    }
    endpoint = bson_malloc0(sizeof(_mongocrypt_endpoint_t));
    endpoint->original = bson_strdup(src->original);
    endpoint->protocol = bson_strdup(src->protocol);
    endpoint->host = bson_strdup(src->host);
    endpoint->port = bson_strdup(src->port);
    endpoint->domain = bson_strdup(src->domain);
    endpoint->subdomain = bson_strdup(src->subdomain);
    endpoint->path = bson_strdup(src->path);
    endpoint->query = bson_strdup(src->query);
    endpoint->host_and_port = bson_strdup(src->host_and_port);
    return endpoint;
}

void _mongocrypt_apply_default_port(char **endpoint_raw, char *port) {
    BSON_ASSERT_PARAM(endpoint_raw);
    BSON_ASSERT_PARAM(port);
    BSON_ASSERT(*endpoint_raw);

    if (strstr(*endpoint_raw, ":") == NULL) {
        char *tmp = *endpoint_raw;
        *endpoint_raw = bson_strdup_printf("%s:%s", *endpoint_raw, port);
        bson_free(tmp);
    }
}
