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

/* RFC 3986 section 2.3: unreserved = ALPHA / DIGIT / "-" / "." / "_" / "~" */
static bool _is_unreserved(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'
        || c == '_' || c == '~';
}

/* RFC 3986 section 2.2: reserved = gen-delims / sub-delims */
static bool _is_reserved(char c) {
    return NULL
        != strchr(":/?#[]@" /* gen-delims */
                  "!$&'()*+,;=" /* sub-delims */,
                  c);
}

static bool _is_hexdig(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* _endpoint_chars_are_valid returns true if `endpoint_raw` contains only characters permitted in a URI
 * by RFC 3986. Useful to prevent malformed endpoints being used to construct HTTP requests. */
static bool _endpoint_chars_are_valid(const char *endpoint_raw, mongocrypt_status_t *status) {
    for (const char *c = endpoint_raw; *c != '\0'; c++) {
        if (_is_unreserved(*c) || _is_reserved(*c)) {
            continue;
        }
        if (*c == '%') {
            /* Returns false if either character is the NUL terminator. */
            if (!_is_hexdig(c[1])) {
                CLIENT_ERR("Invalid percent-encoding in endpoint at offset %zu: 0x%02x",
                           (size_t)(c - endpoint_raw) + 1,
                           (unsigned char)c[1]);
                return false;
            }
            if (!_is_hexdig(c[2])) {
                CLIENT_ERR("Invalid percent-encoding in endpoint at offset %zu: 0x%02x",
                           (size_t)(c - endpoint_raw) + 2,
                           (unsigned char)c[2]);
                return false;
            }
            c += 2;
            continue;
        }
        CLIENT_ERR("Invalid character in endpoint at offset %zu: 0x%02x",
                   (size_t)(c - endpoint_raw),
                   (unsigned char)*c);
        return false;
    }
    return true;
}

/* RFC 3986 section 2.2: sub-delims. Permitted in a reg-name; gen-delims are not. */
static bool _is_sub_delim(char c) {
    return NULL != strchr("!$&'()*+,;=", c);
}

/* _host_chars_are_valid returns true if `host` is an RFC 3986 section 3.2.2 `reg-name`:
 *
 *   reg-name = *( unreserved / pct-encoded / sub-delims )
 *
 * The gen-delims (":/?#[]@") are not permitted. The parse above already ends the host at ':', '/', and '?',
 * so in practice this rejects '#', '@', and the brackets of an IP-literal.
 *
 * This is defense in depth. `host` reaches the Host header, where CR and LF are the only characters that can
 * alter the message and kms-message rejects them, and `host_and_port` is passed to the consumer as the
 * address to connect to, where a gen-delim yields a name that does not resolve rather than a structural
 * problem. An `IP-literal` (RFC 3986 section 3.2.2) is not supported: the parse splits the host at the first
 * colon with no bracket handling, so "[2001:db8::1]" yielded a host of "[2001" and a port of "db8::1]". */
static bool _host_chars_are_valid(const char *host, mongocrypt_status_t *status) {
    for (const char *c = host; *c != '\0'; c++) {
        if (_is_unreserved(*c) || _is_sub_delim(*c)) {
            continue;
        }
        if (*c == '%') {
            /* Already validated by _endpoint_chars_are_valid. */
            c += 2;
            continue;
        }
        CLIENT_ERR("Invalid character in endpoint host: 0x%02x", (unsigned char)*c);
        return false;
    }
    return true;
}

/* _port_chars_are_valid returns true if `port` is a non-empty run of digits. */
static bool _port_chars_are_valid(const char *port, mongocrypt_status_t *status) {
    if (*port == '\0') {
        CLIENT_ERR("Invalid endpoint, expected a port after the colon");
        return false;
    }
    for (const char *c = port; *c != '\0'; c++) {
        if (*c < '0' || *c > '9') {
            CLIENT_ERR("Invalid character in endpoint port: 0x%02x", (unsigned char)*c);
            return false;
        }
    }
    return true;
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

    /* Validate before parsing: every field parsed below is a substring of `original`. */
    if (!_endpoint_chars_are_valid(endpoint->original, status)) {
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
    /* The host ends at the first delimiter. A colon delimits a port only if it
     * precedes the path and query: in "example.com/path:8080" the colon is part
     * of the path, not a port separator. */
    host_end = colon;
    if (slash && (!host_end || slash < host_end)) {
        host_end = slash;
    }
    if (qmark && (!host_end || qmark < host_end)) {
        host_end = qmark;
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

    if (!_host_chars_are_valid(endpoint->host, status)) {
        goto fail;
    }

    /* Parse optional port */
    if (colon && colon == host_end) {
        prev = colon + 1;
        qmark = strstr(prev, "?");
        slash = strstr(prev, "/");
        if (slash && (!qmark || slash < qmark)) {
            endpoint->port = bson_strndup(prev, (size_t)(slash - prev));
        } else if (qmark) {
            BSON_ASSERT(qmark >= prev);
            endpoint->port = bson_strndup(prev, (size_t)(qmark - prev));
        } else {
            endpoint->port = bson_strdup(prev);
        }

        if (!_port_chars_are_valid(endpoint->port, status)) {
            goto fail;
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
