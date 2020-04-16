/*-
 * Public Domain 2014-2020 MongoDB, Inc.
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
 * key_init --
 *     Initialize the keys for a run.
 */
void
key_init(void)
{
    FILE *fp;
    size_t i;
    uint32_t max;

    /*
     * The key is a variable length item with a leading 10-digit value. Since we have to be able
     * re-construct it from the record number (when doing row lookups), we pre-load a set of random
     * lengths in a lookup table, and then use the record number to choose one of the pre-loaded
     * lengths.
     *
     * Read in the values during reopen.
     */
    if (g.reopen) {
        if ((fp = fopen(g.home_key, "r")) == NULL)
            testutil_die(errno, "%s", g.home_key);
        for (i = 0; i < WT_ELEMENTS(g.key_rand_len); ++i)
            fp_readv(fp, g.home_key, false, &g.key_rand_len[i]);
        fclose_and_clear(&fp);
        return;
    }

    /*
     * Fill in the random key lengths.
     *
     * Focus on relatively small items, admitting the possibility of larger items. Pick a size close
     * to the minimum most of the time, only create a larger item 1 in 20 times.
     */
    for (i = 0; i < WT_ELEMENTS(g.key_rand_len); ++i) {
        max = g.c_key_max;
        if (i % 20 != 0 && max > g.c_key_min + 20)
            max = g.c_key_min + 20;
        g.key_rand_len[i] = mmrand(NULL, g.c_key_min, max);
    }

    /* Write out the values for a subsequent reopen. */
    if ((fp = fopen(g.home_key, "w")) == NULL)
        testutil_die(errno, "%s", g.home_key);
    for (i = 0; i < WT_ELEMENTS(g.key_rand_len); ++i)
        fprintf(fp, "%" PRIu32 "\n", g.key_rand_len[i]);
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

    len = WT_MAX(KILOBYTE(100), g.c_key_max);
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

/*
 * key_gen_common --
 *     Key generation code shared between normal and insert key generation.
 */
void
key_gen_common(WT_ITEM *key, uint64_t keyno, const char *const suffix)
{
    int len;
    char *p;

    p = key->mem;

    /*
     * The key always starts with a 10-digit string (the specified row) followed by two digits, a
     * random number between 1 and 15 if it's an insert, otherwise 00.
     */
    u64_to_string_zf(keyno, key->mem, 11);
    p[10] = '.';
    p[11] = suffix[0];
    p[12] = suffix[1];
    len = 13;

    /*
     * In a column-store, the key isn't used, it doesn't need a random length.
     */
    if (g.type == ROW) {
        p[len] = '/';

        /*
         * Because we're doing table lookup for key sizes, we weren't able to set really big keys
         * sizes in the table, the table isn't big enough to keep our hash from selecting too many
         * big keys and blowing out the cache. Handle that here, use a really big key 1 in 2500
         * times.
         */
        len = keyno % 2500 == 0 && g.c_key_max < KILOBYTE(80) ?
          KILOBYTE(80) :
          (int)g.key_rand_len[keyno % WT_ELEMENTS(g.key_rand_len)];
    }

    key->data = key->mem;
    key->size = (size_t)len;
}

static char *val_base;            /* Base/original value */
static uint32_t val_dup_data_len; /* Length of duplicate data items */
static uint32_t val_len;          /* Length of data items */

static inline uint32_t
value_len(WT_RAND_STATE *rnd, uint64_t keyno, uint32_t min, uint32_t max)
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

void
val_init(void)
{
    size_t i;

    /* Discard any previous value initialization. */
    free(val_base);
    val_base = NULL;
    val_dup_data_len = val_len = 0;

    /*
     * Set initial buffer contents to recognizable text.
     *
     * Add a few extra bytes in order to guarantee we can always offset into the buffer by a few
     * extra bytes, used to generate different data for column-store run-length encoded files.
     */
    val_len = WT_MAX(KILOBYTE(100), g.c_value_max) + 20;
    val_base = dmalloc(val_len);
    for (i = 0; i < val_len; ++i)
        val_base[i] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"[i % 26];

    val_dup_data_len = value_len(NULL, (uint64_t)mmrand(NULL, 1, 20), g.c_value_min, g.c_value_max);
}

void
val_gen_init(WT_ITEM *value)
{
    value->mem = dmalloc(val_len);
    value->memsize = val_len;
    value->data = value->mem;
    value->size = 0;
}

void
val_gen_teardown(WT_ITEM *value)
{
    free(value->mem);
    memset(value, 0, sizeof(*value));
}

void
val_gen(WT_RAND_STATE *rnd, WT_ITEM *value, uint64_t keyno)
{
    char *p;

    p = value->mem;
    value->data = value->mem;

    /*
     * Fixed-length records: take the low N bits from the last digit of the record number.
     */
    if (g.type == FIX) {
        switch (g.c_bitcnt) {
        case 8:
            p[0] = (char)mmrand(rnd, 1, 0xff);
            break;
        case 7:
            p[0] = (char)mmrand(rnd, 1, 0x7f);
            break;
        case 6:
            p[0] = (char)mmrand(rnd, 1, 0x3f);
            break;
        case 5:
            p[0] = (char)mmrand(rnd, 1, 0x1f);
            break;
        case 4:
            p[0] = (char)mmrand(rnd, 1, 0x0f);
            break;
        case 3:
            p[0] = (char)mmrand(rnd, 1, 0x07);
            break;
        case 2:
            p[0] = (char)mmrand(rnd, 1, 0x03);
            break;
        case 1:
            p[0] = 1;
            break;
        }
        value->size = 1;
        return;
    }

    /*
     * WiredTiger doesn't store zero-length data items in row-store files, test that by inserting a
     * zero-length data item every so often.
     */
    if (keyno % 63 == 0) {
        p[0] = '\0';
        value->size = 0;
        return;
    }

    /*
     * Data items have unique leading numbers by default and random lengths; variable-length
     * column-stores use a duplicate data value to test RLE.
     */
    if (g.type == VAR && mmrand(rnd, 1, 100) < g.c_repeat_data_pct) {
        value->size = val_dup_data_len;
        memcpy(p, val_base, value->size);
        (void)strcpy(p, "DUPLICATEV");
        p[10] = '/';
    } else {
        value->size = value_len(rnd, keyno, g.c_value_min, g.c_value_max);
        memcpy(p, val_base, value->size);
        u64_to_string_zf(keyno, p, 11);
        p[10] = '/';
    }
}
