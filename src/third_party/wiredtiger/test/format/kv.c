/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

/*
 * key_init_random --
 *     Fill in random key lengths.
 */
static void
key_init_random(TABLE *table)
{
    size_t i;
    uint32_t max;

    /*
     * Fill in random key lengths. Focus on relatively small items, admitting the possibility of
     * larger items. Pick a size close to the minimum most of the time, only create a larger item 1
     * in 20 times.
     */
    for (i = 0; i < WT_ELEMENTS(table->key_rand_len); ++i) {
        max = TV(BTREE_KEY_MAX);
        if (i % 20 != 0 && max > TV(BTREE_KEY_MIN) + 20)
            max = TV(BTREE_KEY_MIN) + 20;
        table->key_rand_len[i] = mmrand(&g.data_rnd, TV(BTREE_KEY_MIN), max);
    }
}

/*
 * key_init --
 *     Initialize the keys for a run.
 */
void
key_init(TABLE *table, void *arg)
{
    FILE *fp;
    u_int i;
    char buf[MAX_FORMAT_PATH];

    (void)arg; /* unused argument */
    testutil_assert(table != NULL);

    /* Key initialization is only required by row-store objects. */
    if (table->type != ROW)
        return;

    /* Backward compatibility, built the correct path to the saved key-length file. */
    if (ntables == 0)
        testutil_snprintf(buf, sizeof(buf), "%s", g.home_key);
    else
        testutil_snprintf(buf, sizeof(buf), "%s.%u", g.home_key, table->id);

    /*
     * The key is a variable length item with a leading 10-digit value. Since we have to be able
     * re-construct it from the record number (when doing row lookups), we pre-load a set of random
     * lengths in a lookup table, and then use the record number to choose one of the pre-loaded
     * lengths.
     *
     * Read in the values during reopen.
     */
    if (g.reopen) {
        if ((fp = fopen(buf, "r")) == NULL)
            testutil_die(errno, "%s", buf);
        for (i = 0; i < WT_ELEMENTS(table->key_rand_len); ++i) {
            testutil_assert_errno(fgets(buf, sizeof(buf), fp) != NULL);
            table->key_rand_len[i] = atou32(__func__, buf, '\n');
        }
        fclose_and_clear(&fp);
        return;
    }

    key_init_random(table);

    /* Write out the values for a subsequent reopen. */
    if ((fp = fopen(buf, "w")) == NULL)
        testutil_die(errno, "%s", buf);
    for (i = 0; i < WT_ELEMENTS(table->key_rand_len); ++i)
        fprintf(fp, "%" PRIu32 "\n", table->key_rand_len[i]);
    fclose_and_clear(&fp);
}

/*
 * key_gen_init --
 *     Initialize the key structures for a run.
 */
void
key_gen_init(WT_ITEM *key)
{
    size_t i, len;
    char *p;

    len = WT_MAX(KILOBYTE(100), table_maxv(V_TABLE_BTREE_KEY_MAX) + g.prefix_len_max + 10);
    p = dmalloc(len);
    for (i = 0; i < len; ++i)
        p[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];

    key->mem = p;
    key->memsize = len;
    key->data = key->mem;
    key->size = 0;
}

/*
 * key_gen_teardown --
 *     Tear down the key structures.
 */
void
key_gen_teardown(WT_ITEM *key)
{
    free(key->mem);
    memset(key, 0, sizeof(*key));
}

#define COMMON_PREFIX_CHAR 'C'

/*
 * key_gen_common --
 *     Row-store key generation code shared between normal and insert key generation.
 */
void
key_gen_common(TABLE *table, WT_ITEM *key, uint64_t keyno, const char *const suffix)
{
    size_t i;
    uint64_t n;
    uint32_t prefix_len;
    char *p;
    const char *bucket;

    testutil_assert(table->type == ROW);

    /*
     * The workload we're trying to mimic with a prefix is a long common prefix followed by a record
     * number, the tricks are creating a prefix that won't re-order keys, and to change the prefix
     * with some regularity to test prefix boundaries. Split the key space into power-of-2 buckets:
     * that results in tiny runs of prefix strings at the beginning of the tree, and increasingly
     * large common prefixes as the tree grows (with a testing sweet spot in the middle). After the
     * bucket value, append a string of common bytes. The standard, zero-padded key itself sorts
     * lexicographically, meaning the common key prefix will grow and shrink by a few bytes as the
     * number increments, which is a good thing for testing.
     */
    p = key->mem;
    prefix_len = TV(BTREE_PREFIX_LEN);
    if (g.prefix_len_max != 0) {
        /*
         * Not all tables have prefixes and prefixes may be of different lengths. If any table has a
         * prefix, check if we need to reset the leading bytes in the key to their original values.
         * It's an ugly test, but it avoids rewriting the key in a performance path. The variable is
         * the largest prefix in the run, and the hard-coded 20 gets us past the key appended to
         * that prefix.
         */
        if (p[1] == COMMON_PREFIX_CHAR) {
            for (i = 0; i < g.prefix_len_max + 20; ++i)
                p[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
            p = key->mem;
        }
        if (prefix_len != 0) {
            bucket = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
            for (n = keyno; n > 0; n >>= 1) {
                if (*bucket == 'z')
                    break;
                ++bucket;
            }
            p[0] = *bucket;
            memset(p + 1, COMMON_PREFIX_CHAR, prefix_len - 1);
            p += prefix_len;
        }
    }

    /*
     * After any common prefix, the key starts with a 10-digit string (the specified row) followed
     * by two digits (a random number between 1 and 15 if it's an insert, otherwise 00).
     */
    u64_to_string_zf(keyno, p, 11);
    p[10] = '.';
    p[11] = suffix[0];
    p[12] = suffix[1];
    p[13] = '/';

    /*
     * Because we're doing table lookup for key sizes, we can't set overflow key sizes in the table,
     * the table isn't big enough to keep our hash from selecting too many big keys and blowing out
     * the cache. Handle that here, use a really big key 1 in 2500 times.
     */
    key->data = key->mem;
    key->size = prefix_len;
    key->size += keyno % 2500 == 0 && TV(BTREE_KEY_MAX) < KILOBYTE(80) ?
      KILOBYTE(80) :
      table->key_rand_len[keyno % WT_ELEMENTS(table->key_rand_len)];
    testutil_assert(key->size <= key->memsize);
}

/*
 * val_len --
 *     Select and return the length for a value.
 */
static inline uint32_t
val_len(WT_RAND_STATE *rnd, uint64_t keyno, uint32_t min, uint32_t max)
{
    /*
     * Focus on relatively small items, admitting the possibility of larger items. Pick a size close
     * to the minimum most of the time, only create a larger item 1 in 20 times, and a really big
     * item 1 in somewhere around 2500 items.
     */
    if (keyno % 2500 == 0 && max < KILOBYTE(80)) {
        min = KILOBYTE(80);
        max = KILOBYTE(100);
    } else if (keyno % 20 != 0 && max > min + 20)
        max = min + 20;
    return (mmrand(rnd, min, max));
}

/*
 * val_init --
 *     Initialize the value structures for a table.
 */
void
val_init(TABLE *table, void *arg)
{
    WT_RAND_STATE *rnd;
    size_t i;
    uint32_t len;

    (void)arg; /* unused argument */
    testutil_assert(table != NULL);

    /* Discard any previous value initialization. */
    free(table->val_base);
    table->val_base = NULL;
    table->val_dup_data_len = 0;

    /*
     * Set initial buffer contents to recognizable text.
     *
     * Add a few extra bytes in order to guarantee we can always offset into the buffer by a few
     * extra bytes, used to generate different data for column-store run-length encoded files.
     */
    len = WT_MAX(KILOBYTE(100), table_maxv(V_TABLE_BTREE_VALUE_MAX)) + 20;
    table->val_base = dmalloc(len);
    for (i = 0; i < len; ++i)
        table->val_base[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26];

    rnd = &g.data_rnd;
    table->val_dup_data_len =
      val_len(rnd, (uint64_t)mmrand(rnd, 1, 20), TV(BTREE_VALUE_MIN), TV(BTREE_VALUE_MAX));
}

/*
 * val_gen_init --
 *     Initialize a single value structure.
 */
void
val_gen_init(WT_ITEM *value)
{
    uint32_t len;

    len = WT_MAX(KILOBYTE(100), table_maxv(V_TABLE_BTREE_VALUE_MAX)) + 20;
    value->mem = dmalloc(len);
    value->memsize = len;
    value->data = value->mem;
    value->size = 0;
}

/*
 * val_gen_teardown --
 *     Discard a single value structure.
 */
void
val_gen_teardown(WT_ITEM *value)
{
    free(value->mem);
    memset(value, 0, sizeof(*value));
}

/*
 * val_to_flcs --
 *     Take a RS or VLCS value, and choose an FLCS value in a reproducible way.
 */
void
val_to_flcs(TABLE *table, WT_ITEM *value, uint8_t *bitvp)
{
    uint32_t i, max_check;
    uint8_t bitv;
    const char *p;

    /* Use the first random byte of the key being cautious around the length of the value. */
    bitv = FIX_MIRROR_DNE;
    max_check = (uint32_t)WT_MIN(PREFIX_LEN_CONFIG_MIN + 10, value->size);
    for (p = value->data, i = 0; i < max_check; ++p, ++i)
        if (p[0] == '/' && i < max_check - 1) {
            bitv = (uint8_t)p[1];
            break;
        }

    switch (TV(BTREE_BITCNT)) {
    case 8:
        break;
    case 7:
        bitv &= 0x7f;
        break;
    case 6:
        bitv &= 0x3f;
        break;
    case 5:
        bitv &= 0x1f;
        break;
    case 4:
        bitv &= 0x0f;
        break;
    case 3:
        bitv &= 0x07;
        break;
    case 2:
        bitv &= 0x03;
        break;
    case 1:
        bitv &= 0x01;
        break;
    }
    *bitvp = bitv;
}

/*
 * val_gen --
 *     Generate a new value.
 */
void
val_gen(TABLE *table, WT_RAND_STATE *rnd, WT_ITEM *value, uint8_t *bitvp, uint64_t keyno)
{
    char *p;

    value->data = NULL;
    value->size = 0;
    *bitvp = FIX_VALUE_WRONG;

    if (table->type == FIX) {
        /*
         * FLCS remove is the same as storing a zero value, so where there are more than a couple of
         * bits to work with, stay away from 0 values.
         */
        switch (TV(BTREE_BITCNT)) {
        case 8:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0xff);
            break;
        case 7:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0x7f);
            break;
        case 6:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0x3f);
            break;
        case 5:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0x1f);
            break;
        case 4:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0x0f);
            break;
        case 3:
            *bitvp = (u_int8_t)mmrand(rnd, 1, 0x07);
            break;
        case 2:
            *bitvp = (u_int8_t)mmrand(rnd, 0, 0x03);
            break;
        case 1:
            *bitvp = (u_int8_t)mmrand(rnd, 0, 1);
            break;
        }
        return;
    }

    /*
     * WiredTiger doesn't store zero-length data items in row-store files, test that by inserting a
     * zero-length data item every so often.
     */
    if (keyno % 63 == 0) {
        *(uint8_t *)value->mem = 0x0;
        value->size = 0;
        return;
    }

    /*
     * Data items have unique leading numbers by default and random lengths; variable-length
     * column-stores use a duplicate data value to test RLE.
     */
    p = value->mem;
    value->data = value->mem;
    if (table->type == VAR && mmrand(rnd, 1, 100) < TV(BTREE_REPEAT_DATA_PCT)) {
        value->size = table->val_dup_data_len;
        memcpy(p, table->val_base, value->size);
        (void)strcpy(p, "DUPLICATEV");
        p[10] = '/';
    } else {
        value->size = val_len(rnd, keyno, TV(BTREE_VALUE_MIN), TV(BTREE_VALUE_MAX));
        memcpy(p, table->val_base, value->size);
        u64_to_string_zf(keyno, p, 11);
        p[10] = '/';

        /* Randomize the first character, we use it for FLCS values. */
        p[11] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"[mmrand(rnd, 0, 51)];
    }
}
