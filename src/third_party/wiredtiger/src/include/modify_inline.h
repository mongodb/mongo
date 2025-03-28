/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_MODIFY_FOREACH_BEGIN(mod, p, nentries, napplied)                                       \
    do {                                                                                          \
        const uint8_t *__p = p;                                                                   \
        const uint8_t *__data = (const uint8_t *)(__p + (size_t)(nentries) * sizeof(size_t) * 3); \
        size_t __i;                                                                               \
        for (__i = 0; __i < (nentries); ++__i) {                                                  \
            memcpy(&(mod).data.size, __p, sizeof(size_t));                                        \
            __p += sizeof(size_t);                                                                \
            memcpy(&(mod).offset, __p, sizeof(size_t));                                           \
            __p += sizeof(size_t);                                                                \
            memcpy(&(mod).size, __p, sizeof(size_t));                                             \
            __p += sizeof(size_t);                                                                \
            (mod).data.data = __data;                                                             \
            __data += (mod).data.size;                                                            \
            if ((int)__i < (int)napplied)                                                         \
                continue;

#define WT_MODIFY_FOREACH_REVERSE(mod, p, nentries, napplied, datasz)       \
    do {                                                                    \
        const uint8_t *__p = (p) + (size_t)(nentries) * sizeof(size_t) * 3; \
        const uint8_t *__data = (const uint8_t *)__p + datasz;              \
        size_t __i;                                                         \
        for (__i = (napplied); __i < (nentries); ++__i) {                   \
            __p -= sizeof(size_t);                                          \
            memcpy(&(mod).size, __p, sizeof(size_t));                       \
            __p -= sizeof(size_t);                                          \
            memcpy(&(mod).offset, __p, sizeof(size_t));                     \
            __p -= sizeof(size_t);                                          \
            memcpy(&(mod).data.size, __p, sizeof(size_t));                  \
            (mod).data.data = (__data -= (mod).data.size);

#define WT_MODIFY_FOREACH_END \
    }                         \
    }                         \
    while (0)

/*
 * __wt_modify_max_memsize --
 *     Calculate the maximum memory usage when applying a packed modify.
 */
static WT_INLINE void
__wt_modify_max_memsize(const void *modify, size_t base_value_size, size_t *max_memsize)
{
    WT_MODIFY mod;
    size_t nentries;
    const uint8_t *p;
    *max_memsize = base_value_size;

    /* Get the number of modify entries. */
    p = (const uint8_t *)modify;
    memcpy(&nentries, p, sizeof(nentries));
    p += sizeof(nentries);

    WT_MODIFY_FOREACH_BEGIN (mod, p, nentries, 0) {
        *max_memsize = WT_MAX(*max_memsize, mod.offset) + mod.data.size;
    }
    WT_MODIFY_FOREACH_END;
}

/*
 * __wt_modify_max_memsize_format --
 *     Calculate the maximum memory usage when applying a packed modify. This function also
 *     considers the memory usage of the string terminator.
 */
static WT_INLINE void
__wt_modify_max_memsize_format(
  const void *modify, const char *value_format, size_t base_value_size, size_t *max_memsize)
{
    __wt_modify_max_memsize(modify, base_value_size, max_memsize);

    if (value_format[0] == 'S')
        ++(*max_memsize);
}

/*
 * __wt_modify_max_memsize_unpacked --
 *     Calculate the maximum memory usage when applying an unpacked modify. This function also
 *     considers the memory usage of the string terminator.
 */
static WT_INLINE void
__wt_modify_max_memsize_unpacked(WT_MODIFY *entries, int nentries, const char *value_format,
  size_t base_value_size, size_t *max_memsize)
{
    int i;

    *max_memsize = base_value_size;

    for (i = 0; i < nentries; ++i)
        *max_memsize = WT_MAX(*max_memsize, entries[i].offset) + entries[i].data.size;

    if (value_format[0] == 'S')
        ++(*max_memsize);
}

/*
 * __wt_modifies_max_memsize --
 *     Calculate the maximum memory usage when applying a series of modifies. This function also
 *     considers the memory usage of the string terminator.
 */
static WT_INLINE void
__wt_modifies_max_memsize(
  WT_UPDATE_VECTOR *modifies, const char *value_format, size_t base_value_size, size_t *max_memsize)
{
    WT_UPDATE *upd;
    int i;

    *max_memsize = base_value_size;

    for (i = (int)modifies->size - 1; i >= 0; --i) {
        upd = modifies->listp[i];
        __wt_modify_max_memsize(upd->data, *max_memsize, max_memsize);
    }

    if (value_format[0] == 'S')
        ++(*max_memsize);
}
