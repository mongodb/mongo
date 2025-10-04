/* MIT License
 *
 * Copyright (c) 2023 Brad House
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * SPDX-License-Identifier: MIT
 */
#include "ares_setup.h"
#include "ares.h"
#include "ares_private.h"
#include "ares__htable.h"
#include "ares__htable_szvp.h"

struct ares__htable_szvp {
  ares__htable_szvp_val_free_t free_val;
  ares__htable_t              *hash;
};

typedef struct {
  size_t               key;
  void                *val;
  ares__htable_szvp_t *parent;
} ares__htable_szvp_bucket_t;

void ares__htable_szvp_destroy(ares__htable_szvp_t *htable)
{
  if (htable == NULL) {
    return;
  }

  ares__htable_destroy(htable->hash);
  ares_free(htable);
}

static unsigned int hash_func(const void *key, unsigned int seed)
{
  const size_t *arg = key;
  return ares__htable_hash_FNV1a((const unsigned char *)arg, sizeof(*arg),
                                 seed);
}

static const void *bucket_key(const void *bucket)
{
  const ares__htable_szvp_bucket_t *arg = bucket;
  return &arg->key;
}

static void bucket_free(void *bucket)
{
  ares__htable_szvp_bucket_t *arg = bucket;

  if (arg->parent->free_val) {
    arg->parent->free_val(arg->val);
  }

  ares_free(arg);
}

static ares_bool_t key_eq(const void *key1, const void *key2)
{
  const size_t *k1 = key1;
  const size_t *k2 = key2;

  if (*k1 == *k2) {
    return ARES_TRUE;
  }

  return ARES_FALSE;
}

ares__htable_szvp_t *
  ares__htable_szvp_create(ares__htable_szvp_val_free_t val_free)
{
  ares__htable_szvp_t *htable = ares_malloc(sizeof(*htable));
  if (htable == NULL) {
    goto fail;
  }

  htable->hash =
    ares__htable_create(hash_func, bucket_key, bucket_free, key_eq);
  if (htable->hash == NULL) {
    goto fail;
  }

  htable->free_val = val_free;

  return htable;

fail:
  if (htable) {
    ares__htable_destroy(htable->hash);
    ares_free(htable);
  }
  return NULL;
}

ares_bool_t ares__htable_szvp_insert(ares__htable_szvp_t *htable, size_t key,
                                     void *val)
{
  ares__htable_szvp_bucket_t *bucket = NULL;

  if (htable == NULL) {
    goto fail;
  }

  bucket = ares_malloc(sizeof(*bucket));
  if (bucket == NULL) {
    goto fail;
  }

  bucket->parent = htable;
  bucket->key    = key;
  bucket->val    = val;

  if (!ares__htable_insert(htable->hash, bucket)) {
    goto fail;
  }

  return ARES_TRUE;

fail:
  if (bucket) {
    ares_free(bucket);
  }
  return ARES_FALSE;
}

ares_bool_t ares__htable_szvp_get(const ares__htable_szvp_t *htable, size_t key,
                                  void **val)
{
  ares__htable_szvp_bucket_t *bucket = NULL;

  if (val) {
    *val = NULL;
  }

  if (htable == NULL) {
    return ARES_FALSE;
  }

  bucket = ares__htable_get(htable->hash, &key);
  if (bucket == NULL) {
    return ARES_FALSE;
  }

  if (val) {
    *val = bucket->val;
  }
  return ARES_TRUE;
}

void *ares__htable_szvp_get_direct(const ares__htable_szvp_t *htable,
                                   size_t                     key)
{
  void *val = NULL;
  ares__htable_szvp_get(htable, key, &val);
  return val;
}

ares_bool_t ares__htable_szvp_remove(ares__htable_szvp_t *htable, size_t key)
{
  if (htable == NULL) {
    return ARES_FALSE;
  }

  return ares__htable_remove(htable->hash, &key);
}

size_t ares__htable_szvp_num_keys(const ares__htable_szvp_t *htable)
{
  if (htable == NULL) {
    return 0;
  }
  return ares__htable_num_keys(htable->hash);
}
