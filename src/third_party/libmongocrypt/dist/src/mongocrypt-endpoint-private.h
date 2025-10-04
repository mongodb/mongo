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

#ifndef MONGOCRYPT_ENDPOINT_PRIVATE_H
#define MONGOCRYPT_ENDPOINT_PRIVATE_H

#include "mongocrypt-status-private.h"

typedef struct {
    /* e.g. https://kevin.keyvault.azure.net:443/path/path/?query=value */
    char *original;
    char *protocol;  /* e.g. https */
    char *host;      /* e.g. kevin.keyvault.azure.net */
    char *port;      /* e.g. 443 */
    char *domain;    /* e.g. keyvault.azure.net */
    char *subdomain; /* e.g. kevin */
    char *path;      /* e.g. path/path */
    char *query;     /* e.g. query=value */
    /* host_and_port is the form that should be returned to drivers. */
    char *host_and_port; /* e.g. kevin.keyvault.azure.net:443 */
} _mongocrypt_endpoint_t;

_mongocrypt_endpoint_t *_mongocrypt_endpoint_copy(_mongocrypt_endpoint_t *src);

void _mongocrypt_endpoint_destroy(_mongocrypt_endpoint_t *endpoint);

typedef struct {
    /* allow_empty_subdomain does not require "host" to contain dot separators.
     * If allow_empty_subdomain is true, then "localhost" is a valid endpoint. */
    bool allow_empty_subdomain;
} _mongocrypt_endpoint_parse_opts_t;

/* Parses a subset of URIs of the form:
 * [protocol://][host[:port]][path][?query]
 */
_mongocrypt_endpoint_t *_mongocrypt_endpoint_new(const char *endpoint_raw,
                                                 int32_t len,
                                                 _mongocrypt_endpoint_parse_opts_t *opts,
                                                 mongocrypt_status_t *status);

/* _mongocrypt_apply_default_port checks if the endpoint string *endpoint_raw
 * contains a port. If *endpoint_raw does not contain a port, *endpoint_raw is
 * freed and overwritten to a copy of *endpoint_raw with ":<port>" appended.
 */
void _mongocrypt_apply_default_port(char **endpoint_raw, char *port);

#endif /* MONGOCRYPT_ENDPOINT_PRIVATE_H */
