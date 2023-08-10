#ifndef MONGOCRYPT_PATH_PRIVATE_H
#define MONGOCRYPT_PATH_PRIVATE_H

#include "./user-check.h"

#include "./str.h"

#include <inttypes.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include "./windows-lean.h"
#endif

typedef enum mpath_format {
    /// The POSIX path format
    MPATH_POSIX = 'p',
    /// The Win32 path format
    MPATH_WIN32 = 'w',
    /// The native path format for the current platform
    MPATH_NATIVE =
#ifdef _WIN32
        MPATH_WIN32,
#else
        MPATH_POSIX,
#endif
} mpath_format;

/**
 * @brief Determine if the given character is a path separator for the given
 * path format
 *
 * @param c A path character
 * @param f The path format to use
 */
static inline bool mpath_is_sep(char c, mpath_format f) {
    if (f == MPATH_WIN32) {
        return c == '/' || c == '\\';
    } else {
        return c == '/';
    }
}

/**
 * @brief Obtain the preferred path separator character for the given format
 */
static inline char mpath_preferred_sep(mpath_format f) {
    if (f == MPATH_WIN32) {
        return '\\';
    } else {
        return '/';
    }
}

/**
 * @brief Obtain the path string denoting the application's current working
 * directory
 *
 * @return mstr A new string which must be freed with mstr_free()
 */
static inline mstr mpath_current_path(void) {
#if _WIN32
    while (1) {
        DWORD len = GetCurrentDirectoryW(0, NULL);
        wchar_t *wstr = calloc(sizeof(wchar_t), len);
        DWORD got_len = GetCurrentDirectoryW(len, wstr);
        if (got_len > len) {
            free(wstr);
            continue;
        }
        mstr_narrow_result nar = mstr_win32_narrow(wstr);
        free(wstr);
        assert(nar.error == 0);
        return nar.string;
    }
#else
    mstr_mut mut = mstr_new(8096);
    char *p = getcwd(mut.data, mut.len);
    if (p == NULL) {
        mstr_free(mut.mstr);
        return MSTR_NULL;
    }
    mstr ret = mstr_copy_cstr(mut.data);
    mstr_free(mut.mstr);
    return ret;
#endif
}

/**
 * @brief Determine whether the given path string has a trailing path separator
 */
static inline bool mpath_has_trailing_sep(mstr_view path, mpath_format f) {
    return path.len && mpath_is_sep(path.data[path.len - 1], f);
}

/**
 * @brief Obtain the parent path of the given path.
 */
static inline mstr_view mpath_parent(mstr_view path, mpath_format f) {
    if (mpath_has_trailing_sep(path, f)) {
        // Remove trailing separators:
        while (mpath_has_trailing_sep(path, f)) {
            path = mstrv_remove_suffix(path, 1);
        }
        return path;
    }
    // Remove everything that isn't a path separator:
    while (path.len != 0 && !mpath_has_trailing_sep(path, f)) {
        path = mstrv_remove_suffix(path, 1);
    }
    // Remove trailing separators again
    while (path.len > 1 && mpath_has_trailing_sep(path, f)) {
        path = mstrv_remove_suffix(path, 1);
    }
    // The result is the parent path.
    return path;
}

/**
 * @brief Obtain the filename denoted by the given path.
 *
 * The returned path will include no directory separators. If the given path
 * ends with a directory separator, the single-dot '.' path is returned instead.
 */
static inline mstr_view mpath_filename(mstr_view path, mpath_format f) {
    if (!path.len) {
        return path;
    }
    const char *it = path.data + path.len;
    while (it != path.data && !mpath_is_sep(it[-1], f)) {
        --it;
    }
    size_t off = (size_t)(it - path.data);
    mstr_view fname = mstrv_subview(path, off, path.len);
    if (fname.len == 0) {
        return mstrv_lit(".");
    }
    return fname;
}

/**
 * @brief Join the two given paths into a single path
 *
 * The two strings will be combined into a single string with a path separator
 * between them. If either string is empty, the other string will be copied
 * without modification.
 *
 * @param base The left-hand of the join
 * @param suffix The right-hand of the join
 * @param f The path format to use
 * @return mstr A new string resulting from the join
 */
static inline mstr mpath_join(mstr_view base, mstr_view suffix, mpath_format f) {
    if (!base.len) {
        return mstr_copy(suffix);
    }
    if (mpath_has_trailing_sep(base, f)) {
        return mstr_append(base, suffix);
    }
    if (!suffix.len) {
        return mstr_copy(base);
    }
    if (mpath_is_sep(suffix.data[0], f)) {
        return mstr_append(base, suffix);
    }
    // We must insert a path separator between the two strings
    assert(base.len <= SIZE_MAX - suffix.len - 1u);
    mstr_mut r = mstr_new(base.len + suffix.len + 1);
    char *p = r.data;
    memcpy(p, base.data, base.len);
    p += base.len;
    *p++ = mpath_preferred_sep(f);
    memcpy(p, suffix.data, suffix.len);
    return r.mstr;
}

/**
 * @brief Obtain the root name for the given path.
 *
 * For the Windows format, this will return the drive letter, if present.
 * Otherwise, this will return an empty string.
 */
static inline mstr_view mpath_root_name(mstr_view path, mpath_format f) {
    if (f == MPATH_WIN32 && path.len > 1) {
        char c = path.data[0];
        if (path.len > 2 && path.data[1] == ':' && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return mstrv_subview(path, 0, 2);
        }
    }
    return mstrv_subview(path, 0, 0);
}

/**
 * @brief Return the root directory of the given path, if present.
 *
 * @note This will not include the drive letter of a Win32 path.
 */
static inline mstr_view mpath_root_directory(mstr_view path, mpath_format f) {
    mstr_view rname = mpath_root_name(path, f);
    path = mstrv_subview(path, rname.len, path.len);
    if (path.len && mpath_is_sep(path.data[0], f)) {
        return mstrv_subview(path, 0, 1);
    } else {
        return mstrv_subview(path, 0, 0);
    }
}

/**
 * @brief Obtain the root filepath of the given path.
 *
 * This will include both the root name and the root filepath, if present.
 */
static inline mstr_view mpath_root_path(mstr_view path, mpath_format f) {
    mstr_view rname = mpath_root_name(path, f);
    mstr_view rdir = mpath_root_directory(path, f);
    assert(rname.len <= SIZE_MAX - rdir.len);
    return mstrv_subview(path, 0, rname.len + rdir.len);
}

/**
 * @brief Determine whether the given filepath designates a single unambiguous
 * filesystem location.
 *
 * @note A Win32 filepath without a drive letter is not absolute!
 */
static inline bool mpath_is_absolute(mstr_view path, mpath_format f) {
    if (f == MPATH_WIN32) {
        // Win32 requires both a root name and a root directory for an absolute
        // path
        return mpath_root_name(path, f).len && mpath_root_directory(path, f).len;
    } else {
        // POSIX doesn't have "root names"
        return mpath_root_directory(path, f).len;
    }
}

/**
 * @brief Obtain a relative path from the given filepath
 *
 * If the path has a root path, returns the content of the path following that
 * root path, otherwise returns the same path itself.
 */
static inline mstr_view mpath_relative_path(mstr_view path, mpath_format f) {
    mstr_view root = mpath_root_path(path, f);
    return mstrv_subview(path, root.len, path.len);
}

/**
 * @brief Convert the filepath from one format to a preferred form in another
 * format.
 *
 * @note The return value must be freed with mstr_free()
 */
static inline mstr mpath_to_format(mpath_format from, mstr_view path, mpath_format to) {
    mstr_mut ret = mstr_new(path.len);
    const char *p = path.data;
    char *out = ret.data;
    const char *stop = path.data + path.len;
    for (; p != stop; ++p, ++out) {
        if (mpath_is_sep(*p, from)) {
            *out = mpath_preferred_sep(to);
        } else {
            *out = *p;
        }
    }
    return ret.mstr;
}

/**
 * @brief Determine whether the given path is relative (not absolute)
 */
static inline bool mpath_is_relative(mstr_view path, mpath_format f) {
    return !mpath_is_absolute(path, f);
}

/**
 * @brief Convert the given path to an absolute path, if it is not already.
 *
 * @note The return value must be freed with mstr_free()
 */
static inline mstr mpath_absolute(mstr_view path, mpath_format f);

/**
 * @brief Resolve a path to an absolute path from the given base path.
 *
 * @note This is not the same as mpath_join(): If the given path is already
 * absolute, returns that path unchanged. Otherwise, resolves that path as being
 * relative to `base`.
 *
 * @note If `base` is also a relative path, it will also be given to
 * mpath_absolute() to resolve it.
 */
static inline mstr mpath_absolute_from(mstr_view path, mstr_view base, mpath_format f) {
    mstr_view rname = mpath_root_name(path, f);
    mstr_view rdir = mpath_root_directory(path, f);
    if (rname.len) {
        if (rdir.len) {
            // The path is already fully absolute
            return mstr_copy(path);
        } else {
            mstr abs_base = mpath_absolute(base, f);
            mstr_view base_rdir = mpath_root_directory(abs_base.view, f);
            mstr_view base_relpath = mpath_relative_path(abs_base.view, f);
            mstr_view relpath = mpath_relative_path(path, f);
            mstr ret = mstr_copy(rname);
            mstr_assign(&ret, mpath_join(ret.view, base_rdir, f));
            mstr_assign(&ret, mpath_join(ret.view, base_relpath, f));
            mstr_assign(&ret, mpath_join(ret.view, relpath, f));
            mstr_free(abs_base);
            return ret;
        }
    } else {
        // No root name
        if (rdir.len) {
            if (f == MPATH_POSIX) {
                // No root name, but a root directory on a POSIX path indicates an
                // absolute path
                return mstr_copy(path);
            }
            mstr abs_base = mpath_absolute(base, f);
            mstr_view base_rname = mpath_root_name(abs_base.view, f);
            mstr ret = mpath_join(base_rname, path, f);
            mstr_free(abs_base);
            return ret;
        } else {
            mstr abs_base = mpath_absolute(base, f);
            mstr r = mpath_join(abs_base.view, path, f);
            mstr_free(abs_base);
            return r;
        }
    }
}

static inline mstr mpath_absolute(mstr_view path, mpath_format f) {
    if (mpath_is_absolute(path, f)) {
        return mstr_copy(path);
    }
    mstr cur = mpath_current_path();
    mstr ret = mpath_absolute_from(path, cur.view, MPATH_NATIVE);
    mstr_assign(&ret, mpath_to_format(MPATH_NATIVE, ret.view, f));
    mstr_free(cur);
    return ret;
}

#endif // MONGOCRYPT_PATH_PRIVATE_H
