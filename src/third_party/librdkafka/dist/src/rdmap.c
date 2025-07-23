/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2020 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rdsysqueue.h"
#include "rdstring.h"
#include "rdmap.h"


static RD_INLINE int rd_map_elem_cmp(const rd_map_elem_t *a,
                                     const rd_map_elem_t *b,
                                     const rd_map_t *rmap) {
        int r = a->hash - b->hash;
        if (r != 0)
                return r;
        return rmap->rmap_cmp(a->key, b->key);
}

static void rd_map_elem_destroy(rd_map_t *rmap, rd_map_elem_t *elem) {
        rd_assert(rmap->rmap_cnt > 0);
        rmap->rmap_cnt--;
        if (rmap->rmap_destroy_key)
                rmap->rmap_destroy_key((void *)elem->key);
        if (rmap->rmap_destroy_value)
                rmap->rmap_destroy_value((void *)elem->value);
        LIST_REMOVE(elem, hlink);
        LIST_REMOVE(elem, link);
        rd_free(elem);
}

static rd_map_elem_t *
rd_map_find(const rd_map_t *rmap, int *bktp, const rd_map_elem_t *skel) {
        int bkt = skel->hash % rmap->rmap_buckets.cnt;
        rd_map_elem_t *elem;

        if (bktp)
                *bktp = bkt;

        LIST_FOREACH(elem, &rmap->rmap_buckets.p[bkt], hlink) {
                if (!rd_map_elem_cmp(skel, elem, rmap))
                        return elem;
        }

        return NULL;
}


/**
 * @brief Create and return new element based on \p skel without value set.
 */
static rd_map_elem_t *
rd_map_insert(rd_map_t *rmap, int bkt, const rd_map_elem_t *skel) {
        rd_map_elem_t *elem;

        elem       = rd_calloc(1, sizeof(*elem));
        elem->hash = skel->hash;
        elem->key  = skel->key; /* takes ownership of key */
        LIST_INSERT_HEAD(&rmap->rmap_buckets.p[bkt], elem, hlink);
        LIST_INSERT_HEAD(&rmap->rmap_iter, elem, link);
        rmap->rmap_cnt++;

        return elem;
}


rd_map_elem_t *rd_map_set(rd_map_t *rmap, void *key, void *value) {
        rd_map_elem_t skel = {.key = key, .hash = rmap->rmap_hash(key)};
        rd_map_elem_t *elem;
        int bkt;

        if (!(elem = rd_map_find(rmap, &bkt, &skel))) {
                elem = rd_map_insert(rmap, bkt, &skel);
        } else {
                if (elem->value && rmap->rmap_destroy_value)
                        rmap->rmap_destroy_value((void *)elem->value);
                if (rmap->rmap_destroy_key)
                        rmap->rmap_destroy_key(key);
        }

        elem->value = value; /* takes ownership of value */

        return elem;
}


void *rd_map_get(const rd_map_t *rmap, const void *key) {
        const rd_map_elem_t skel = {.key  = (void *)key,
                                    .hash = rmap->rmap_hash(key)};
        rd_map_elem_t *elem;

        if (!(elem = rd_map_find(rmap, NULL, &skel)))
                return NULL;

        return (void *)elem->value;
}


void rd_map_delete(rd_map_t *rmap, const void *key) {
        const rd_map_elem_t skel = {.key  = (void *)key,
                                    .hash = rmap->rmap_hash(key)};
        rd_map_elem_t *elem;
        int bkt;

        if (!(elem = rd_map_find(rmap, &bkt, &skel)))
                return;

        rd_map_elem_destroy(rmap, elem);
}


void rd_map_copy(rd_map_t *dst,
                 const rd_map_t *src,
                 rd_map_copy_t *key_copy,
                 rd_map_copy_t *value_copy) {
        const rd_map_elem_t *elem;

        RD_MAP_FOREACH_ELEM(elem, src) {
                rd_map_set(
                    dst, key_copy ? key_copy(elem->key) : (void *)elem->key,
                    value_copy ? value_copy(elem->value) : (void *)elem->value);
        }
}


void rd_map_iter_begin(const rd_map_t *rmap, const rd_map_elem_t **elem) {
        *elem = LIST_FIRST(&rmap->rmap_iter);
}

size_t rd_map_cnt(const rd_map_t *rmap) {
        return (size_t)rmap->rmap_cnt;
}

rd_bool_t rd_map_is_empty(const rd_map_t *rmap) {
        return rmap->rmap_cnt == 0;
}


/**
 * @brief Calculates the number of desired buckets and returns
 *        a struct with pre-allocated buckets.
 */
struct rd_map_buckets rd_map_alloc_buckets(size_t expected_cnt) {
        static const int max_depth      = 15;
        static const int bucket_sizes[] = {
            5,     11,    23,     47,     97,   199, /* default */
            409,   823,   1741,   3469,   6949, 14033,
            28411, 57557, 116731, 236897, -1};
        struct rd_map_buckets buckets = RD_ZERO_INIT;
        int i;

        if (!expected_cnt) {
                buckets.cnt = 199;
        } else {
                /* Strive for an average (at expected element count) depth
                 * of 15 elements per bucket, but limit the maximum
                 * bucket count to the maximum value in bucket_sizes above.
                 * When a real need arise we'll change this to a dynamically
                 * growing hash map instead, but this will do for now. */
                buckets.cnt = bucket_sizes[0];
                for (i = 1; bucket_sizes[i] != -1 &&
                            (int)expected_cnt / max_depth > bucket_sizes[i];
                     i++)
                        buckets.cnt = bucket_sizes[i];
        }

        rd_assert(buckets.cnt > 0);

        buckets.p = rd_calloc(buckets.cnt, sizeof(*buckets.p));

        return buckets;
}


void rd_map_init(rd_map_t *rmap,
                 size_t expected_cnt,
                 int (*cmp)(const void *a, const void *b),
                 unsigned int (*hash)(const void *key),
                 void (*destroy_key)(void *key),
                 void (*destroy_value)(void *value)) {

        memset(rmap, 0, sizeof(*rmap));
        rmap->rmap_buckets       = rd_map_alloc_buckets(expected_cnt);
        rmap->rmap_cmp           = cmp;
        rmap->rmap_hash          = hash;
        rmap->rmap_destroy_key   = destroy_key;
        rmap->rmap_destroy_value = destroy_value;
}

void rd_map_clear(rd_map_t *rmap) {
        rd_map_elem_t *elem;

        while ((elem = LIST_FIRST(&rmap->rmap_iter)))
                rd_map_elem_destroy(rmap, elem);
}

void rd_map_destroy(rd_map_t *rmap) {
        rd_map_clear(rmap);
        rd_free(rmap->rmap_buckets.p);
}


int rd_map_str_cmp(const void *a, const void *b) {
        return strcmp((const char *)a, (const char *)b);
}

/**
 * @brief A djb2 string hasher.
 */
unsigned int rd_map_str_hash(const void *key) {
        const char *str = key;
        return rd_string_hash(str, -1);
}



/**
 * @name Unit tests
 *
 */
#include "rdtime.h"
#include "rdunittest.h"
#include "rdcrc32.h"


/**
 * Typed hash maps
 */

/* Complex key type */
struct mykey {
        int k;
        int something_else; /* Ignored by comparator and hasher below */
};

/* Key comparator */
static int mykey_cmp(const void *_a, const void *_b) {
        const struct mykey *a = _a, *b = _b;
        return a->k - b->k;
}

/* Key hasher */
static unsigned int mykey_hash(const void *_key) {
        const struct mykey *key = _key;
        return (unsigned int)key->k;
}

/* Complex value type */
struct person {
        char *name;
        char *surname;
};

/* Define typed hash map type */
typedef RD_MAP_TYPE(const struct mykey *,
                    const struct person *) ut_my_typed_map_t;


/**
 * @brief Test typed hash map with pre-defined type.
 */
static int unittest_typed_map(void) {
        ut_my_typed_map_t rmap =
            RD_MAP_INITIALIZER(0, mykey_cmp, mykey_hash, NULL, NULL);
        ut_my_typed_map_t dup =
            RD_MAP_INITIALIZER(0, mykey_cmp, mykey_hash, NULL, NULL);
        struct mykey k1  = {1};
        struct mykey k2  = {2};
        struct person v1 = {"Roy", "McPhearsome"};
        struct person v2 = {"Hedvig", "Lindahl"};
        const struct mykey *key;
        const struct person *value;

        RD_MAP_SET(&rmap, &k1, &v1);
        RD_MAP_SET(&rmap, &k2, &v2);

        value = RD_MAP_GET(&rmap, &k2);
        RD_UT_ASSERT(value == &v2, "mismatch");

        RD_MAP_FOREACH(key, value, &rmap) {
                RD_UT_SAY("enumerated key %d person %s %s", key->k, value->name,
                          value->surname);
        }

        RD_MAP_COPY(&dup, &rmap, NULL, NULL);

        RD_MAP_DELETE(&rmap, &k1);
        value = RD_MAP_GET(&rmap, &k1);
        RD_UT_ASSERT(value == NULL, "expected no k1");

        value = RD_MAP_GET(&dup, &k1);
        RD_UT_ASSERT(value == &v1, "copied map: k1 mismatch");
        value = RD_MAP_GET(&dup, &k2);
        RD_UT_ASSERT(value == &v2, "copied map: k2 mismatch");

        RD_MAP_DESTROY(&rmap);
        RD_MAP_DESTROY(&dup);

        RD_UT_PASS();
}


static int person_cmp(const void *_a, const void *_b) {
        const struct person *a = _a, *b = _b;
        int r;
        if ((r = strcmp(a->name, b->name)))
                return r;
        return strcmp(a->surname, b->surname);
}
static unsigned int person_hash(const void *_key) {
        const struct person *key = _key;
        return 31 * rd_map_str_hash(key->name) + rd_map_str_hash(key->surname);
}

/**
 * @brief Test typed hash map with locally defined type.
 */
static int unittest_typed_map2(void) {
        RD_MAP_LOCAL_INITIALIZER(usermap, 3, const char *,
                                 const struct person *, rd_map_str_cmp,
                                 rd_map_str_hash, NULL, NULL);
        RD_MAP_LOCAL_INITIALIZER(personmap, 3, const struct person *,
                                 const char *, person_cmp, person_hash, NULL,
                                 NULL);
        struct person p1 = {"Magnus", "Lundstrom"};
        struct person p2 = {"Peppy", "Popperpappies"};
        const char *user;
        const struct person *person;

        /* Populate user -> person map */
        RD_MAP_SET(&usermap, "user1234", &p1);
        RD_MAP_SET(&usermap, "user9999999999", &p2);

        person = RD_MAP_GET(&usermap, "user1234");


        RD_UT_ASSERT(person == &p1, "mismatch");

        RD_MAP_FOREACH(user, person, &usermap) {
                /* Populate reverse name -> user map */
                RD_MAP_SET(&personmap, person, user);
        }

        RD_MAP_FOREACH(person, user, &personmap) {
                /* Just reference the memory to catch memory errors.*/
                RD_UT_ASSERT(strlen(person->name) > 0 &&
                                 strlen(person->surname) > 0 &&
                                 strlen(user) > 0,
                             "bug");
        }

        RD_MAP_DESTROY(&usermap);
        RD_MAP_DESTROY(&personmap);

        return 0;
}


/**
 * @brief Untyped hash map.
 *
 * This is a more thorough test of the underlying hash map implementation.
 */
static int unittest_untyped_map(void) {
        rd_map_t rmap;
        int pass, i, r;
        int cnt     = 100000;
        int exp_cnt = 0, get_cnt = 0, iter_cnt = 0;
        const rd_map_elem_t *elem;
        rd_ts_t ts     = rd_clock();
        rd_ts_t ts_get = 0;

        rd_map_init(&rmap, cnt, rd_map_str_cmp, rd_map_str_hash, rd_free,
                    rd_free);

        /* pass 0 is set,delete,overwrite,get
         * pass 1-5 is get */
        for (pass = 0; pass < 6; pass++) {
                if (pass == 1)
                        ts_get = rd_clock();

                for (i = 1; i < cnt; i++) {
                        char key[10];
                        char val[64];
                        const char *val2;
                        rd_bool_t do_delete = !(i % 13);
                        rd_bool_t overwrite = !do_delete && !(i % 5);

                        rd_snprintf(key, sizeof(key), "key%d", i);
                        rd_snprintf(val, sizeof(val), "VALUE=%d!", i);

                        if (pass == 0) {
                                rd_map_set(&rmap, rd_strdup(key),
                                           rd_strdup(val));

                                if (do_delete)
                                        rd_map_delete(&rmap, key);
                        }

                        if (overwrite) {
                                rd_snprintf(val, sizeof(val), "OVERWRITE=%d!",
                                            i);
                                if (pass == 0)
                                        rd_map_set(&rmap, rd_strdup(key),
                                                   rd_strdup(val));
                        }

                        val2 = rd_map_get(&rmap, key);

                        if (do_delete)
                                RD_UT_ASSERT(!val2,
                                             "map_get pass %d "
                                             "returned value %s "
                                             "for deleted key %s",
                                             pass, val2, key);
                        else
                                RD_UT_ASSERT(val2 && !strcmp(val, val2),
                                             "map_get pass %d: "
                                             "expected value %s, not %s, "
                                             "for key %s",
                                             pass, val, val2 ? val2 : "NULL",
                                             key);

                        if (pass == 0 && !do_delete)
                                exp_cnt++;
                }

                if (pass >= 1)
                        get_cnt += cnt;
        }

        ts_get = rd_clock() - ts_get;
        RD_UT_SAY("%d map_get iterations took %.3fms = %" PRId64 "us/get",
                  get_cnt, (float)ts_get / 1000.0, ts_get / get_cnt);

        RD_MAP_FOREACH_ELEM(elem, &rmap) {
                iter_cnt++;
        }

        r = (int)rd_map_cnt(&rmap);
        RD_UT_ASSERT(r == exp_cnt, "expected %d map entries, not %d", exp_cnt,
                     r);

        RD_UT_ASSERT(r == iter_cnt,
                     "map_cnt() = %d, iteration gave %d elements", r, iter_cnt);

        rd_map_destroy(&rmap);

        ts = rd_clock() - ts;
        RD_UT_SAY("Total time over %d entries took %.3fms", cnt,
                  (float)ts / 1000.0);

        RD_UT_PASS();
}


int unittest_map(void) {
        int fails = 0;
        fails += unittest_untyped_map();
        fails += unittest_typed_map();
        fails += unittest_typed_map2();
        return 0;
}
