#ifndef MONGOCRYPT_STR_PRIVATE_H
#define MONGOCRYPT_STR_PRIVATE_H

#include "./macros.h"
#include "./user-check.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h> /* INT_MAX */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MLIB_HAVE_STRINGS_H)
#include <strings.h> /* For strncasecmp. */
#endif

MLIB_C_LINKAGE_BEGIN

/**
 * @brief A simple non-owning string-view type.
 *
 * The viewed string can be treated as an array of char. It's pointed-to data
 * must not be freed or manipulated.
 *
 * @note The viewed string is NOT guaranteed to be null-terminated. It WILL
 * be null-terminated if: Directly created from a string literal via
 * @ref mstrv_lit, OR created by accessing the @ref mstr::view member of an
 * @ref mstr object, OR returned from @ref mstrv_view_cstr.
 */
typedef struct mstr_view {
    /**
     * @brief Pointer to the beginning of the code unit array.
     *
     * @note DO NOT MODIFY
     */
    const char *data;
    /**
     * @brief Length of the pointed-to code unit array
     *
     * @note DO NOT MODIFY
     */
    size_t len;
} mstr_view;

/**
 * @brief A simple string utility type.
 *
 * This string type has the following semantics:
 *
 * The member `data` is a pointer to the beginning of a read-only array of code
 * units. This array will always be null-terminated, but MAY contain
 * intermittent null characters. The member `len` is the length of the code unit
 * array (not including the null terminator). These two members should not be
 * modified.
 *
 * The `view` member is a union member that will view the `mstr` as an
 * @ref mstr_view.
 *
 * If you create an @ref mstr, it MUST eventually be passed to @ref mstr_free()
 *
 * The pointed-to code units of an mstr are immutable. To initialize the
 * contents of an mstr, @ref mstr_new returns an @ref mstr_mut, which can then
 * be "sealed" by converting it to an @ref mstr through the @ref mstr_mut::mstr
 * union member.
 *
 * By convention, passing/returning an `mstr` to/from a function should
 * relinquish ownership of that `mstr` to the callee/caller, respectively.
 * Passing or returning an `mstr_view` is non-owning.
 */
typedef struct mstr {
    union {
        struct {
            /**
             * @brief Pointer to the beginning of the code unit array.
             *
             * @note DO NOT MODIFY
             */
            const char *data;
            /**
             * @brief Length of the pointed-to code unit array
             *
             * @note DO NOT MODIFY
             */
            size_t len;
        };

        /**
         * @brief A non-owning `mstr_view` of the string
         */
        mstr_view view;
    };
} mstr;

/**
 * @brief An interface for initializing the contents of an mstr.
 *
 * Returned by @ref mstr_new(). Once initialization is complete, the result can
 * be used as an @ref mstr by accessing the @ref mstr_mut::mstr member.
 */
typedef struct mstr_mut {
    union {
        struct {
            /**
             * @brief Pointer to the beginning of the mutable code unit array.
             *
             * @note DO NOT MODIFY THE POINTER VALUE. Only modify the pointed-to
             * characters.
             */
            char *data;
            /**
             * @brief Length of the pointed-to code unit array.
             *
             * @note DO NOT MODIFY
             */
            size_t len;
        };
        /// Convert the mutable string to an immutable string
        struct mstr mstr;
        /// Convert the mutable string to an immutable string view
        mstr_view view;
    };
} mstr_mut;

/**
 * @brief A null @ref mstr
 */
#define MSTR_NULL (MLIB_INIT(mstr){{{NULL, 0}}})
/**
 * @brief A null @ref mstr_view
 */
#define MSTRV_NULL (MLIB_INIT(mstr_view){NULL, 0})

/**
 * @brief Create an @ref mstr_view that views the given string literal
 */
#define mstrv_lit(String) (mstrv_view_data(String "", (sizeof String) - 1))

/**
 * @brief Create a new mutable code-unit array of the given length,
 * zero-initialized. The caller can then modify the code units in the array via
 * the @ref mstr_mut::data member. Once finished modifying, can be converted to
 * an immutable mstr by copying the @ref mtsr_mut::mstr union member.
 *
 * @param len The length of the new string.
 * @return mstr_mut A new mstr_mut
 *
 * @note The @ref mstr_mut::mstr member MUST eventually be given to
 * @ref mstr_free().
 */
static inline mstr_mut mstr_new(size_t len) {
#ifndef __clang_analyzer__
    assert(len < SIZE_MAX);
    return MLIB_INIT(mstr_mut){{{(char *)calloc(1, len + 1), len}}};
#else
    // Clang-analyzer is smart enough to see the calloc(), but not smart enough
    // to link it to the free() in mstr_free()
    return MLIB_INIT(mstr_mut){};
#endif
}

/**
 * @brief Create a non-owning @ref mstr_view from the given C string and length
 *
 * @param s A pointer to the beginning of a character array.
 * @param len The length of the character array, in code units
 * @return mstr_view A non-owning string.
 */
static inline mstr_view mstrv_view_data(const char *s, size_t len) {
    return MLIB_INIT(mstr_view){s, len};
}

/**
 * @brief Create a non-owning @ref mstr_view from a C-style null-terminated
 * string.
 *
 * @param s A pointer to a null-terminated character array
 * @return mstr_view A view of the pointed-to string
 */
static inline mstr_view mstrv_view_cstr(const char *s) {
    return mstrv_view_data(s, strlen(s));
}

/**
 * @brief Create an @ref mstr from the given character array and length.
 *
 * @param s A pointer to a character array
 * @param len The length of the string to create
 * @return mstr A new null-terminated string with the contents copied from the
 * pointed-to array.
 *
 * @note The resulting string will be null-terminated.
 */
static inline mstr mstr_copy_data(const char *s, size_t len) {
    mstr_mut r = mstr_new(len);
    memcpy(r.data, s, len);
    return r.mstr;
}

/**
 * @brief Create an @ref mstr from A C-style null-terminated string.
 *
 * @param s A pointer to a null-terminated character array
 * @return mstr A new string copied from the pointed-to string
 */
static inline mstr mstr_copy_cstr(const char *s) {
    return mstr_copy_data(s, strlen(s));
}

/**
 * @brief Copy the contents of the given string view
 *
 * @param s A string view to copy from
 * @return mstr A new string copied from the given view
 */
static inline mstr mstr_copy(mstr_view s) {
    return mstr_copy_data(s.data, s.len);
}

/**
 * @brief Free the resources of the given string
 *
 * @param s The string to free
 */
static inline void mstr_free(mstr s) {
    free((char *)s.data);
}

/**
 * @brief Resize the given mutable string, maintaining the existing content, and
 * zero-initializing any added characters.
 *
 * @param s The @ref mstr_mut to update
 * @param new_len The new length of the string
 */
static inline void mstrm_resize(mstr_mut *s, size_t new_len) {
    if (new_len <= s->len) {
        s->len = new_len;
    } else {
        const size_t old_len = s->len;
#ifndef __clang_analyzer__
        // Clang-analyzer is smart enough to see the calloc(), but not smart
        // enough to link it to the free() in mstr_free()
        assert(new_len < SIZE_MAX);
        s->data = (char *)realloc((char *)s->data, new_len + 1);
#endif
        s->len = new_len;
        assert(new_len >= old_len);
        memset(s->data + old_len, 0, new_len - old_len);
    }
    s->data[new_len] = (char)0;
}

/**
 * @brief Free and re-assign the given @ref mstr
 *
 * @param s Pointer to an @ref mstr. This will be freed, then updated to the
 * value of @ref from
 * @param from An @ref mstr to take from.
 *
 * @note Ownership of the resource is handed to the pointed-to @ref s.
 * Equivalent to:
 *
 * ```c
 * mstr s = some_mstr();
 * mstr another = get_another_mstr();
 * mstr_free(s);
 * s = another;
 * ```
 *
 * Intended as a convenience for rebinding an @ref mstr in a single statement
 * from an expression returning a new @ref mstr, which may itself use @ref s,
 * without requiring a temporary variable, for example:
 *
 * ```c
 * mstr s = get_mstr();
 * mstr_assign(&s, convert_to_uppercase(s.view));
 * ```
 */
static inline void mstr_assign(mstr *s, mstr from) {
    mstr_free(*s);
    *s = from;
}

/**
 * @brief Find the index of the first occurrence of the given "needle" as a
 * substring of another string.
 *
 * @param given A string to search within
 * @param needle The substring to search for
 * @return int The zero-based index of the first instance of `needle` in
 * `given`, or -1 if no substring is found.
 */
static inline int mstr_find(mstr_view given, mstr_view needle) {
    const char *const scan_end = given.data + given.len;
    const char *const needle_end = needle.data + needle.len;
    for (const char *scan = given.data; scan != scan_end; ++scan) {
        size_t remain = (size_t)(scan_end - scan);
        if (remain < needle.len) {
            break;
        }
        const char *subscan = scan;
        for (const char *nscan = needle.data; nscan != needle_end; ++nscan, ++subscan) {
            if (*nscan == *subscan) {
                continue;
            } else {
                goto skip;
            }
        }
        // Got through the whole loop of scanning the needle
        return (int)(scan - given.data);
    skip:
        (void)0;
    }
    return -1;
}

/**
 * @brief Find the index of the last occurrence of the given "needle" as a
 * substring of another string.
 *
 * @param given A string to search within
 * @param needle The substring to search for
 * @return int The zero-based index of the last instance of `needle` in
 * `given`, or -1 if no substring is found.
 */
static inline int mstr_rfind(mstr_view given, mstr_view needle) {
    if (needle.len > given.len) {
        return -1;
    }
    const char *scan = given.data + given.len - needle.len;
    const char *const needle_end = needle.data + needle.len;
    for (; scan >= given.data; --scan) {
        const char *subscan = scan;
        for (const char *nscan = needle.data; nscan != needle_end; ++nscan, ++subscan) {
            if (*nscan == *subscan) {
                continue;
            } else {
                goto skip;
            }
        }
        // Got through the whole loop of scanning the needle
        return (int)(scan - given.data);
    skip:
        (void)0;
    }
    return -1;
}

/**
 * @brief Modify a string by deleting and/or inserting another string.
 *
 * @param s The string to modify
 * @param at The position at which to insert and delete characters
 * @param del_count The number of characters to delete. Clamped to the string
 * length.
 * @param insert The string to insert at `at`.
 * @return mstr A new string that is the result of the splice
 */
static inline mstr mstr_splice(mstr_view s, size_t at, size_t del_count, mstr_view insert) {
    assert(at <= s.len);
    const size_t remain = s.len - at;
    if (del_count > remain) {
        del_count = remain;
    }
    /* at this point, it is absolutely necessary that del_count <= s.len */
    assert(s.len - del_count <= SIZE_MAX - insert.len);
    const size_t new_size = s.len - del_count + insert.len;
    mstr_mut ret = mstr_new(new_size);
    char *p = ret.data;
    memcpy(p, s.data, at);
    p += at;
    if (insert.data) {
        memcpy(p, insert.data, insert.len);
        p += insert.len;
    }
    /* 'at <= s.len' was already asserted earlier */
    assert(s.len - at >= del_count);
    memcpy(p, s.data + at + del_count, s.len - at - del_count);
    return ret.mstr;
}

/**
 * @brief Append the given suffix to the given string
 */
static inline mstr mstr_append(mstr_view s, mstr_view suffix) {
    return mstr_splice(s, s.len, 0, suffix);
}

/**
 * @brief Prepend the given prefix to the given string
 */
static inline mstr mstr_prepend(mstr_view s, mstr_view prefix) {
    return mstr_splice(s, 0, 0, prefix);
}

/**
 * @brief Insert the given string into another string
 *
 * @param s The string to start with
 * @param at The position in `s` where `infix` will be inserted
 * @param infix The string to insert into `s`
 * @return mstr A new string with `infix` inserted
 */
static inline mstr mstr_insert(mstr_view s, size_t at, mstr_view infix) {
    return mstr_splice(s, at, 0, infix);
}

/**
 * @brief Erase characters from the given string
 *
 * @param s The string to start with
 * @param at The position at which to begin deleting characters
 * @param count The number of characters to remove
 * @return mstr A new string with the deletion result.
 */
static inline mstr mstr_erase(mstr_view s, size_t at, size_t count) {
    return mstr_splice(s, at, count, mstrv_view_cstr(""));
}

/**
 * @brief Erase `len` characters from the beginning of the string
 */
static inline mstr mstr_remove_prefix(mstr_view s, size_t len) {
    return mstr_erase(s, 0, len);
}

/**
 * @brief Erase `len` characters from the end of the string
 */
static inline mstr mstr_remove_suffix(mstr_view s, size_t len) {
    assert(s.len >= len);
    return mstr_erase(s, s.len - len, len);
}

/**
 * @brief Obtain a substring of the given string
 *
 * @param s The string to start with
 * @param at The beginning position of the new string
 * @param len The number of characters to include. Automatically clamped to the
 * remaining length.
 * @return mstr A new string that is a substring of `s`
 */
static inline mstr mstr_substr(mstr_view s, size_t at, size_t len) {
    assert(at <= s.len);
    const size_t remain = s.len - at;
    if (len > remain) {
        len = remain;
    }
    mstr_mut r = mstr_new(len);
    memcpy(r.data, s.data + at, len);
    return r.mstr;
}

/**
 * @brief Obtain a view of a substring of another string.
 *
 * @param s The string to view
 * @param at The position at which the new view will begin
 * @param len The number of characters to view. Automatically clamped to the
 * remaining length.
 * @return mstr_view A view of `s`.
 */
static inline mstr_view mstrv_subview(mstr_view s, size_t at, size_t len) {
    assert(at <= s.len);
    const size_t remain = s.len - at;
    if (len > remain) {
        len = remain;
    }
    return mstrv_view_data(s.data + at, len);
}

/**
 * @brief Obtain a view of another string by removing `len` characters from the
 * front
 */
static inline mstr_view mstrv_remove_prefix(mstr_view s, size_t len) {
    return mstrv_subview(s, len, s.len);
}

/**
 * @brief Obtain a view of another string by removing `len` characters from the
 * end.
 */
static inline mstr_view mstrv_remove_suffix(mstr_view s, size_t len) {
    assert(s.len >= len);
    return mstrv_subview(s, 0, s.len - len);
}

/**
 * @brief Truncate the given string to `new_len` characters.
 *
 * @param s The string to truncate
 * @param new_len The new length of the string
 * @return mstr A new string copied from the beginning of `s`
 */
static inline mstr mstr_trunc(mstr_view s, size_t new_len) {
    assert(new_len <= s.len);
    return mstr_remove_suffix(s, s.len - new_len);
}

/**
 * @brief Obtain a new string with all occurrences of a string replaced with a
 * different string
 *
 * @param string The string to start with
 * @param find The substring that will be replaced
 * @param subst The string to insert in place of `find`
 * @return mstr A new string modified from `string`
 *
 * @note If `find` is empty, returns a copy of `string`
 */
static inline mstr mstr_replace(const mstr_view string, const mstr_view find, const mstr_view subst) {
    if (find.len == 0) {
        // Finding an empty string would loop forever
        return mstr_copy(string);
    }
    // First copy the string
    mstr ret = mstr_copy(string);
    // Keep an index of how far we have processed
    size_t whence = 0;
    for (;;) {
        // Chop off the front that has already been processed
        mstr_view tail = mstrv_subview(ret.view, whence, SIZE_MAX);
        // Find where in that tail is the next needle
        int pos = mstr_find(tail, find);
        if (pos == -1) {
            // We're done
            break;
        }
        // Do the replacement
        assert(whence <= SIZE_MAX - (size_t)pos);
        mstr_assign(&ret, mstr_splice(ret.view, (size_t)pos + whence, find.len, subst));
        // Advance our position by how many chars we skipped and how many we
        // inserted
        whence += (size_t)pos + subst.len;
    }
    return ret;
}

/**
 * @brief Determine whether two strings are equivalent.
 */
static inline bool mstr_eq(mstr_view left, mstr_view right) {
    if (left.len != right.len) {
        return false;
    }
    return memcmp(left.data, right.data, left.len) == 0;
}

/**
 * @brief Determine whether two strings are equivalent ignoring case.
 */
static inline bool mstr_eq_ignore_case(mstr_view left, mstr_view right) {
#ifdef _WIN32
#define _mstr_strncasecmp _strnicmp
#else
#define _mstr_strncasecmp strncasecmp
#endif

    if (left.len != right.len) {
        return false;
    }
    return _mstr_strncasecmp(left.data, right.data, left.len) == 0;
#undef _mstr_strncasecmp
}

/// Determine whether the given character is an printable ASCII codepoint
static inline bool mstr_is_printable(char c) {
    return (c >= ' ' && c <= '~');
}

/// Write the given string to `out`, rendering non-printable characters as hex
/// escapes
static inline void _mstr_write_str_repr_(FILE *out, mstr_view s) {
    for (const char *it = s.data; it != s.data + s.len; ++it) {
        if (mstr_is_printable(*it)) {
            fputc(*it, out);
        } else {
            fprintf(out, "\\x%.2x", (unsigned)(unsigned char)*it);
        }
    }
}

static inline void
_mstr_assert_fail_(mstr_view left, const char *predicate, mstr_view right, const char *file, int line) {
    fprintf(stderr, "%s:%d: ASSERTION FAILED: \"", file, line);
    _mstr_write_str_repr_(stderr, left);
    fprintf(stderr, "\" %s \"", predicate);
    _mstr_write_str_repr_(stderr, right);
    fprintf(stderr, "\"\n");
    abort();
}

static inline void _mstr_assert_(mstr_view left,
                                 mstr_view right,
                                 bool (*pred)(mstr_view left, mstr_view right),
                                 bool B,
                                 const char *pred_str,
                                 const char *file,
                                 int line) {
    if (pred(left, right) != B) {
        mstr pstr = mstr_copy_cstr(pred_str);
        if (!B) {
            mstr_assign(&pstr, mstr_prepend(pstr.view, mstrv_lit("not ")));
        }
        _mstr_assert_fail_(left, pstr.data, right, file, line);
    }
}

#define MSTR_ASSERT(Bool, Left, Pred, Right)                                                                           \
    (_mstr_assert_((Left), (Right), mstr_##Pred, (Bool), #Pred, __FILE__, __LINE__))

/**
 * @brief Assert that two strings are equivalent.
 *
 * Prints and error message and aborts if they are not
 */
#define MSTR_ASSERT_EQ(Left, Right) MSTR_ASSERT(true, Left, eq, Right)

/**
 * @brief Determine whether the given string contains the given substring
 *
 * @param given A string to search within
 * @param needle A substring to search for
 * @return true If `given` contains at least one occurrence of `needle`
 * @return false Otherwise
 */
static inline bool mstr_contains(mstr_view given, mstr_view needle) {
    return mstr_find(given, needle) >= 0;
}

/**
 * @brief Determine whether `given` starts with `prefix`
 */
static inline bool mstr_starts_with(mstr_view given, mstr_view prefix) {
    given = mstrv_subview(given, 0, prefix.len);
    return mstr_eq(given, prefix);
}

/**
 * @brief Determine whether `given` ends with `suffix`
 */
static inline bool mstr_ends_with(mstr_view given, mstr_view suffix) {
    if (suffix.len > given.len) {
        return false;
    }
    given = mstrv_subview(given, given.len - suffix.len, SIZE_MAX);
    return mstr_eq(given, suffix);
}

/// Compound in-place version of @ref mstr_splice
static inline void mstr_inplace_splice(mstr *s, size_t at, size_t del_count, mstr_view insert) {
    mstr_assign(s, mstr_splice(s->view, at, del_count, insert));
}

/// Compound in-place version of @ref mstr_append
static inline void mstr_inplace_append(mstr *s, mstr_view suffix) {
    mstr_assign(s, mstr_append(s->view, suffix));
}

/// Compound in-place version of @ref mstr_prepend
static inline void mstr_inplace_prepend(mstr *s, mstr_view prefix) {
    mstr_assign(s, mstr_prepend(s->view, prefix));
}

/// Compound in-place version of @ref mstr_insert
static inline void mstr_inplace_insert(mstr *s, size_t at, mstr_view infix) {
    mstr_assign(s, mstr_insert(s->view, at, infix));
}

/// Compound in-place version of @ref mstr_erase
static inline void mstr_inplace_erase(mstr *s, size_t at, size_t count) {
    mstr_assign(s, mstr_erase(s->view, at, count));
}

/// Compound in-place version of @ref mstr_remove_prefix
static inline void mstr_inplace_remove_prefix(mstr *s, size_t len) {
    mstr_assign(s, mstr_remove_prefix(s->view, len));
}

/// Compound in-place version of @ref mstr_remove_suffix
static inline void mstr_inplace_remove_suffix(mstr *s, size_t len) {
    mstr_assign(s, mstr_remove_suffix(s->view, len));
}

/// Compound in-place version of @ref mstr_substr
static inline void mstr_inplace_substr(mstr *s, size_t at, size_t count) {
    mstr_assign(s, mstr_substr(s->view, at, count));
}

/// Compound in-place version of @ref mstr_trunc
static inline void mstr_inplace_trunc(mstr *s, size_t new_len) {
    mstr_assign(s, mstr_trunc(s->view, new_len));
}

/// Compound in-place version of @ref mstr_replace
static inline void mstr_inplace_replace(mstr *s, mstr_view find, mstr_view subst) {
    mstr_assign(s, mstr_replace(s->view, find, subst));
}

#ifdef _WIN32
#include "./windows-lean.h"

/**
 * @brief The result type of mstr_win32_widen
 */
typedef struct mstr_widen_result {
    wchar_t *wstring;
    int error;
} mstr_widen_result;

/**
 * @brief Widen a UTF-8 string using Win32 MultiBytetoWideChar
 *
 * @param str The UTF-8 string to widen.
 * @return mstr_widen_result The result of widening, which may contain an error.
 *
 * @note The returned @ref mstr_widen_result::wstring must be given to free()
 */
static inline mstr_widen_result mstr_win32_widen(mstr_view str) {
    assert(str.len <= INT_MAX);
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data, (int)str.len, NULL, 0);
    if (length == 0 && str.len != 0) {
        return MLIB_INIT(mstr_widen_result){NULL, (int)GetLastError()};
    }
    wchar_t *ret = (wchar_t *)calloc(length + 1, sizeof(wchar_t));
    assert(length < INT_MAX);
    int got_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, str.data, (int)str.len, ret, length + 1);
    assert(got_length == length);
    return MLIB_INIT(mstr_widen_result){ret, 0};
}

/**
 * @brief The result type of mstr_win32_narrow
 */
typedef struct mstr_narrow_result {
    mstr string;
    int error;
} mstr_narrow_result;

/**
 * @brief Narrow a UTF-16 string to UTF-8 using Win32 WideCharToMultiByte
 *
 * @param wstring A null-terminated UTF-16 string to narrow
 * @return mstr_narrow_result The result of narrowing, which may contain an
 * error.
 *
 * @note The returned @ref mstr_narrow_result::string must be freed with
 * mstr_free()
 */
static inline mstr_narrow_result mstr_win32_narrow(const wchar_t *wstring) {
    // Some older versions of MinGW fail to include the WC_ERR_INVALID_CHARS
    // flag, so we will specify it manually:
    DWORD wcflags = 0x80; // WC_ERR_INVALID_CHARS
    int length =
        WideCharToMultiByte(CP_UTF8, wcflags, wstring, -1 /* wstring is null-terminated */, NULL, 0, NULL, NULL);
    if (length == 0 && wstring[0] != 0) {
        return MLIB_INIT(mstr_narrow_result){MSTR_NULL, (int)GetLastError()};
    }
    // Allocate a new string, not including the null terminator
    mstr_mut ret = mstr_new((size_t)(length - 1));
    int got_len = WideCharToMultiByte(CP_UTF8,
                                      wcflags,
                                      wstring,
                                      -1,
                                      ret.data,
                                      // Plus one byte for the NUL
                                      (int)(ret.len + 1),
                                      NULL,
                                      NULL);
    assert(length == got_len);
    return MLIB_INIT(mstr_narrow_result){ret.mstr, 0};
}
#endif

/// Iteration state for string splitting
struct _mstr_split_iter_ {
    /// What hasn't been parsed yet
    mstr_view remaining;
    /// The string that we split on
    mstr_view splitter;
    /// The current part
    mstr_view part;
    /// A once-var for the inner loop. Set to 1 by iter_next, then decremented
    int once;
    /// The loop state. Starts at zero. Set to one when we part the final split.
    /// Set to two to break out of the loop.
    int state;
};

/// Hidden function to advance a string-split iterator
static inline void _mstr_split_iter_next_(struct _mstr_split_iter_ *iter) {
    if (iter->once == 1) {
        // We only get here if the loop body hit a 'break', skipping the decrement
        // of the 'once'. Break out of the whole loop, as the user expects.
        iter->state = 2;
        return;
    }
    if (iter->state == 1) {
        // We just completed the final loop pass.
        iter->state = 2;
        return;
    }
    // Find the next occurence of the token
    const int pos = mstr_find(iter->remaining, iter->splitter);
    if (pos < 0) {
        // There are no more occurences. yield the remaining string
        iter->part = iter->remaining;
        iter->remaining = mstrv_subview(iter->remaining, iter->remaining.len, 0);
        // Set state to 1 to break on the next pass
        iter->state = 1;
    } else {
        // Advance our parts:
        iter->part = mstrv_subview(iter->remaining, 0, (size_t)pos);
        assert(iter->splitter.len <= SIZE_MAX - (size_t)pos);
        iter->remaining = mstrv_subview(iter->remaining, (size_t)pos + iter->splitter.len, SIZE_MAX);
    }
    // Prime the inner "loop" to execute once
    iter->once = 1;
}

/// init a new split iterator
static inline struct _mstr_split_iter_ _mstr_split_iter_begin_(mstr_view str, mstr_view split) {
    struct _mstr_split_iter_ iter = {str, split};
    _mstr_split_iter_next_(&iter);
    return iter;
}

/// Check whether we are done iterating
static inline bool _mstr_split_iter_done_(struct _mstr_split_iter_ *iter) {
    return iter->state == 2;
}

// clang-format off
#define MSTR_ITER_SPLIT(LineVar, String, SplitToken)              \
   /* Open the main outer loop */                                 \
   for (/* Declare and init the iterator */                       \
        struct _mstr_split_iter_ _iter_var_ =                     \
            _mstr_split_iter_begin_ ((String), (SplitToken));     \
         /* Iterate until it is marked as done */                 \
        !_mstr_split_iter_done_ (&_iter_var_);                    \
        _mstr_split_iter_next_ (&_iter_var_))                     \
      /* This inner loop will only execute once, but gives us */  \
      /* a point to declare the loop variable: */                 \
      for (mstr_view const LineVar = _iter_var_.part;             \
           _iter_var_.once;                                       \
           --_iter_var_.once)

// clang-format on

/**
 * @brief Equivalent to strlen(), but has a constexpr annotation.
 */
static mlib_constexpr_fn size_t mlib_strlen(const char *s) {
    size_t r = 0;
    for (; *s; ++r, ++s) {}
    return r;
}

/**
 * @brief Copy characters into the destination, guaranteed null-terminated and
 * bounds checked.
 *
 * @param dst Pointer to the beginning of the destination array.
 * @param dst_bufsize The size of the destination array, in characters. MUST be
 * greater than zero.
 * @param src Pointer to the beginning of a null-terminated character array.
 * @param src_bufsize The size of the array pointed-to by `src`.
 * @return size_t The number `R` of characters written (NOT including the null
 * terminator). R is guaranteed to be less than dst_bufsize, and
 * less-than-or-equal-to src_bufsize.
 *
 * @note Characters beyond (dst + R) are unmodified. dst[R] is guaranteed to
 * be a null terminator.
 */
static mlib_constexpr_fn size_t mlib_strnmcopy(char *dst, size_t dst_bufsize, const char *src, size_t src_bufsize) {
    // No empty destination, since we *must* write a null terminator:
    assert(dst_bufsize > 0);
    // The maximum number of characters in the dest is one less than the buffer
    // size, since we need room for the null terminator:
    const size_t dstlen = dst_bufsize - 1u;
    // The actual maximum number of characters we can copy is the less of the
    // source length and the dest length:
    const size_t minlen = dstlen < src_bufsize ? dstlen : src_bufsize;
    // Track what we copy:
    size_t ncopied = 0;
    while (ncopied != minlen // Stop if we hit our character limit
           && *src != 0      // Or if we hit the null terminator in the source
    ) {
        // Copy:
        *dst = *src;
        // Advance:
        ++dst;
        ++src;
        ++ncopied;
    }
    // "dst" now points past the final character we copied (if any), and is still
    // in-bounds. This will be the null terminator.
    *dst = 0;
    return ncopied;
}

MLIB_C_LINKAGE_END

#endif // MONGOCRYPT_STR_PRIVATE_H
