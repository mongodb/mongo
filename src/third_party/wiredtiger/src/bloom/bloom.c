/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
/*
 * Less Hashing, Same Performance: Building a Better Bloom Filter
 *	by Adam Kirsch, Michael Mitzenmacher
 *	Random Structures & Algorithms, Volume 33 Issue 2, September 2008
 */

#include "wt_internal.h"

#define WT_BLOOM_TABLE_CONFIG "key_format=r,value_format=1t,exclusive=true"

/*
 * __bloom_init --
 *     Allocate a WT_BLOOM handle.
 */
static int
__bloom_init(WT_SESSION_IMPL *session, const char *uri, const char *config, WT_BLOOM **bloomp)
{
    WT_BLOOM *bloom;
    WT_DECL_RET;
    size_t len;

    *bloomp = NULL;

    WT_RET(__wt_calloc_one(session, &bloom));

    WT_ERR(__wt_strdup(session, uri, &bloom->uri));
    len = strlen(WT_BLOOM_TABLE_CONFIG) + 2;
    if (config != NULL)
        len += strlen(config);
    WT_ERR(__wt_calloc_def(session, len, &bloom->config));
    /* Add the standard config at the end, so it overrides user settings. */
    WT_ERR(__wt_snprintf(
      bloom->config, len, "%s,%s", config == NULL ? "" : config, WT_BLOOM_TABLE_CONFIG));

    bloom->session = session;

    *bloomp = bloom;
    return (0);

err:
    __wt_free(session, bloom->uri);
    __wt_free(session, bloom->config);
    __wt_free(session, bloom->bitstring);
    __wt_free(session, bloom);
    return (ret);
}

/*
 * __bloom_setup --
 *     Populate the bloom structure. Setup is passed in either the count of items expected (n), or
 *     the length of the bitstring (m). Depends on whether the function is called via create or
 *     open.
 */
static int
__bloom_setup(WT_BLOOM *bloom, uint64_t n, uint64_t m, uint32_t factor, uint32_t k)
{
    if (k < 2)
        WT_RET_MSG(bloom->session, EINVAL,
          "bloom filter hash values to be set/tested must be greater than 2");

    bloom->k = k;
    bloom->factor = factor;
    if (n != 0) {
        bloom->n = n;
        bloom->m = bloom->n * bloom->factor;
    } else {
        bloom->m = m;
        bloom->n = bloom->m / bloom->factor;
    }
    return (0);
}

/*
 * __wt_bloom_create --
 *     Creates and configures a WT_BLOOM handle, allocates a bitstring in memory to use while
 *     populating the bloom filter. count - is the expected number of inserted items factor - is the
 *     number of bits to use per inserted item k - is the number of hash values to set or test per
 *     item
 */
int
__wt_bloom_create(WT_SESSION_IMPL *session, const char *uri, const char *config, uint64_t count,
  uint32_t factor, uint32_t k, WT_BLOOM **bloomp) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_BLOOM *bloom;
    WT_DECL_RET;

    WT_RET(__bloom_init(session, uri, config, &bloom));
    WT_ERR(__bloom_setup(bloom, count, 0, factor, k));

    WT_ERR(__bit_alloc(session, bloom->m, &bloom->bitstring));

    *bloomp = bloom;
    return (0);

err:
    WT_TRET(__wt_bloom_close(bloom));
    return (ret);
}

/*
 * __bloom_open_cursor --
 *     Open a cursor to read from a Bloom filter.
 */
static int
__bloom_open_cursor(WT_BLOOM *bloom, WT_CURSOR *owner)
{
    WT_CURSOR *c;
    WT_SESSION_IMPL *session;
    const char *cfg[3];

    if ((c = bloom->c) != NULL)
        return (0);

    session = bloom->session;
    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = bloom->config;
    cfg[2] = NULL;
    c = NULL;
    WT_RET(__wt_open_cursor(session, bloom->uri, owner, cfg, &c));

/*
 * Bump the cache priority for Bloom filters: this makes eviction favor pages from other trees over
 * Bloom filters.
 */
#define WT_EVICT_BLOOM_SKEW WT_THOUSAND
    __wt_evict_priority_set(session, WT_EVICT_BLOOM_SKEW);

    bloom->c = c;
    return (0);
}

/*
 * __wt_bloom_open --
 *     Open a Bloom filter object for use by a single session. The filter must have been created and
 *     finalized.
 */
int
__wt_bloom_open(WT_SESSION_IMPL *session, const char *uri, uint32_t factor, uint32_t k,
  WT_CURSOR *owner, WT_BLOOM **bloomp) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_BLOOM *bloom;
    WT_CURSOR *c;
    WT_DECL_RET;
    uint64_t size;

    WT_RET(__bloom_init(session, uri, NULL, &bloom));
    WT_ERR(__bloom_open_cursor(bloom, owner));
    c = bloom->c;

    /* Find the largest key, to get the size of the filter. */
    WT_ERR(c->prev(c));
    WT_ERR(c->get_key(c, &size));
    WT_ERR(c->reset(c));

    WT_ERR(__bloom_setup(bloom, 0, size, factor, k));

    *bloomp = bloom;
    return (0);

err:
    WT_TRET(__wt_bloom_close(bloom));
    return (ret);
}

/*
 * __wt_bloom_insert --
 *     Adds the given key to the Bloom filter.
 */
void
__wt_bloom_insert(WT_BLOOM *bloom, WT_ITEM *key) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    uint64_t h1, h2;
    uint32_t i;

    h1 = __wt_hash_fnv64(key->data, key->size);
    h2 = __wt_hash_city64(key->data, key->size);
    for (i = 0; i < bloom->k; i++, h1 += h2)
        __bit_set(bloom->bitstring, h1 % bloom->m);
}

/*
 * __wt_bloom_finalize --
 *     Writes the Bloom filter to stable storage. After calling finalize, only read operations can
 *     be performed on the bloom filter.
 */
int
__wt_bloom_finalize(WT_BLOOM *bloom) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_ITEM values;
    WT_SESSION *wt_session;
    uint64_t i;

    wt_session = (WT_SESSION *)bloom->session;
    WT_CLEAR(values);

    /*
     * Create a bit table to store the bloom filter in. TODO: should this call __wt_schema_create
     * directly?
     */
    WT_RET(wt_session->create(wt_session, bloom->uri, bloom->config));
    WT_RET(wt_session->open_cursor(wt_session, bloom->uri, NULL, "bulk=bitmap", &c));

    /* Add the entries from the array into the table. */
    for (i = 0; i < bloom->m; i += values.size) {
        /* Adjust bits to bytes for string offset */
        values.data = bloom->bitstring + (i >> 3);
        /*
         * Shave off some bytes for pure paranoia, in case WiredTiger reserves some special sizes.
         * Choose a value so that if we do multiple inserts, it will be on an byte boundary.
         */
        values.size = (uint32_t)WT_MIN(bloom->m - i, UINT32_MAX - 127);
        c->set_value(c, &values);
        WT_ERR(c->insert(c));
    }

err:
    WT_TRET(c->close(c));
    __wt_free(bloom->session, bloom->bitstring);
    bloom->bitstring = NULL;

    return (ret);
}

/*
 * __wt_bloom_hash --
 *     Calculate the hash values for a given key.
 */
void
__wt_bloom_hash(WT_BLOOM *bloom, WT_ITEM *key, WT_BLOOM_HASH *bhash)
{
    WT_UNUSED(bloom);

    bhash->h1 = __wt_hash_fnv64(key->data, key->size);
    bhash->h2 = __wt_hash_city64(key->data, key->size);
}

/*
 * __wt_bloom_hash_get --
 *     Tests whether the key (as given by its hash signature) is in the Bloom filter. Returns zero
 *     if found, WT_NOTFOUND if not.
 */
int
__wt_bloom_hash_get(WT_BLOOM *bloom, WT_BLOOM_HASH *bhash)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    uint64_t h1, h2;
    uint32_t i;
    uint8_t bit;
    int result;

    /* Get operations are only supported by finalized bloom filters. */
    WT_ASSERT(bloom->session, bloom->bitstring == NULL);

    /* Create a cursor on the first time through. */
    c = NULL;
    WT_ERR(__bloom_open_cursor(bloom, NULL));
    c = bloom->c;

    h1 = bhash->h1;
    h2 = bhash->h2;

    result = 0;
    for (i = 0; i < bloom->k; i++, h1 += h2) {
        /*
         * Add 1 to the hash because WiredTiger tables are 1 based and the original bitstring array
         * was 0 based.
         */
        c->set_key(c, (h1 % bloom->m) + 1);
        WT_ERR(c->search(c));
        WT_ERR(c->get_value(c, &bit));

        if (bit == 0) {
            result = WT_NOTFOUND;
            break;
        }
    }
    WT_ERR(c->reset(c));
    return (result);

err:
    if (c != NULL)
        WT_TRET(c->reset(c));

    /*
     * Error handling from this function is complex. A search in the backing bit field should never
     * return WT_NOTFOUND - so translate that into a different error code and report an error. If we
     * got a WT_ROLLBACK it may be because there is a lot of cache pressure and the transaction is
     * being killed - don't report an error message in that case.
     */
    if (ret == WT_ROLLBACK || ret == WT_CACHE_FULL)
        return (ret);
    WT_RET_MSG(
      bloom->session, ret == WT_NOTFOUND ? WT_ERROR : ret, "Failed lookup in bloom filter");
}

/*
 * __wt_bloom_get --
 *     Tests whether the given key is in the Bloom filter. Returns zero if found, WT_NOTFOUND if
 *     not.
 */
int
__wt_bloom_get(WT_BLOOM *bloom, WT_ITEM *key) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_BLOOM_HASH bhash;

    __wt_bloom_hash(bloom, key, &bhash);
    return (__wt_bloom_hash_get(bloom, &bhash));
}

/*
 * __wt_bloom_inmem_get --
 *     Tests whether the given key is in the Bloom filter. This can be used in place of
 *     __wt_bloom_get for Bloom filters that are memory only.
 */
int
__wt_bloom_inmem_get(WT_BLOOM *bloom, WT_ITEM *key)
{
    uint64_t h1, h2;
    uint32_t i;

    h1 = __wt_hash_fnv64(key->data, key->size);
    h2 = __wt_hash_city64(key->data, key->size);
    for (i = 0; i < bloom->k; i++, h1 += h2) {
        if (!__bit_test(bloom->bitstring, h1 % bloom->m))
            return (WT_NOTFOUND);
    }
    return (0);
}

/*
 * __wt_bloom_intersection --
 *     Modify the Bloom filter to contain the intersection of this filter with another.
 */
int
__wt_bloom_intersection(WT_BLOOM *bloom, WT_BLOOM *other)
{
    uint64_t i, nbytes;

    if (bloom->k != other->k || bloom->factor != other->factor || bloom->m != other->m ||
      bloom->n != other->n)
        WT_RET_MSG(bloom->session, EINVAL,
          "bloom filter intersection configuration mismatch: (%" PRIu32 "/%" PRIu32 ", %" PRIu32
          "/%" PRIu32 ", %" PRIu64 "/%" PRIu64 ", %" PRIu64 "/%" PRIu64 ")",
          bloom->k, other->k, bloom->factor, other->factor, bloom->m, other->m, bloom->n, other->n);

    nbytes = __bitstr_size(bloom->m);
    for (i = 0; i < nbytes; i++)
        bloom->bitstring[i] &= other->bitstring[i];
    return (0);
}

/*
 * __wt_bloom_close --
 *     Close the Bloom filter, release any resources.
 */
int
__wt_bloom_close(WT_BLOOM *bloom) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = bloom->session;

    if (bloom->c != NULL)
        ret = bloom->c->close(bloom->c);
    __wt_free(session, bloom->uri);
    __wt_free(session, bloom->config);
    __wt_free(session, bloom->bitstring);
    __wt_free(session, bloom);

    return (ret);
}

/*
 * __wt_bloom_drop --
 *     Drop a Bloom filter, release any resources.
 */
int
__wt_bloom_drop(WT_BLOOM *bloom, const char *config) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_RET;
    WT_SESSION *wt_session;

    wt_session = (WT_SESSION *)bloom->session;
    if (bloom->c != NULL) {
        ret = bloom->c->close(bloom->c);
        bloom->c = NULL;
    }
    WT_TRET(wt_session->drop(wt_session, bloom->uri, config));
    WT_TRET(__wt_bloom_close(bloom));

    return (ret);
}
