/*
 * Copyright 2018-present MongoDB, Inc.
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

#ifndef KMS_KV_LIST_H
#define KMS_KV_LIST_H

#include "kms_message/kms_message.h"
#include "kms_request_str.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* key-value pair */
typedef struct {
   kms_request_str_t *key;
   kms_request_str_t *value;
} kms_kv_t;

typedef struct {
   kms_kv_t *kvs;
   size_t len;
   size_t size;
} kms_kv_list_t;

kms_kv_list_t *
kms_kv_list_new (void);
void
kms_kv_list_destroy (kms_kv_list_t *lst);
void
kms_kv_list_add (kms_kv_list_t *lst,
                 kms_request_str_t *key,
                 kms_request_str_t *value);
const kms_kv_t *
kms_kv_list_find (const kms_kv_list_t *lst, const char *key);
void
kms_kv_list_del (kms_kv_list_t *lst, const char *key);
kms_kv_list_t *
kms_kv_list_dup (const kms_kv_list_t *lst);
void
kms_kv_list_sort (kms_kv_list_t *lst, int (*cmp) (const void *, const void *));

#endif /* KMS_KV_LIST_H */
