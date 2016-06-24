/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * File system interface for in-memory implementation.
 */
typedef struct {
	WT_FILE_SYSTEM iface;

	TAILQ_HEAD(__wt_fhhash_inmem,
	    __wt_file_handle_inmem) fhhash[WT_HASH_ARRAY_SIZE];
	TAILQ_HEAD(__wt_fh_inmem_qh, __wt_file_handle_inmem) fhqh;

	WT_SPINLOCK lock;
} WT_FILE_SYSTEM_INMEM;

static int __im_file_size(WT_FILE_HANDLE *, WT_SESSION *, wt_off_t *);

/*
 * __im_handle_search --
 *	Return a matching handle, if one exists.
 */
static WT_FILE_HANDLE_INMEM *
__im_handle_search(WT_FILE_SYSTEM *file_system, const char *name)
{
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	uint64_t bucket, hash;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;

	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	TAILQ_FOREACH(im_fh, &im_fs->fhhash[bucket], hashq)
		if (strcmp(im_fh->iface.name, name) == 0)
			break;

	return (im_fh);
}

/*
 * __im_handle_remove --
 *	Destroy an in-memory file handle. Should only happen on remove or
 *	shutdown.
 */
static int
__im_handle_remove(WT_SESSION_IMPL *session,
    WT_FILE_SYSTEM *file_system, WT_FILE_HANDLE_INMEM *im_fh)
{
	WT_FILE_HANDLE *fhp;
	WT_FILE_SYSTEM_INMEM *im_fs;
	uint64_t bucket;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;

	if (im_fh->ref != 0)
		WT_RET_MSG(session, EBUSY,
		    "%s: file-remove", im_fh->iface.name);

	bucket = im_fh->name_hash % WT_HASH_ARRAY_SIZE;
	WT_FILE_HANDLE_REMOVE(im_fs, im_fh, bucket);

	/* Clean up private information. */
	__wt_buf_free(session, &im_fh->buf);

	/* Clean up public information. */
	fhp = (WT_FILE_HANDLE *)im_fh;
	__wt_free(session, fhp->name);

	__wt_free(session, im_fh);

	return (0);
}

/*
 * __im_fs_directory_list --
 *	Return the directory contents.
 */
static int
__im_fs_directory_list(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *directory,
    const char *prefix, char ***dirlistp, uint32_t *countp)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	size_t dirallocsz, len;
	uint32_t count;
	char *name, **entries;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	*dirlistp = NULL;
	*countp = 0;

	dirallocsz = 0;
	len = strlen(directory);
	entries = NULL;

	__wt_spin_lock(session, &im_fs->lock);

	count = 0;
	TAILQ_FOREACH(im_fh, &im_fs->fhqh, q) {
		name = im_fh->iface.name;
		if (strncmp(name, directory, len) != 0 ||
		    (prefix != NULL && !WT_PREFIX_MATCH(name + len, prefix)))
			continue;

		WT_ERR(__wt_realloc_def(
		    session, &dirallocsz, count + 1, &entries));
		WT_ERR(__wt_strdup(session, name, &entries[count]));
		++count;
	}

	*dirlistp = entries;
	*countp = count;

err:	__wt_spin_unlock(session, &im_fs->lock);
	if (ret == 0)
		return (0);

	if (entries != NULL) {
		while (count > 0)
			__wt_free(session, entries[--count]);
		__wt_free(session, entries);
	}

	WT_RET_MSG(session, ret,
	    "%s: directory-list, prefix \"%s\"",
	    directory, prefix == NULL ? "" : prefix);
}

/*
 * __im_fs_directory_list_free --
 *	Free memory returned by __im_fs_directory_list.
 */
static int
__im_fs_directory_list_free(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, char **dirlist, uint32_t count)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	if (dirlist != NULL) {
		while (count > 0)
			__wt_free(session, dirlist[--count]);
		__wt_free(session, dirlist);
	}
	return (0);
}

/*
 * __im_fs_exist --
 *	Return if the file exists.
 */
static int
__im_fs_exist(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *name, bool *existp)
{
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	*existp = __im_handle_search(file_system, name) != NULL;

	__wt_spin_unlock(session, &im_fs->lock);
	return (0);
}

/*
 * __im_fs_remove --
 *	POSIX remove.
 */
static int
__im_fs_remove(
    WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session, const char *name)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	ret = ENOENT;
	if ((im_fh = __im_handle_search(file_system, name)) != NULL)
		ret = __im_handle_remove(session, file_system, im_fh);

	__wt_spin_unlock(session, &im_fs->lock);
	return (ret);
}

/*
 * __im_fs_rename --
 *	POSIX rename.
 */
static int
__im_fs_rename(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *from, const char *to)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	uint64_t bucket;
	char *copy;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	ret = ENOENT;
	if ((im_fh = __im_handle_search(file_system, from)) != NULL) {
		WT_ERR(__wt_strdup(session, to, &copy));
		__wt_free(session, im_fh->iface.name);
		im_fh->iface.name = copy;

		bucket = im_fh->name_hash % WT_HASH_ARRAY_SIZE;
		WT_FILE_HANDLE_REMOVE(im_fs, im_fh, bucket);
		im_fh->name_hash = __wt_hash_city64(to, strlen(to));
		bucket = im_fh->name_hash % WT_HASH_ARRAY_SIZE;
		WT_FILE_HANDLE_INSERT(im_fs, im_fh, bucket);
	}

err:	__wt_spin_unlock(session, &im_fs->lock);
	return (ret);
}

/*
 * __im_fs_size --
 *	Get the size of a file in bytes, by file name.
 */
static int
__im_fs_size(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *name, wt_off_t *sizep)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	/* Search for the handle, then get its size. */
	if ((im_fh = __im_handle_search(file_system, name)) == NULL)
		ret = ENOENT;
	else
		*sizep = (wt_off_t)im_fh->buf.size;

	__wt_spin_unlock(session, &im_fs->lock);

	return (ret);
}

/*
 * __im_file_close --
 *	ANSI C close.
 */
static int
__im_file_close(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	im_fh = (WT_FILE_HANDLE_INMEM *)file_handle;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_handle->file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	--im_fh->ref;

	__wt_spin_unlock(session, &im_fs->lock);

	return (0);
}

/*
 * __im_file_lock --
 *	Lock/unlock a file.
 */
static int
__im_file_lock(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, bool lock)
{
	WT_UNUSED(file_handle);
	WT_UNUSED(wt_session);
	WT_UNUSED(lock);
	return (0);
}

/*
 * __im_file_read --
 *	POSIX pread.
 */
static int
__im_file_read(WT_FILE_HANDLE *file_handle,
    WT_SESSION *wt_session, wt_off_t offset, size_t len, void *buf)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	size_t off;

	im_fh = (WT_FILE_HANDLE_INMEM *)file_handle;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_handle->file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	off = (size_t)offset;
	if (off < im_fh->buf.size) {
		len = WT_MIN(len, im_fh->buf.size - off);
		memcpy(buf, (uint8_t *)im_fh->buf.mem + off, len);
	} else
		ret = WT_ERROR;

	__wt_spin_unlock(session, &im_fs->lock);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, WT_ERROR,
	    "%s: handle-read: failed to read %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    file_handle->name, len, off);
}

/*
 * __im_file_size --
 *	Get the size of a file in bytes, by file handle.
 */
static int
__im_file_size(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t *sizep)
{
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	im_fh = (WT_FILE_HANDLE_INMEM *)file_handle;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_handle->file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	*sizep = (wt_off_t)im_fh->buf.size;

	__wt_spin_unlock(session, &im_fs->lock);

	return (0);
}

/*
 * __im_file_sync --
 *	In-memory sync.
 */
static int
__im_file_sync(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session)
{
	WT_UNUSED(file_handle);
	WT_UNUSED(wt_session);
	return (0);
}

/*
 * __im_file_truncate --
 *	POSIX ftruncate.
 */
static int
__im_file_truncate(
    WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session, wt_off_t offset)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	size_t off;

	im_fh = (WT_FILE_HANDLE_INMEM *)file_handle;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_handle->file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	/*
	 * Grow the buffer as necessary, clear any new space in the file, and
	 * reset the file's data length.
	 */
	off = (size_t)offset;
	WT_ERR(__wt_buf_grow(session, &im_fh->buf, off));
	if (im_fh->buf.size < off)
		memset((uint8_t *)im_fh->buf.data + im_fh->buf.size,
		    0, off - im_fh->buf.size);
	im_fh->buf.size = off;

err:	__wt_spin_unlock(session, &im_fs->lock);
	return (ret);
}

/*
 * __im_file_write --
 *	POSIX pwrite.
 */
static int
__im_file_write(WT_FILE_HANDLE *file_handle, WT_SESSION *wt_session,
    wt_off_t offset, size_t len, const void *buf)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	size_t off;

	im_fh = (WT_FILE_HANDLE_INMEM *)file_handle;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_handle->file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	off = (size_t)offset;
	WT_ERR(__wt_buf_grow(session, &im_fh->buf, off + len + 1024));

	memcpy((uint8_t *)im_fh->buf.data + off, buf, len);
	if (off + len > im_fh->buf.size)
		im_fh->buf.size = off + len;

err:	__wt_spin_unlock(session, &im_fs->lock);
	if (ret == 0)
		return (0);
	WT_RET_MSG(session, ret,
	    "%s: handle-write: failed to write %" WT_SIZET_FMT " bytes at "
	    "offset %" WT_SIZET_FMT,
	    file_handle->name, len, off);
}

/*
 * __im_file_open --
 *	POSIX fopen/open.
 */
static int
__im_file_open(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session,
    const char *name, WT_OPEN_FILE_TYPE file_type, uint32_t flags,
    WT_FILE_HANDLE **file_handlep)
{
	WT_DECL_RET;
	WT_FILE_HANDLE *file_handle;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;
	uint64_t bucket, hash;

	WT_UNUSED(file_type);
	WT_UNUSED(flags);

	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;
	session = (WT_SESSION_IMPL *)wt_session;

	__wt_spin_lock(session, &im_fs->lock);

	/*
	 * First search the file queue, if we find it, assert there's only a
	 * single reference, in-memory only supports a single handle on any
	 * file, for now.
	 */
	im_fh = __im_handle_search(file_system, name);
	if (im_fh != NULL) {

		if (im_fh->ref != 0)
			WT_ERR_MSG(session, EBUSY,
			    "%s: file-open: already open", name);

		im_fh->ref = 1;

		*file_handlep = (WT_FILE_HANDLE *)im_fh;

		__wt_spin_unlock(session, &im_fs->lock);
		return (0);
	}

	/* The file hasn't been opened before, create a new one. */
	WT_ERR(__wt_calloc_one(session, &im_fh));

	/* Initialize public information. */
	file_handle = (WT_FILE_HANDLE *)im_fh;
	file_handle->file_system = file_system;
	WT_ERR(__wt_strdup(session, name, &file_handle->name));

	/* Initialize private information. */
	im_fh->ref = 1;

	hash = __wt_hash_city64(name, strlen(name));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	im_fh->name_hash = hash;
	WT_FILE_HANDLE_INSERT(im_fs, im_fh, bucket);

	file_handle->close = __im_file_close;
	file_handle->fh_lock = __im_file_lock;
	file_handle->fh_read = __im_file_read;
	file_handle->fh_size = __im_file_size;
	file_handle->fh_sync = __im_file_sync;
	file_handle->fh_truncate = __im_file_truncate;
	file_handle->fh_write = __im_file_write;

	*file_handlep = file_handle;

	if (0) {
err:		__wt_free(session, im_fh);
	}

	__wt_spin_unlock(session, &im_fs->lock);
	return (ret);
}

/*
 * __im_terminate --
 *	Terminate an in-memory configuration.
 */
static int
__im_terminate(WT_FILE_SYSTEM *file_system, WT_SESSION *wt_session)
{
	WT_DECL_RET;
	WT_FILE_HANDLE_INMEM *im_fh;
	WT_FILE_SYSTEM_INMEM *im_fs;
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;
	im_fs = (WT_FILE_SYSTEM_INMEM *)file_system;

	while ((im_fh = TAILQ_FIRST(&im_fs->fhqh)) != NULL)
		WT_TRET(__im_handle_remove(session, file_system, im_fh));

	__wt_spin_destroy(session, &im_fs->lock);
	__wt_free(session, im_fs);

	return (ret);
}

/*
 * __wt_os_inmemory --
 *	Initialize an in-memory configuration.
 */
int
__wt_os_inmemory(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_FILE_SYSTEM *file_system;
	WT_FILE_SYSTEM_INMEM *im_fs;
	u_int i;

	WT_RET(__wt_calloc_one(session, &im_fs));

	/* Initialize private information. */
	TAILQ_INIT(&im_fs->fhqh);
	for (i = 0; i < WT_HASH_ARRAY_SIZE; i++)
		TAILQ_INIT(&im_fs->fhhash[i]);

	WT_ERR(__wt_spin_init(session, &im_fs->lock, "in-memory I/O"));

	/* Initialize the in-memory jump table. */
	file_system = (WT_FILE_SYSTEM *)im_fs;
	file_system->fs_directory_list = __im_fs_directory_list;
	file_system->fs_directory_list_free = __im_fs_directory_list_free;
	file_system->fs_exist = __im_fs_exist;
	file_system->fs_open_file = __im_file_open;
	file_system->fs_remove = __im_fs_remove;
	file_system->fs_rename = __im_fs_rename;
	file_system->fs_size = __im_fs_size;
	file_system->terminate = __im_terminate;

	/* Switch the file system into place. */
	S2C(session)->file_system = (WT_FILE_SYSTEM *)im_fs;

	return (0);

err:	__wt_free(session, im_fs);
	return (ret);
}
