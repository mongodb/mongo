/*
 * Copyright 2019-present MongoDB, Inc.
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

#ifndef MONGOCRYPT_KEY_PRIVATE_H
#define MONGOCRYPT_KEY_PRIVATE_H

#include "mongocrypt-buffer-private.h"
#include "mongocrypt-kek-private.h"
#include "mongocrypt-opts-private.h"

/* A linked list of key alt names */
typedef struct __mongocrypt_key_alt_name_t {
    struct __mongocrypt_key_alt_name_t *next;
    bson_value_t value;
} _mongocrypt_key_alt_name_t;

typedef struct {
    bson_t bson; /* original BSON for this key. */
    _mongocrypt_buffer_t id;
    _mongocrypt_key_alt_name_t *key_alt_names;
    _mongocrypt_buffer_t key_material;
    int64_t creation_date;
    int64_t update_date;
    _mongocrypt_kek_t kek;
} _mongocrypt_key_doc_t;

_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_new(const bson_value_t *value);

bool _mongocrypt_key_alt_name_from_iter(const bson_iter_t *iter,
                                        _mongocrypt_key_alt_name_t **out,
                                        mongocrypt_status_t *status);

_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_copy_all(_mongocrypt_key_alt_name_t *list);
void _mongocrypt_key_alt_name_destroy_all(_mongocrypt_key_alt_name_t *list);
bool _mongocrypt_key_alt_name_intersects(_mongocrypt_key_alt_name_t *list_a, _mongocrypt_key_alt_name_t *list_b);
bool _mongocrypt_key_parse_owned(const bson_t *bson,
                                 _mongocrypt_key_doc_t *out,
                                 mongocrypt_status_t *status) MONGOCRYPT_WARN_UNUSED_RESULT;

_mongocrypt_key_doc_t *_mongocrypt_key_new(void);

void _mongocrypt_key_doc_copy_to(_mongocrypt_key_doc_t *src, _mongocrypt_key_doc_t *dst);

void _mongocrypt_key_destroy(_mongocrypt_key_doc_t *key);

const char *_mongocrypt_key_alt_name_get_string(_mongocrypt_key_alt_name_t *key_alt_name);

/* Begin: Functions for tests. */
/* Are the two lists equal without ordering.  */
bool _mongocrypt_key_alt_name_unique_list_equal(_mongocrypt_key_alt_name_t *list_a, _mongocrypt_key_alt_name_t *list_b);

/* For testing, construct a list of key alt names from variadic args */
_mongocrypt_key_alt_name_t *_mongocrypt_key_alt_name_create(const char *name, ...);
#define _MONGOCRYPT_KEY_ALT_NAME_CREATE(...) _mongocrypt_key_alt_name_create(__VA_ARGS__, NULL)
/* End: Functions for tests. */

#endif /* MONGOCRYPT_KEY_PRIVATE_H */
