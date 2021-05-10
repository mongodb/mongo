/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_fs_file_system --
 *     Get the active file system handle.
 */
static inline WT_FILE_SYSTEM *
__wt_fs_file_system(WT_SESSION_IMPL *session)
{
    return (S2FS(session));
}

/*
 * __wt_fs_directory_list --
 *     Return a list of files from a directory.
 */
static inline int
__wt_fs_directory_list(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *path;

    *dirlistp = NULL;
    *countp = 0;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: directory-list: prefix %s", dir,
      prefix == NULL ? "all" : prefix);

    WT_RET(__wt_filename(session, dir, &path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_directory_list(file_system, wt_session, path, prefix, dirlistp, countp);

    __wt_free(session, path);
    return (ret);
}

/*
 * __wt_fs_directory_list_single --
 *     Return a single matching file from a directory.
 */
static inline int
__wt_fs_directory_list_single(
  WT_SESSION_IMPL *session, const char *dir, const char *prefix, char ***dirlistp, u_int *countp)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *path;

    *dirlistp = NULL;
    *countp = 0;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: directory-list-single: prefix %s", dir,
      prefix == NULL ? "all" : prefix);

    WT_RET(__wt_filename(session, dir, &path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_directory_list_single(
      file_system, wt_session, path, prefix, dirlistp, countp);

    __wt_free(session, path);
    return (ret);
}

/*
 * __wt_fs_directory_list_free --
 *     Free memory allocated by __wt_fs_directory_list.
 */
static inline int
__wt_fs_directory_list_free(WT_SESSION_IMPL *session, char ***dirlistp, u_int count)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;

    if (*dirlistp != NULL) {
        file_system = __wt_fs_file_system(session);
        wt_session = (WT_SESSION *)session;
        ret = file_system->fs_directory_list_free(file_system, wt_session, *dirlistp, count);
    }

    *dirlistp = NULL;
    return (ret);
}

/*
 * __wt_fs_exist --
 *     Return if the file exists.
 */
static inline int
__wt_fs_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *path;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: file-exist", name);

    WT_RET(__wt_filename(session, name, &path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_exist(file_system, wt_session, path, existp);

    __wt_free(session, path);
    return (ret);
}

/*
 * __wt_fs_remove --
 *     Remove the file.
 */
static inline int
__wt_fs_remove(WT_SESSION_IMPL *session, const char *name, bool durable)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *path;

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: file-remove", name);

#ifdef HAVE_DIAGNOSTIC
    /*
     * It is a layering violation to retrieve a WT_FH here, but it is a useful diagnostic to ensure
     * WiredTiger doesn't have the handle open.
     */
    if (__wt_handle_is_open(session, name))
        WT_RET_MSG(session, EINVAL, "%s: file-remove: file has open handles", name);
#endif

    WT_RET(__wt_filename(session, name, &path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_remove(file_system, wt_session, path, durable ? WT_FS_DURABLE : 0);

    __wt_free(session, path);
    return (ret);
}

/*
 * __wt_fs_rename --
 *     Rename the file.
 */
static inline int
__wt_fs_rename(WT_SESSION_IMPL *session, const char *from, const char *to, bool durable)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *from_path, *to_path;

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

    __wt_verbose(session, WT_VERB_FILEOPS, "%s to %s: file-rename", from, to);

#ifdef HAVE_DIAGNOSTIC
    /*
     * It is a layering violation to retrieve a WT_FH here, but it is a useful diagnostic to ensure
     * WiredTiger doesn't have the handle open.
     */
    if (__wt_handle_is_open(session, from))
        WT_RET_MSG(session, EINVAL, "%s: file-rename: file has open handles", from);
    if (__wt_handle_is_open(session, to))
        WT_RET_MSG(session, EINVAL, "%s: file-rename: file has open handles", to);
#endif

    from_path = to_path = NULL;
    WT_ERR(__wt_filename(session, from, &from_path));
    WT_ERR(__wt_filename(session, to, &to_path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_rename(
      file_system, wt_session, from_path, to_path, durable ? WT_FS_DURABLE : 0);

err:
    __wt_free(session, from_path);
    __wt_free(session, to_path);
    return (ret);
}

/*
 * __wt_fs_size --
 *     Return the size of a file in bytes, by file name.
 */
static inline int
__wt_fs_size(WT_SESSION_IMPL *session, const char *name, wt_off_t *sizep)
{
    WT_DECL_RET;
    WT_FILE_SYSTEM *file_system;
    WT_SESSION *wt_session;
    char *path;

    __wt_verbose(session, WT_VERB_FILEOPS, "%s: file-size", name);

    WT_RET(__wt_filename(session, name, &path));

    file_system = __wt_fs_file_system(session);
    wt_session = (WT_SESSION *)session;
    ret = file_system->fs_size(file_system, wt_session, path, sizep);

    __wt_free(session, path);
    return (ret);
}
