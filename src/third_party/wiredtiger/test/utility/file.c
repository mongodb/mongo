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
#include "test_util.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <glob.h>
#include <libgen.h>
#include <unistd.h>
#include <utime.h>
#endif

typedef struct {
    const char *base_name;  /* The base name (the file name, without the path). */
    const char *start_path; /* The starting point of the traversal. */
    const char *rel_path;   /* The path relative to the start path. */

    bool directory; /* This is a directory. */
    int depth;      /* The depth we are at (0 = the source). */

    struct stat stat; /* File metadata. */
} file_info_t;
typedef void (*file_callback_t)(const char *, const file_info_t *, void *);

/*
 * process_directory_tree --
 *     Process a directory tree recursively. Fail the test on error.
 */
static void
process_directory_tree(const char *start_path, const char *rel_path, int depth, bool must_exist,
  file_callback_t on_file, file_callback_t on_directory_enter, file_callback_t on_directory_leave,
  void *user_data)
{
#ifdef _WIN32
    DWORD r;
    HANDLE h;
    WIN32_FIND_DATAA d;
    WT_DECL_RET;
    char file_ext[_MAX_EXT], file_name[_MAX_FNAME];
    char base_name[PATH_MAX], path[PATH_MAX], s[PATH_MAX], search[PATH_MAX];
    file_info_t info;

    /* Sanitize the base path. */
    if (start_path == NULL || start_path[0] == '\0')
        start_path = ".";

    memset(&info, 0, sizeof(info));
    info.depth = depth;
    info.rel_path = rel_path;
    info.start_path = start_path;

    /* Get the full path to the provided file or a directory. */
    if (rel_path == NULL || rel_path[0] == '\0')
        testutil_snprintf(path, sizeof(path), "%s", start_path);
    else
        testutil_snprintf(path, sizeof(path), "%s" DIR_DELIM_STR "%s", start_path, info.rel_path);

    /* Get just the base name. */
    testutil_check(
      _splitpath_s(start_path, NULL, 0, NULL, 0, file_name, _MAX_FNAME, file_ext, _MAX_EXT));
    testutil_snprintf(base_name, sizeof(base_name), "%s%s", file_name, file_ext);
    info.base_name = base_name;

    /* Check if the provided path exists and whether it points to a file. */
    ret = stat(path, &info.stat);
    if (ret != 0) {
        if (ret != ENOENT || must_exist)
            /*
             * Use a Windows-specific message function, because it often reveals more detail about
             * why the operation failed.
             */
            testutil_die(ret, "Error accessing %s: %s", path, last_windows_error_message());
        return;
    }

    if (!S_ISDIR(info.stat.st_mode)) {
        if (ret == 0 && on_file != NULL)
            on_file(path, &info, user_data);
        return;
    }

    /* It is a directory, so process it recursively. */
    testutil_snprintf(search, sizeof(search), "%s" DIR_DELIM_STR "*", path);
    h = FindFirstFileA(search, &d);
    if (h == INVALID_HANDLE_VALUE) {
        ret = __wt_map_windows_error(__wt_getlasterror());
        /*
         * If the directory has disappeared in the meantime due to a concurrent operation, it is
         * okay.
         */
        if (ret != ENOENT)
            testutil_die(ret, "Cannot list directory %s: %s", path, last_windows_error_message());
        return;
    }
    info.directory = true;

    /* Invoke the directory enter callback. */
    if (on_directory_enter != NULL)
        on_directory_enter(path, &info, user_data);

    for (;;) {
        /* Skip . and .. */
        if (strcmp(d.cFileName, ".") != 0 && strcmp(d.cFileName, "..") != 0) {
            if (rel_path == NULL || rel_path[0] == '\0')
                testutil_snprintf(s, sizeof(s), "%s", d.cFileName);
            else
                testutil_snprintf(s, sizeof(s), "%s" DIR_DELIM_STR "%s", rel_path, d.cFileName);
            process_directory_tree(start_path, s, depth + 1, must_exist, on_file,
              on_directory_enter, on_directory_leave, user_data);
        }

        if (FindNextFileA(h, &d) == 0) {
            r = __wt_getlasterror();
            if (r == ERROR_NO_MORE_FILES)
                break;
            ret = __wt_map_windows_error(__wt_getlasterror());
            testutil_die(ret, "Cannot list directory %s: %s", path, last_windows_error_message());
        }
    }

    if (FindClose(h) == 0) {
        ret = __wt_map_windows_error(__wt_getlasterror());
        testutil_die(
          ret, "Cannot close the directory list handle %s: %s", path, last_windows_error_message());
    }

    /* Invoke the directory leave callback. */
    if (on_directory_leave != NULL)
        on_directory_leave(path, &info, user_data);
#else
    struct dirent *dp;
    DIR *dirp;
    WT_DECL_RET;
    char buf[PATH_MAX], path[PATH_MAX], s[PATH_MAX];
    file_info_t info;

    /* Sanitize the base path. */
    if (start_path == NULL || start_path[0] == '\0')
        start_path = ".";

    memset(&info, 0, sizeof(info));
    info.depth = depth;
    info.rel_path = rel_path;
    info.start_path = start_path;

    /* Get the full path to the provided file or a directory. */
    if (rel_path == NULL || rel_path[0] == '\0')
        testutil_snprintf(path, sizeof(path), "%s", start_path);
    else
        testutil_snprintf(path, sizeof(path), "%s" DIR_DELIM_STR "%s", start_path, info.rel_path);

    /* Get just the base name. */
    testutil_snprintf(buf, sizeof(buf), "%s", path);
    info.base_name = basename(buf);

    /* Check if the provided path exists and whether it points to a file. */
    ret = stat(path, &info.stat);
    if (ret != 0) {
        if (ret != ENOENT || must_exist)
            testutil_assert_errno(ret);
        return;
    }

    if (!S_ISDIR(info.stat.st_mode)) {
        if (ret == 0 && on_file != NULL)
            on_file(path, &info, user_data);
        return;
    }

    /* It is a directory, so process it recursively. */
    dirp = opendir(path);
    testutil_assert_errno(dirp != NULL);
    info.directory = true;

    /* Invoke the directory enter callback. */
    if (on_directory_enter != NULL)
        on_directory_enter(path, &info, user_data);

    while ((dp = readdir(dirp)) != NULL) {
        testutil_assert(dp->d_name[0] != '\0');

        /* Skip . and .. */
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
            continue;

        if (rel_path == NULL || rel_path[0] == '\0')
            testutil_snprintf(s, sizeof(s), "%s", dp->d_name);
        else
            testutil_snprintf(s, sizeof(s), "%s" DIR_DELIM_STR "%s", rel_path, dp->d_name);
        process_directory_tree(start_path, s, depth + 1, must_exist, on_file, on_directory_enter,
          on_directory_leave, user_data);
    }

    testutil_assert_errno(closedir(dirp) == 0);

    /* Invoke the directory leave callback. */
    if (on_directory_leave != NULL)
        on_directory_leave(path, &info, user_data);
#endif
}

#define COPY_BUF_SIZE ((size_t)(256 * WT_KILOBYTE))

struct copy_data {
    const WT_FILE_COPY_OPTS *opts;
    const char *dest;
    bool dest_is_dir;
    int link_depth;
};

/*
 * copy_on_file --
 *     Worker for copying a file.
 */
static void
copy_on_file(const char *path, const file_info_t *info, void *user_data)
{
    struct copy_data *d;
#ifdef _WIN32
    struct _utimbuf t;
#else
#ifdef __linux__
    struct timeval times[2];
#else
    struct utimbuf t;
#endif
    WT_DECL_RET;
    ssize_t n;
    int rfd, wfd;
    char *buf;
    wt_off_t offset;
#endif
    char dest_path[PATH_MAX];

    d = (struct copy_data *)user_data;

    /* Don't copy special files, such as pipes. For now, we'll just silently ignore them. */
    if ((info->stat.st_mode & S_IFMT) != S_IFREG)
        return;

    /* Get path to the new file. If the relative path is NULL, it means we are copying a file. */
    if (info->rel_path == NULL) {
        if (d->dest_is_dir) {
            testutil_snprintf(
              dest_path, sizeof(dest_path), "%s" DIR_DELIM_STR "%s", d->dest, info->base_name);
        } else
            testutil_snprintf(dest_path, sizeof(dest_path), "%s", d->dest);
    } else
        testutil_snprintf(
          dest_path, sizeof(dest_path), "%s" DIR_DELIM_STR "%s", d->dest, info->rel_path);

    /* Check if we need to switch to using links. */
    if (d->opts->link && d->link_depth < 0)
        if (strncmp(d->opts->link_if_prefix, info->base_name, strlen(d->opts->link_if_prefix)) == 0)
            d->link_depth = info->depth;

#ifndef _WIN32
    if (d->link_depth >= 0 && info->depth >= d->link_depth) {
        /* Create a hard link instead of copying the file. */
        testutil_assert_errno(link(path, dest_path) == 0);
        return;
    }
#endif

    /* Copy the file. */
#ifdef _WIN32
    if (CopyFileA(path, dest_path, FALSE) == 0) /* This also preserves file attributes. */
        testutil_die(__wt_map_windows_error(__wt_getlasterror()), "Cannot copy %s to %s: %s", path,
          dest_path, last_windows_error_message());
#else
    testutil_assert_errno((rfd = open(path, O_RDONLY)) > 0);
    testutil_assert_errno(
      (wfd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, info->stat.st_mode)) > 0);

    buf = dmalloc(COPY_BUF_SIZE);
    for (offset = 0, n = 0;; offset += n) {
        WT_SYSCALL_RETRY((n = pread(rfd, buf, COPY_BUF_SIZE, offset)) < 0 ? -1 : 0, ret);
        testutil_check(ret);
        if (n == 0) {
            testutil_assert(offset >= info->stat.st_size);
            break;
        }
        testutil_assert_errno(write(wfd, buf, (size_t)n) == n);
    }
    testutil_assert_errno(close(rfd) == 0);
    testutil_assert_errno(close(wfd) == 0);
    free(buf);
#endif

    /* Preserve the timestamps. */
    if (d->opts->preserve) {
#if defined(_WIN32)
        t.actime = info->stat.st_atime;
        t.modtime = info->stat.st_mtime;
        testutil_assert_errno(_utime(dest_path, &t) == 0);
#elif defined(__linux__)
        times[0].tv_sec = info->stat.st_atim.tv_sec;
        times[0].tv_usec = info->stat.st_atim.tv_nsec / 1000;
        times[1].tv_sec = info->stat.st_mtim.tv_sec;
        times[1].tv_usec = info->stat.st_mtim.tv_nsec / 1000;
        testutil_assert_errno(utimes(dest_path, times) == 0);
#else
        t.actime = info->stat.st_atime;
        t.modtime = info->stat.st_mtime;
        testutil_assert_errno(utime(dest_path, &t) == 0);
#endif
    }
}

/*
 * copy_on_directory_enter --
 *     Worker for copying a directory.
 */
static void
copy_on_directory_enter(const char *path, const file_info_t *info, void *user_data)
{
    struct copy_data *d;
    char dest_path[PATH_MAX];

    WT_UNUSED(path);
    d = (struct copy_data *)user_data;

    /* No need to do anything for the top-level directory. This is handled elsewhere. */
    if (info->rel_path == NULL || strcmp(info->rel_path, ".") == 0)
        return;

    /* Check if we need to switch to using links. */
    if (d->opts->link && d->link_depth < 0)
        if (strncmp(d->opts->link_if_prefix, info->base_name, strlen(d->opts->link_if_prefix)) == 0)
            d->link_depth = info->depth;

    /* Otherwise we create a new directory. */
    testutil_snprintf(
      dest_path, sizeof(dest_path), "%s" DIR_DELIM_STR "%s", d->dest, info->rel_path);
    testutil_assert_errno(mkdir(dest_path, info->stat.st_mode) == 0);
}

/*
 * copy_on_directory_leave --
 *     Worker for copying a directory.
 */
static void
copy_on_directory_leave(const char *path, const file_info_t *info, void *user_data)
{
    struct copy_data *d;
#if defined(_WIN32)
    struct _utimbuf t;
#elif defined(__linux__)
    struct timeval times[2];
#else
    struct utimbuf t;
#endif
    char dest_path[PATH_MAX];

    WT_UNUSED(path);
    d = (struct copy_data *)user_data;

    if (info->depth <= d->link_depth)
        d->link_depth = -1;

    /* Preserve the timestamps. */
    if (d->opts->preserve) {
        testutil_snprintf(dest_path, sizeof(dest_path), "%s" DIR_DELIM_STR "%s", d->dest,
          info->rel_path == NULL ? "" : info->rel_path);
#if defined(_WIN32)
        t.actime = info->stat.st_atime;
        t.modtime = info->stat.st_mtime;
        testutil_assert_errno(_utime(dest_path, &t) == 0);
#elif defined(__linux__)
        times[0].tv_sec = info->stat.st_atim.tv_sec;
        times[0].tv_usec = info->stat.st_atim.tv_nsec / 1000;
        times[1].tv_sec = info->stat.st_mtim.tv_sec;
        times[1].tv_usec = info->stat.st_mtim.tv_nsec / 1000;
        testutil_assert_errno(utimes(dest_path, times) == 0);
#else
        t.actime = info->stat.st_atime;
        t.modtime = info->stat.st_mtime;
        testutil_assert_errno(utime(dest_path, &t) == 0);
#endif
    }
}

/*
 * testutil_copy --
 *     Recursively copy a file or a directory tree. Fail the test on error.
 */
void
testutil_copy(const char *source, const char *dest)
{
    testutil_copy_ext(source, dest, NULL);
}

/*
 * testutil_move --
 *     Move a file or folder.
 */
void
testutil_move(const char *source, const char *dest)
{
    testutil_remove(dest);
    testutil_copy(source, dest);
    testutil_remove(source);
}

/* Default options for the file copy function. */
static const WT_FILE_COPY_OPTS default_copy_opts = {0};

/*
 * testutil_copy_ext --
 *     Recursively copy a file or a directory tree. Fail the test on error. With extra options.
 */
void
testutil_copy_ext(const char *source, const char *dest, const WT_FILE_COPY_OPTS *opts)
{
    struct copy_data data;
    struct stat source_stat, dest_stat;
    WT_DECL_RET;
    const char *path;
    bool dest_exists;
    bool is_dest_dir;
    bool is_source_dir;
    glob_t g;
    size_t i;

    if (opts == NULL)
        opts = &default_copy_opts;

    memset(&data, 0, sizeof(data));
    data.dest = dest;
    data.link_depth = opts->link && opts->link_if_prefix == NULL ? 0 : -1;
    data.opts = opts;

    memset(&g, 0, sizeof(g));
    testutil_check_error_ok(glob(source, 0, NULL, &g), GLOB_NOMATCH);

    for (i = 0; i != g.gl_pathc; i++) {
        path = g.gl_pathv[i];

        /* Check the source. */
        testutil_assertfmt((ret = stat(path, &source_stat)) == 0, "Failed to stat \"%s\": %s", path,
          strerror(errno));
        is_source_dir = S_ISDIR(source_stat.st_mode);

        /* Check the destination. */
        ret = stat(dest, &dest_stat);
        testutil_assert_errno(ret == 0 || errno == ENOENT);
        dest_exists = ret == 0;
        is_dest_dir = dest_exists ? S_ISDIR(dest_stat.st_mode) : false;
        data.dest_is_dir = is_dest_dir;

        /* If we are copying a directory, make sure that we are not copying over a file. */
        testutil_assert(!(is_source_dir && dest_exists && !is_dest_dir));

        /* If we are copying a directory to another directory that doesn't exist, create it. */
        if (is_source_dir && !dest_exists)
            testutil_assert_errno(mkdir(dest, source_stat.st_mode) == 0);

        process_directory_tree(path, NULL, 0, true, copy_on_file, copy_on_directory_enter,
          copy_on_directory_leave, &data);
    }

    globfree(&g);
}

/*
 * testutil_mkdir --
 *     Create a directory. Fail the test on error.
 */
void
testutil_mkdir(const char *path)
{
    testutil_assertfmt(
      mkdir(path, 0777) == 0, "Cannot create directory %s: %s", path, strerror(errno));
}

/* Default options for directory creation. */
static const WT_MKDIR_OPTS default_mkdir_opts = {0};

/*
 * testutil_mkdir_ext --
 *     Create a directory, with extra options. Fail the test on error.
 */
void
testutil_mkdir_ext(const char *path, const WT_MKDIR_OPTS *opts)
{
    WT_DECL_RET;
    char *buf, *parent;
#ifdef _WIN32
    char dir[_MAX_DIR], drive[_MAX_DRIVE], p[_MAX_DIR + _MAX_DRIVE];
    int i;
#endif

    if (path[0] == '\0')
        testutil_die(EINVAL, "Cannot create directory");

    if (opts == NULL)
        opts = &default_mkdir_opts;

    for (;;) {
        ret = mkdir(path, 0755);
        if (ret == 0)
            return;
        if (opts->can_exist && errno == EEXIST)
            return;

        /* Create the parent directory. */
        if (opts->parents && errno == ENOENT) {
            /* Get the parent directory. */
            buf = dstrdup(path);
#ifdef _WIN32
            /* Remove trailing separators - note that we need to deal with both '\\' and '/'! */
            i = ((int)strlen(buf)) - 1;
            while (i >= 0 && (buf[i] == '/' || buf[i] == '\\'))
                buf[i--] = '\0';
            testutil_check(_splitpath_s(buf, drive, _MAX_DRIVE, dir, _MAX_DIR, NULL, 0, NULL, 0));
            testutil_snprintf(p, sizeof(p), "%s%s", drive, dir);

            /* Fail if we reached the top, e.g., if we the drive does not exist. */
            if (strcmp(dir, "") == 0 || strcmp(dir, ".") == 0 || strcmp(dir, "/") == 0 ||
              strcmp(dir, "\\") == 0)
                testutil_die(EINVAL, "Cannot create directory %s", path);
            parent = p;
#else
            parent = dirname(buf);
            testutil_assert(strcmp(parent, "") != 0);

            /* Fail if we reached the top - this should never happen. */
            if (strcmp(parent, ".") == 0 || strcmp(parent, "/") == 0)
                testutil_die(EINVAL, "Cannot create directory %s", path);
#endif

            /* Create the parent recursively. */
            testutil_mkdir_ext(parent, opts);
            free(buf);

            /* Try again. */
            continue;
        }

        testutil_die(errno, "Cannot create directory %s", path);
    }
}

/*
 * testutil_recreate_dir --
 *     Delete the existing directory, then create a new one.
 */
void
testutil_recreate_dir(const char *dir)
{
    testutil_remove(dir);
    testutil_mkdir(dir);
}

/*
 * remove_on_file --
 *     Worker for removing a file.
 */
static void
remove_on_file(const char *path, const file_info_t *info, void *user_data)
{
    WT_DECL_RET;

    WT_UNUSED(info);
    WT_UNUSED(user_data);

    ret = unlink(path);
    testutil_assertfmt(ret == 0 || errno == ENOENT, "Cannot remove %s: %s", path, strerror(errno));
}

/*
 * remove_on_directory_leave --
 *     Worker for removing a directory.
 */
static void
remove_on_directory_leave(const char *path, const file_info_t *info, void *user_data)
{
    WT_DECL_RET;

    WT_UNUSED(info);
    WT_UNUSED(user_data);

    ret = rmdir(path);
    testutil_assertfmt(ret == 0 || errno == ENOENT, "Cannot remove %s: %s", path, strerror(errno));
}

/*
 * testutil_remove --
 *     Recursively remove a file or a directory tree. Fail the test on error.
 */
void
testutil_remove(const char *path)
{
    glob_t g;
    size_t i;

    memset(&g, 0, sizeof(g));
    testutil_check_error_ok(glob(path, 0, NULL, &g), GLOB_NOMATCH);

    for (i = 0; i != g.gl_pathc; i++)
        process_directory_tree(
          g.gl_pathv[i], NULL, 0, true, remove_on_file, NULL, remove_on_directory_leave, NULL);

    globfree(&g);
}

/*
 * testutil_exists --
 *     Check whether a file exists. The function takes both a directory and a file argument, because
 *     it is often used to check whether a file exists in a different directory. This saves the
 *     caller an unnecessary snprintf.
 */
bool
testutil_exists(const char *dir, const char *file)
{
    struct stat sb;
    char path[PATH_MAX];

    if (dir == NULL)
        testutil_snprintf(path, sizeof(path), "%s", file);
    else
        testutil_snprintf(path, sizeof(path), "%s" DIR_DELIM_STR "%s", dir, file);

    if (stat(path, &sb) == 0)
        return (true);
    else {
        /* If stat failed, make sure that it is because the file does not exist. */
        testutil_assert(errno == ENOENT);
        return (false);
    }
}

/*
 * testutil_sentinel --
 *     Create an empty "sentinel" file to indicate that something has happened. For example, this
 *     can be used to indicate that a checkpoint or a backup completed.
 */
void
testutil_sentinel(const char *dir, const char *file)
{
    FILE *fp;
    char path[PATH_MAX];

    if (dir == NULL)
        testutil_snprintf(path, sizeof(path), "%s", file);
    else
        testutil_snprintf(path, sizeof(path), "%s" DIR_DELIM_STR "%s", dir, file);

    testutil_assert_errno((fp = fopen(path, "w")) != NULL);
    testutil_assert_errno(fclose(fp) == 0);
}
