/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_filename --
 *	Build a file name in a scratch buffer, automatically calculate the
 *	length of the file name.
 */
int
__wt_filename(WT_SESSION_IMPL *session, const char *name, char **path)
{
	return (__wt_nfilename(session, name, strlen(name), path));
}

/*
 * __wt_nfilename --
 *	Build a file name in a scratch buffer.  If the name is already an
 *	absolute path duplicate it, otherwise generate a path relative to the
 *	connection home directory.
 */
int
__wt_nfilename(
    WT_SESSION_IMPL *session, const char *name, size_t namelen, char **path)
{
	WT_DECL_RET;
	size_t len;
	char *buf;

	*path = NULL;

	/*
	 * Needs to work with a NULL session handle - since this is called via
	 * the exists API which is used by the test utilities.
	 */
	if (session == NULL || __wt_absolute_path(name))
		return (__wt_strndup(session, name, namelen, path));

	len = strlen(S2C(session)->home) + 1 + namelen + 1;
	WT_RET(__wt_calloc(session, 1, len, &buf));
	WT_ERR(__wt_snprintf(buf, len, "%s%s%.*s",
	    S2C(session)->home, __wt_path_separator(), (int)namelen, name));
	*path = buf;
	return (0);

err:	__wt_free(session, buf);
	return (ret);
}

/*
 * __wt_filename_construct --
 *	Given unique identifiers, return a WT_ITEM of a generated file name of
 *	the given prefix type. Any identifier that is 0 will be skipped.
 */
int
__wt_filename_construct(WT_SESSION_IMPL *session, const char *path,
    const char *file_prefix, uintmax_t id_1, uint32_t id_2, WT_ITEM *buf)
{
	if (path != NULL && path[0] != '\0')
		WT_RET(__wt_buf_catfmt(
		    session, buf, "%s%s", path, __wt_path_separator()));
	WT_RET(__wt_buf_catfmt(session, buf, "%s", file_prefix));
	if (id_1 != UINTMAX_MAX)
		WT_RET(__wt_buf_catfmt(session, buf, ".%010" PRIuMAX, id_1));
	if (id_2 != UINT32_MAX)
		WT_RET(__wt_buf_catfmt(session, buf, ".%010" PRIu32, id_2));

	return (0);
}

/*
 * __wt_remove_if_exists --
 *	Remove a file if it exists.
 */
int
__wt_remove_if_exists(WT_SESSION_IMPL *session, const char *name, bool durable)
{
	bool exist;

	WT_RET(__wt_fs_exist(session, name, &exist));
	if (exist)
		WT_RET(__wt_fs_remove(session, name, durable));
	return (0);
}

/*
 * __wt_copy_and_sync --
 *	Copy a file safely.
 */
int
__wt_copy_and_sync(WT_SESSION *wt_session, const char *from, const char *to)
    WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_FH *ffh, *tfh;
	WT_SESSION_IMPL *session;
	wt_off_t n, offset, size;
	char *buf;

	session = (WT_SESSION_IMPL *)wt_session;
	ffh = tfh = NULL;
	buf = NULL;

	/*
	 * Remove the target file if it exists, then create a temporary file,
	 * copy the original into it and rename it into place. I don't think
	 * its necessary to remove the file, or create a copy and do a rename,
	 * it's likely safe to overwrite the backup file directly. I'm doing
	 * the remove and rename to insulate us from errors in other programs
	 * that might not detect a corrupted backup file; it's cheap insurance
	 * in a path where undetected failure is very bad.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_buf_fmt(session, tmp, "%s.copy", to));

	WT_ERR(__wt_remove_if_exists(session, to, false));
	WT_ERR(__wt_remove_if_exists(session, tmp->data, false));

	/* Open the from and temporary file handles. */
	WT_ERR(__wt_open(session, from, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, &ffh));
	WT_ERR(__wt_open(session, tmp->data, WT_FS_OPEN_FILE_TYPE_REGULAR,
	    WT_FS_OPEN_CREATE | WT_FS_OPEN_EXCLUSIVE, &tfh));

	/*
	 * Allocate a copy buffer. Don't use a scratch buffer, this thing is
	 * big, and we don't want it hanging around.
	 */
#define	WT_BACKUP_COPY_SIZE	(128 * 1024)
	WT_ERR(__wt_malloc(session, WT_BACKUP_COPY_SIZE, &buf));

	/* Get the file's size, then copy the bytes. */
	WT_ERR(__wt_filesize(session, ffh, &size));
	for (offset = 0; size > 0; size -= n, offset += n) {
		n = WT_MIN(size, WT_BACKUP_COPY_SIZE);
		WT_ERR(__wt_read(session, ffh, offset, (size_t)n, buf));
		WT_ERR(__wt_write(session, tfh, offset, (size_t)n, buf));
	}

	/* Close the from handle, then swap the temporary file into place. */
	WT_ERR(__wt_close(session, &ffh));
	WT_ERR(__wt_fsync(session, tfh, true));
	WT_ERR(__wt_close(session, &tfh));

	ret = __wt_fs_rename(session, tmp->data, to, true);

err:	WT_TRET(__wt_close(session, &ffh));
	WT_TRET(__wt_close(session, &tfh));

	__wt_free(session, buf);
	__wt_scr_free(session, &tmp);
	return (ret);
}
