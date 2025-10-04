/*
 * Copyright 2018-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"){}
 *
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

#include "kms_kv_list.h"
#include "kms_message/kms_message.h"
#include "kms_message_private.h"
#include "kms_request_str.h"
#include "kms_port.h"
#include "sort.h"

static void
kv_init (kms_kv_t *kv, kms_request_str_t *key, kms_request_str_t *value)
{
   kv->key = kms_request_str_dup (key);
   kv->value = kms_request_str_dup (value);
}

static void
kv_cleanup (kms_kv_t *kv)
{
   kms_request_str_destroy (kv->key);
   kms_request_str_destroy (kv->value);
}

kms_kv_list_t *
kms_kv_list_new (void)
{
   kms_kv_list_t *lst = malloc (sizeof (kms_kv_list_t));
   KMS_ASSERT (lst);

   lst->size = 16;
   lst->kvs = malloc (lst->size * sizeof (kms_kv_t));
   KMS_ASSERT (lst->kvs);

   lst->len = 0;

   return lst;
}

void
kms_kv_list_destroy (kms_kv_list_t *lst)
{
   size_t i;

   if (!lst) {
      return;
   }

   for (i = 0; i < lst->len; i++) {
      kv_cleanup (&lst->kvs[i]);
   }

   free (lst->kvs);
   free (lst);
}

void
kms_kv_list_add (kms_kv_list_t *lst,
                 kms_request_str_t *key,
                 kms_request_str_t *value)
{
   if (lst->len == lst->size) {
      lst->size *= 2;
      lst->kvs = realloc (lst->kvs, lst->size * sizeof (kms_kv_t));
      KMS_ASSERT (lst->kvs);
   }

   kv_init (&lst->kvs[lst->len], key, value);
   ++lst->len;
}

const kms_kv_t *
kms_kv_list_find (const kms_kv_list_t *lst, const char *key)
{
   size_t i;

   for (i = 0; i < lst->len; i++) {
      if (0 == kms_strcasecmp (lst->kvs[i].key->str, key)) {
         return &lst->kvs[i];
      }
   }

   return NULL;
}

void
kms_kv_list_del (kms_kv_list_t *lst, const char *key)
{
   size_t i;

   for (i = 0; i < lst->len; i++) {
      if (0 == strcmp (lst->kvs[i].key->str, key)) {
         kv_cleanup (&lst->kvs[i]);
         memmove (&lst->kvs[i],
                  &lst->kvs[i + 1],
                  sizeof (kms_kv_t) * (lst->len - i - 1));
         lst->len--;
      }
   }
}

kms_kv_list_t *
kms_kv_list_dup (const kms_kv_list_t *lst)
{
   kms_kv_list_t *dup;
   size_t i;

   if (lst->len == 0) {
      return kms_kv_list_new ();
   }

   dup = malloc (sizeof (kms_kv_list_t));
   KMS_ASSERT (dup);

   dup->size = dup->len = lst->len;
   dup->kvs = malloc (lst->len * sizeof (kms_kv_t));
   KMS_ASSERT (dup->kvs);


   for (i = 0; i < lst->len; i++) {
      kv_init (&dup->kvs[i], lst->kvs[i].key, lst->kvs[i].value);
   }

   return dup;
}


void
kms_kv_list_sort (kms_kv_list_t *lst, int (*cmp) (const void *, const void *))
{
   /* A stable sort is required to sort headers when creating canonical
    * requests. qsort is not stable. */
   insertionsort (
      (unsigned char *) (lst->kvs), lst->len, sizeof (kms_kv_t), cmp);
}
