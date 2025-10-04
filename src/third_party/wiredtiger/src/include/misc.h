/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * When compiling for code coverage measurement it is necessary to ensure that inline functions in
 * header files that are #included in multiple source files are not inlined.
 *
 * Otherwise, it is possible that there will be multiple copies of the function across the linked
 * executable with the result that the code coverage counts for branch coverage (both in terms of
 * branches in the code and the number of branches executed) will be too high and incorrect.
 *
 * In non-code coverage builds, preventing inlining would impact performance and so this must only
 * take place when performing code coverage.
 */
#ifdef CODE_COVERAGE_MEASUREMENT
#ifdef _WIN32
#error "Code coverage measurement is not currently supported for WiredTiger on Windows."
#endif /* _WIN32 */
#define WT_INLINE __attribute__((noinline))
#else
#define WT_INLINE inline
#endif /* CODE_COVERAGE_MEASUREMENT */

/*
 * Explicitly suppress compiler warnings about unused variables, and function parameters.
 */
#define WT_UNUSED(var) (void)(var)
#define WT_NOT_READ(v, val) \
    do {                    \
        (v) = (val);        \
        (void)(v);          \
    } while (0);

/*
 * Explicitly suppress: warning unused result.
 *
 * Simply casting to void as in WT_UNUSED will not suppress this warning on the current version of
 * gcc (11.3.0) used for the server build.
 *
 * This workaround works with every supported toolchain, and does not employ unused temporary values
 * that are then detected by Coverity.
 */
#define WT_IGNORE_RET(call) ((void)!(call))

#define WT_DIVIDER "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

/* Basic constants. */
#define WT_MILLION_LITERAL 1000000
#define WT_HUNDRED (100)
#define WT_THOUSAND (1000)
#define WT_MILLION (WT_MILLION_LITERAL)
#define WT_BILLION (1000000000)

#define WT_DAY (86400)
#define WT_MINUTE (60)

#define WT_PROGRESS_MSG_PERIOD (20) /* Seconds. */

#define WT_KILOBYTE (1024)
#define WT_MEGABYTE (1048576)
#define WT_GIGABYTE (1073741824)
#define WT_TERABYTE ((uint64_t)1099511627776)
#define WT_PETABYTE ((uint64_t)1125899906842624)
#define WT_EXABYTE ((uint64_t)1152921504606846976)

/* Strings used for indicating failed string buffer construction. */
#define WT_ERR_STRING "[Error]"
#define WT_NO_ADDR_STRING "[NoAddr]"

/* Maximum length of an encoded JSON character. */
#define WT_MAX_JSON_ENCODE 6

/*
 * Sizes that cannot be larger than 2**32 are stored in uint32_t fields in common structures to save
 * space. To minimize conversions from size_t to uint32_t through the code, we use the following
 * macros.
 */
#define WT_STORE_SIZE(s) ((uint32_t)(s))
#define WT_PTRDIFF(end, begin) ((size_t)((const uint8_t *)(end) - (const uint8_t *)(begin)))
#define WT_PTRDIFF32(end, begin) WT_STORE_SIZE(WT_PTRDIFF((end), (begin)))
#define WT_BLOCK_FITS(p, len, begin, maxlen)             \
    ((const uint8_t *)(p) >= (const uint8_t *)(begin) && \
      ((const uint8_t *)(p) + (len) <= (const uint8_t *)(begin) + (maxlen)))

/*
 * Align an unsigned value of any type to a specified power-of-2, including the offset result of a
 * pointer subtraction; do the calculation using the largest unsigned integer type available.
 */
#define WT_ALIGN(n, v) ((((uintmax_t)(n)) + ((v)-1)) & ~(((uintmax_t)(v)) - 1))

#define WT_ALIGN_NEAREST(n, v) ((((uintmax_t)(n)) + ((v) / 2)) & ~(((uintmax_t)(v)) - 1))

/* Min, max. */
#define WT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define WT_MAX(a, b) ((a) < (b) ? (b) : (a))
#define WT_CLAMP(x, low, high) (WT_MIN(WT_MAX((x), (low)), (high)))

/* Check and reset, implicitly reset to 0. */
#define WT_CHECK_AND_RESET(a, v) ((a) == (v) ? ((a) = 0, true) : false)

/* Ceil for unsigned/positive real numbers. */
#define WT_CEIL_POS(a) ((a) - (double)(uintmax_t)(a) > 0.0 ? (uintmax_t)(a) + 1 : (uintmax_t)(a))

/* Elements in an array. */
#define WT_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))

/* 10 level skip lists, 1/4 have a link to the next element. */
#define WT_SKIP_MAXDEPTH 10
#define WT_SKIP_PROBABILITY (UINT32_MAX >> 2)

/*
 * Encryption needs to know its original length before either the block or logging subsystems pad.
 * Constant value.
 */
#define WT_ENCRYPT_LEN_SIZE sizeof(uint32_t)

/*
 * WT-specific return codes for __wt_getopt(); use __wt_optwt to enable.
 */
#define WT_GETOPT_BAD_ARGUMENT 1
#define WT_GETOPT_BAD_OPTION 2

/*
 * __wt_calloc_def, __wt_calloc_one --
 *     Most calloc calls don't need separate count or sizeof arguments.
 */
#define __wt_calloc_def(session, number, addr) \
    __wt_calloc(session, (size_t)(number), sizeof(**(addr)), addr)
#define __wt_calloc_one(session, addr) __wt_calloc(session, (size_t)1, sizeof(**(addr)), addr)

/*
 * __wt_realloc_def --
 *     Common case allocate-and-grow function. Starts by allocating the requested number of items
 *     (at least 10), then doubles each time the list needs to grow.
 */
#define __wt_realloc_def(session, sizep, number, addr)                          \
    (((number) * sizeof(**(addr)) <= *(sizep)) ?                                \
        0 :                                                                     \
        __wt_realloc(session, sizep,                                            \
          (FLD_ISSET(S2C(session)->debug_flags, WT_CONN_DEBUG_REALLOC_EXACT)) ? \
            (number) * sizeof(**(addr)) :                                       \
            WT_MAX(*(sizep)*2, WT_MAX(10, (number)) * sizeof(**(addr))),        \
          addr))

/*
 * Our internal free function clears the underlying address so there is a smaller chance of racing
 * threads seeing intermediate results while a structure is being free'd. (That would be a bug, of
 * course, but I'd rather not drop core, just the same.) That's a non-standard "free" API, and the
 * resulting bug is non-trivial to find -- make sure we get it right, don't make the caller remember
 * to put the & operator on the pointer.
 */
#define __wt_free(session, p)            \
    do {                                 \
        void *__p = &(p);                \
        if (*(void **)__p != NULL)       \
            __wt_free_int(session, __p); \
    } while (0)

/* Overwrite whether or not this is a diagnostic build. */
#define __wt_explicit_overwrite(p, size) memset(p, WT_DEBUG_BYTE, size)
#ifdef HAVE_DIAGNOSTIC
#define __wt_overwrite_and_free(session, p)           \
    do {                                              \
        void *__p = &(p);                             \
        if (*(void **)__p != NULL) {                  \
            __wt_explicit_overwrite(p, sizeof(*(p))); \
            __wt_free_int(session, __p);              \
        }                                             \
    } while (0)
#define __wt_overwrite_and_free_len(session, p, len) \
    do {                                             \
        void *__p = &(p);                            \
        if (*(void **)__p != NULL) {                 \
            __wt_explicit_overwrite(p, len);         \
            __wt_free_int(session, __p);             \
        }                                            \
    } while (0)
#else
#define __wt_overwrite_and_free(session, p) __wt_free(session, p)
#define __wt_overwrite_and_free_len(session, p, len) __wt_free(session, p)
#endif

/*
 * Flag set, clear and test.
 *
 * They come in 3 flavors: F_XXX (handles a field named "flags" in the structure referenced by its
 * argument), LF_XXX (handles a local variable named "flags"), and FLD_XXX (handles any variable,
 * anywhere).
 *
 * Flags can be different unsigned bit values -- we cast to keep the compiler quiet (the hex
 * constant might be a negative integer), and to ensure the hex constant is the correct size before
 * applying the bitwise not operator.
 *
 * Summary of flag tests:
 *
 * Is any flag set? - F*_ISSET()
 *
 * Are none of the flags set? - !F*_ISSET()
 *
 * Are all of the flags set? - F*_AREALLSET()
 */
#ifdef TSAN_BUILD
/*
 * FIXME-WT-12534 We need atomics to fix data races detected by TSan, however these atomics come
 * with a large performance cost. Define these atomics only for TSan builds as they aren't
 * performance critical and we'll investigate a long term solution separately.
 */
#define FLD_CLR(field, mask) (void)__wt_atomic_and_generic(&field, (__typeof__(field))(~(mask)))
#define FLD_MASK(field, mask) (__wt_atomic_load_generic(&field) & (mask))
#define FLD_ISSET(field, mask) (FLD_MASK(field, (mask)) != 0)
#define FLD_SET(field, mask) ((void)__wt_atomic_or_generic(&field, (mask)))
#else
#define FLD_CLR(field, mask) ((void)((field) &= ~(mask)))
#define FLD_MASK(field, mask) ((field) & (mask))
#define FLD_ISSET(field, mask) (FLD_MASK(field, mask) != 0)
#define FLD_SET(field, mask) ((void)((field) |= (mask)))
#endif
/* Named like a macro for consistency. An inline function to evaluate mask only once. */
static inline bool
FLD_AREALLSET(uint64_t field, uint64_t mask)
{
    return (FLD_MASK(field, mask) == mask);
}

#define F_CLR(p, mask) FLD_CLR((p)->flags, mask)
#define F_ISSET(p, mask) FLD_ISSET((p)->flags, mask)
#define F_MASK(p, mask) FLD_MASK((p)->flags, mask)
#define F_SET(p, mask) FLD_SET((p)->flags, mask)
#define F_AREALLSET(p, mask) FLD_AREALLSET((uint64_t)((p)->flags), (uint64_t)(mask))

#define LF_CLR(mask) FLD_CLR(flags, mask)
#define LF_ISSET(mask) FLD_ISSET(flags, mask)
#define LF_MASK(mask) FLD_MASK(flags, mask)
#define LF_SET(mask) FLD_SET(flags, mask)
#define LF_AREALLSET(mask) FLD_AREALLSET(flags, mask)

/*
 * Insertion sort, for sorting small sets of values.
 *
 * The "compare_lt" argument is a function or macro that returns true when its first argument is
 * less than its second argument.
 */
#define WT_INSERTION_SORT(arrayp, n, value_type, compare_lt)                           \
    do {                                                                               \
        value_type __v;                                                                \
        int __i, __j, __n = (int)(n);                                                  \
        if (__n == 2) {                                                                \
            __v = (arrayp)[1];                                                         \
            if (compare_lt(__v, (arrayp)[0])) {                                        \
                (arrayp)[1] = (arrayp)[0];                                             \
                (arrayp)[0] = __v;                                                     \
            }                                                                          \
        }                                                                              \
        if (__n > 2) {                                                                 \
            for (__i = 1; __i < __n; ++__i) {                                          \
                __v = (arrayp)[__i];                                                   \
                for (__j = __i - 1; __j >= 0 && compare_lt(__v, (arrayp)[__j]); --__j) \
                    (arrayp)[__j + 1] = (arrayp)[__j];                                 \
                (arrayp)[__j + 1] = __v;                                               \
            }                                                                          \
        }                                                                              \
    } while (0)

/*
 * Some C compiler address sanitizers complain if qsort is passed a NULL base reference, even if
 * there are no elements to compare (note zero elements is allowed by the IEEE Std 1003.1-2017
 * standard). Avoid the complaint.
 */
#define __wt_qsort(base, nmemb, size, compar) \
    if ((nmemb) != 0)                         \
    qsort(base, nmemb, size, compar)

/*
 * Merge two sorted arrays into a single sorted array. With `prefer_latest` true, if duplicate keys
 * are found, the element from the later array (arr2) is preferred.
 */
#define WT_MERGE_SORT(                                                                        \
  session, arr1, arr1_size, arr2, arr2_size, cmp, prefer_latest, merged_arr, merged_arr_size) \
    do {                                                                                      \
        uint32_t __i, __j, __k;                                                               \
        int __compar;                                                                         \
        __i = __j = __k = 0;                                                                  \
        while (__i < (arr1_size) && __j < (arr2_size)) {                                      \
            __compar = (cmp)((session), (arr1)[__i], (arr2)[__j]);                            \
            if (__compar < 0)                                                                 \
                (merged_arr)[__k++] = (arr1)[__i++];                                          \
            else {                                                                            \
                (merged_arr)[__k++] = (arr2)[__j++];                                          \
                if ((prefer_latest) && __compar == 0)                                         \
                    __i++; /* Skip corresponding element from arr1 */                         \
            }                                                                                 \
        }                                                                                     \
        while (__j < (arr2_size))                                                             \
            (merged_arr)[__k++] = (arr2)[__j++];                                              \
        while (__i < (arr1_size))                                                             \
            (merged_arr)[__k++] = (arr1)[__i++];                                              \
        (merged_arr_size) = __k;                                                              \
    } while (0)

/*
 * Binary search for an integer key.
 */
#define WT_BINARY_SEARCH(key, arrayp, n, found)                        \
    do {                                                               \
        uint32_t __base, __indx, __limit;                              \
        (found) = false;                                               \
        for (__base = 0, __limit = (n); __limit != 0; __limit >>= 1) { \
            __indx = __base + (__limit >> 1);                          \
            if ((arrayp)[__indx] < (key)) {                            \
                __base = __indx + 1;                                   \
                --__limit;                                             \
            } else if ((arrayp)[__indx] == (key)) {                    \
                (found) = true;                                        \
                break;                                                 \
            }                                                          \
        }                                                              \
    } while (0)

/*
 * Binary search for a string key. Note: For the binary search to function correctly, the array
 * should not contain NULL values.
 */
#define WT_BINARY_SEARCH_STRING(key, arrayp, n, found)                 \
    do {                                                               \
        uint32_t __base, __indx, __limit;                              \
        (found) = false;                                               \
        for (__base = 0, __limit = (n); __limit != 0; __limit >>= 1) { \
            __indx = __base + (__limit >> 1);                          \
            if (strcmp((arrayp)[__indx], (key)) < 0) {                 \
                __base = __indx + 1;                                   \
                --__limit;                                             \
            } else if (strcmp((arrayp)[__indx], (key)) == 0) {         \
                (found) = true;                                        \
                break;                                                 \
            }                                                          \
        }                                                              \
    } while (0)

#define WT_CLEAR(s) memset(&(s), 0, sizeof(s))

/* Check if a string matches a prefix. */
#define WT_PREFIX_MATCH(str, pfx) (strncmp(str, pfx, strlen(pfx)) == 0)

/* Check if a string matches a suffix. */
#define WT_SUFFIX_MATCH(str, sfx) \
    (strlen(str) >= strlen(sfx) && strcmp(&(str)[strlen(str) - strlen(sfx)], sfx) == 0)

/* Check if a string matches a prefix, and move past it. */
#define WT_PREFIX_SKIP(str, pfx) (WT_PREFIX_MATCH(str, pfx) ? ((str) += strlen(pfx), 1) : 0)

/* Assert that a string matches a prefix, and move past it. */
#define WT_PREFIX_SKIP_REQUIRED(session, str, pfx)     \
    do {                                               \
        WT_ASSERT(session, WT_PREFIX_MATCH(str, pfx)); \
        (str) += strlen(pfx);                          \
    } while (0)

/*
 * Check if a variable string equals a constant string. Inline the common case for WiredTiger of a
 * single byte string. This is required because not all compilers optimize this case in strcmp
 * (e.g., clang). While this macro works in the case of comparing two pointers (a sizeof operator on
 * a pointer won't equal 2 and the extra code will be discarded at compile time), that's not its
 * purpose.
 */
#define WT_STREQ(s, cs) (sizeof(cs) == 2 ? (s)[0] == (cs)[0] && (s)[1] == '\0' : strcmp(s, cs) == 0)

/*
 * Check if a literal string matches the length and content of the supplied bytes/len pair. The
 * bytes argument does not need to be null-terminated, and it may be null if the supplied length is
 * zero. Note this macro works differently than the standard strncmp function. When strncmp is given
 * a zero length it returns true. When this macro is given a zero length it returns false, unless
 * the literal string is also zero length.
 */
#define WT_STRING_LIT_MATCH(str, bytes, len) \
    ((len) == strlen("" str "") && strncmp(str, bytes, len) == 0)

/*
 * Identical to WT_STRING_LIT_MATCH, except that this works with non-literal strings. It is slightly
 * slower, so WT_STRING_LIT_MATCH is always preferred for literal strings.
 */
#define WT_STRING_MATCH(str, bytes, len) __wt_string_match(str, bytes, len)

static WT_INLINE bool
__wt_string_match(const char *str, const char *bytes, size_t len)
{
    return (strncmp(str, bytes, len) == 0 && str[len] == '\0');
}

/*
 * CONFIG versions of the WT_STRING_LIT_MATCH and WT_STRING_MATCH macros. These are convenient when
 * matching WT_CONFIG_ITEMs.
 */
#define WT_CONFIG_LIT_MATCH(s, cval) WT_STRING_LIT_MATCH(s, (cval).str, (cval).len)
#define WT_CONFIG_MATCH(s, cval) WT_STRING_MATCH(s, (cval).str, (cval).len)

/*
 * Macro that produces a string literal that isn't wrapped in quotes, to avoid tripping up spell
 * checkers.
 */
#define WT_UNCHECKED_STRING(str) #str

/* Function return value and scratch buffer declaration and initialization. */
#define WT_DECL_ITEM(i) WT_ITEM *i = NULL
#define WT_DECL_RET int ret = 0

/* If a WT_ITEM data field points somewhere in its allocated memory. */
#define WT_DATA_IN_ITEM(i) \
    ((i)->mem != NULL && (i)->data >= (i)->mem && WT_PTRDIFF((i)->data, (i)->mem) < (i)->memsize)

/* Copy the data and size fields of an item. */
#define WT_ITEM_SET(dst, src)    \
    do {                         \
        (dst).data = (src).data; \
        (dst).size = (src).size; \
    } while (0)

/* Transfer ownership of an item. */
#define WT_ITEM_MOVE(dst, src) \
    do {                       \
        (dst) = (src);         \
        WT_CLEAR(src);         \
    } while (0)

/*
 * In diagnostic mode we track the locations from which hazard pointers and scratch buffers were
 * acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define __wt_hazard_set(session, walk, busyp) \
    __wt_hazard_set_func(session, walk, busyp, __PRETTY_FUNCTION__, __LINE__)
#define __wt_scr_alloc(session, size, scratchp) \
    __wt_scr_alloc_func(session, size, scratchp, __PRETTY_FUNCTION__, __LINE__)
#define __wt_page_in(session, ref, flags) \
    __wt_page_in_func(session, ref, flags, __PRETTY_FUNCTION__, __LINE__)
#define __wt_page_swap(session, held, want, flags) \
    __wt_page_swap_func(session, held, want, flags, __PRETTY_FUNCTION__, __LINE__)
#else
#define __wt_hazard_set(session, walk, busyp) __wt_hazard_set_func(session, walk, busyp)
#define __wt_scr_alloc(session, size, scratchp) __wt_scr_alloc_func(session, size, scratchp)
#define __wt_page_in(session, ref, flags) __wt_page_in_func(session, ref, flags)
#define __wt_page_swap(session, held, want, flags) __wt_page_swap_func(session, held, want, flags)
#endif

/* Random number generator state. */
union __wt_rand_state {
    uint64_t v;
    struct {
        uint32_t w, z;
    } x;
};

/*
 * WT_TAILQ_SAFE_REMOVE_BEGIN/END --
 *	Macro to safely walk a TAILQ where we're expecting some underlying
 * function to remove elements from the list, but we don't want to stop on
 * error, nor do we want an error to turn into an infinite loop. Used during
 * shutdown, when we're shutting down various lists. Unlike TAILQ_FOREACH_SAFE,
 * this macro works even when the next element gets removed along with the
 * current one.
 */
#define WT_TAILQ_SAFE_REMOVE_BEGIN(var, head, field, tvar)                     \
    for ((tvar) = NULL; ((var) = TAILQ_FIRST(head)) != NULL; (tvar) = (var)) { \
        if ((tvar) == (var)) {                                                 \
            /* Leak the structure. */                                          \
            TAILQ_REMOVE(head, (var), field);                                  \
            continue;                                                          \
        }
#define WT_TAILQ_SAFE_REMOVE_END }

/*
 * WT_VA_ARGS_BUF_FORMAT --
 *	Format into a scratch buffer, extending it as necessary. This is a
 * macro because we need to repeatedly call va_start/va_end and there's no
 * way to do that inside a function call.
 */
#define WT_VA_ARGS_BUF_FORMAT(session, buf, fmt, concatenate)                   \
    do {                                                                        \
        size_t __len, __space;                                                  \
        va_list __ap;                                                           \
        int __ret_xx; /* __ret already used by WT_ERR */                        \
        char *__p;                                                              \
                                                                                \
        /*                                                                      \
         * This macro is used to both initialize and concatenate into a         \
         * buffer. If not concatenating, clear the size so we don't use         \
         * any existing contents.                                               \
         */                                                                     \
        if (!(concatenate))                                                     \
            (buf)->size = 0;                                                    \
        for (;;) {                                                              \
            WT_ASSERT(session, (buf)->memsize >= (buf)->size);                  \
            if ((__p = (buf)->mem) != NULL)                                     \
                __p += (buf)->size;                                             \
            __space = (buf)->memsize - (buf)->size;                             \
                                                                                \
            /* Format into the buffer. */                                       \
            va_start(__ap, fmt);                                                \
            __ret_xx = __wt_vsnprintf_len_set(__p, __space, &__len, fmt, __ap); \
            va_end(__ap);                                                       \
            WT_ERR(__ret_xx);                                                   \
                                                                                \
            /* Check if there was enough space. */                              \
            if (__len < __space) {                                              \
                (buf)->data = (buf)->mem;                                       \
                (buf)->size += __len;                                           \
                break;                                                          \
            }                                                                   \
                                                                                \
            /*                                                                  \
             * If not, double the size of the buffer: we're dealing             \
             * with strings, we don't expect the size to get huge.              \
             */                                                                 \
            WT_ERR(__wt_buf_extend(session, buf, (buf)->size + __len + 1));     \
        }                                                                       \
    } while (0)
/*
 * __wt_atomic_decrement_if_positive --
 *     Use compare and swap to atomically decrement value by 1 if it's positive.
 */
static WT_INLINE void
__wt_atomic_decrement_if_positive(uint32_t *valuep)
{
    uint32_t old_value;
    do {
        old_value = __wt_atomic_load32(valuep);
        if (old_value == 0)
            break;
    } while (!__wt_atomic_cas32(valuep, old_value, old_value - 1));
}
