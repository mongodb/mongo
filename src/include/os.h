/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Number of directory entries can grow dynamically.
 */
#define	WT_DIR_ENTRY	32

#define	WT_DIRLIST_EXCLUDE	0x1	/* Exclude files matching prefix */
#define	WT_DIRLIST_INCLUDE	0x2	/* Include files matching prefix */

#define	WT_SYSCALL_RETRY(call, ret) do {				\
	int __retry;							\
	for (__retry = 0; __retry < 10; ++__retry) {			\
		if ((call) == 0) {					\
			(ret) = 0;					\
			break;						\
		}							\
		switch ((ret) = __wt_errno()) {				\
		case 0:							\
			/* The call failed but didn't set errno. */	\
			(ret) = WT_ERROR;				\
			break;						\
		case EAGAIN:						\
		case EBUSY:						\
		case EINTR:						\
		case EIO:						\
		case EMFILE:						\
		case ENFILE:						\
		case ENOSPC:						\
			__wt_sleep(0L, 50000L);				\
			continue;					\
		default:						\
			break;						\
		}							\
		break;							\
	}								\
} while (0)

#define	WT_TIMEDIFF_NS(end, begin)					\
	(WT_BILLION * (uint64_t)((end).tv_sec - (begin).tv_sec) +	\
	    (uint64_t)(end).tv_nsec - (uint64_t)(begin).tv_nsec)
#define	WT_TIMEDIFF_US(end, begin)					\
	(WT_TIMEDIFF_NS((end), (begin)) / WT_THOUSAND)
#define	WT_TIMEDIFF_MS(end, begin)					\
	(WT_TIMEDIFF_NS((end), (begin)) / WT_MILLION)
#define	WT_TIMEDIFF_SEC(end, begin)					\
	(WT_TIMEDIFF_NS((end), (begin)) / WT_BILLION)

#define	WT_TIMECMP(t1, t2)						\
	((t1).tv_sec < (t2).tv_sec ? -1 :				\
	     (t1).tv_sec == (t2.tv_sec) ?				\
	     (t1).tv_nsec < (t2).tv_nsec ? -1 :				\
	     (t1).tv_nsec == (t2).tv_nsec ? 0 : 1 : 1)

#define	WT_OPEN_CREATE		0x001	/* Create is OK */
#define	WT_OPEN_EXCLUSIVE	0x002	/* Exclusive open */
#define	WT_OPEN_FIXED		0x004	/* Path isn't relative to home */
#define	WT_OPEN_READONLY	0x008	/* Readonly open */
#define	WT_STREAM_APPEND	0x010	/* Open a stream: append */
#define	WT_STREAM_READ		0x020	/* Open a stream: read */
#define	WT_STREAM_WRITE		0x040	/* Open a stream: write */

#define	WT_STDERR	((void *)0x1)	/* WT_FH to stderr */
#define	WT_STDOUT	((void *)0x2)	/* WT_FH to stdout */

struct __wt_fh {
	char	*name;				/* File name */
	uint64_t name_hash;			/* Hash of name */
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */
	TAILQ_ENTRY(__wt_fh) hashq;		/* Hashed list of handles */

	u_int	ref;				/* Reference count */

	/*
	 * Underlying file system handle support.
	 */
	FILE	*fp;				/* ANSI C file handle */
#ifndef _WIN32
	int	 fd;				/* POSIX file handle */
#else
	HANDLE filehandle;			/* Windows file handle */
	HANDLE filehandle_secondary;		/* Windows file handle
						   for file size changes */
#endif
	wt_off_t size;				/* File size */
	wt_off_t extend_size;			/* File extended size */
	wt_off_t extend_len;			/* File extend chunk size */

	/*
	 * Underlying in-memory handle support.
	 */
	size_t	 off;				/* Read/write offset */
	WT_ITEM  buf;				/* Data */

	bool	 direct_io;			/* O_DIRECT configured */

	enum {					/* file extend configuration */
	    WT_FALLOCATE_AVAILABLE,
	    WT_FALLOCATE_NOT_AVAILABLE,
	    WT_FALLOCATE_POSIX,
	    WT_FALLOCATE_STD,
	    WT_FALLOCATE_SYS } fallocate_available;
	bool fallocate_requires_locking;

#define	WT_FH_IN_MEMORY		0x01		/* In-memory, don't remove */
#define	WT_FH_FLUSH_ON_CLOSE	0x02		/* Flush when closing */
	uint32_t flags;
};

/*
 * OS calls that are currently just stubs.
 */
/*
 * __wt_directory_sync --
 *	Flush a directory to ensure file creation is durable.
 */
static inline int
__wt_directory_sync(WT_SESSION_IMPL *session, const char *path)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (WT_JUMP(j_directory_sync, session, path));
}

/*
 * __wt_directory_sync_fh --
 *	Flush a directory file handle to ensure file creation is durable.
 *
 * We don't use the normal sync path because many file systems don't require
 * this step and we don't want to penalize them.
 */
static inline int
__wt_directory_sync_fh(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

#ifdef __linux__
	return (WT_JUMP(j_handle_sync, session, fh, true));
#else
	WT_UNUSED(fh);
	return (0);
#endif
}

/*
 * __wt_exist --
 *	Return if the file exists.
 */
static inline int
__wt_exist(WT_SESSION_IMPL *session, const char *name, bool *existp)
{
	return (WT_JUMP(j_file_exist, session, name, existp));
}

/*
 * __wt_posix_fadvise --
 *	POSIX fadvise.
 */
static inline int
__wt_posix_fadvise(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, wt_off_t len, int advice)
{
#if defined(HAVE_POSIX_FADVISE)
	return (WT_JUMP(j_handle_advise, session, fh, offset, len, advice));
#else
	return (0);
#endif
}

/*
 * __wt_file_lock --
 *	Lock/unlock a file.
 */
static inline int
__wt_file_lock(WT_SESSION_IMPL * session, WT_FH *fh, bool lock)
{
	return (WT_JUMP(j_handle_lock, session, fh, lock));
}

/*
 * __wt_filesize --
 *	Get the size of a file in bytes, by file handle.
 */
static inline int
__wt_filesize(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t *sizep)
{
	return (WT_JUMP(j_handle_size, session, fh, sizep));
}

/*
 * __wt_filesize_name --
 *	Get the size of a file in bytes, by file name.
 */
static inline int
__wt_filesize_name(
    WT_SESSION_IMPL *session, const char *name, bool silent, wt_off_t *sizep)
{
	return (WT_JUMP(j_file_size, session, name, silent, sizep));
}

/*
 * __wt_fsync --
 *	POSIX fflush/fsync.
 */
static inline int
__wt_fsync(WT_SESSION_IMPL *session, void *fh, bool block)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (WT_JUMP(j_handle_sync, session, fh, block));
}

/*
 * __wt_ftruncate --
 *	POSIX ftruncate.
 */
static inline int
__wt_ftruncate(WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t len)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (WT_JUMP(j_handle_truncate, session, fh, len));
}

/*
 * __wt_read --
 *	POSIX pread.
 */
static inline int
__wt_read(
    WT_SESSION_IMPL *session, WT_FH *fh, wt_off_t offset, size_t len, void *buf)
{
	WT_STAT_FAST_CONN_INCR(session, read_io);

	return (WT_JUMP(j_handle_read, session, fh, offset, len, buf));
}

/*
 * __wt_remove --
 *	POSIX remove.
 */
static inline int
__wt_remove(WT_SESSION_IMPL *session, const char *name)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (WT_JUMP(j_file_remove, session, name));
}

/*
 * __wt_rename --
 *	POSIX rename.
 */
static inline int
__wt_rename(WT_SESSION_IMPL *session, const char *from, const char *to)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY));

	return (WT_JUMP(j_file_rename, session, from, to));
}

/*
 * __wt_write --
 *	POSIX pwrite.
 */
static inline int
__wt_write(WT_SESSION_IMPL *session,
    WT_FH *fh, wt_off_t offset, size_t len, const void *buf)
{
	WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_READONLY) ||
	    WT_STRING_MATCH(fh->name,
	    WT_SINGLETHREAD, strlen(WT_SINGLETHREAD)));

	WT_STAT_FAST_CONN_INCR(session, write_io);

	return (WT_JUMP(j_handle_write, session, fh, offset, len, buf));
}

/*
 * __wt_vfprintf --
 *	ANSI C vfprintf.
 */
static inline int
__wt_vfprintf(WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, va_list ap)
{
	return (WT_JUMP(j_handle_printf, session, fh, fmt, ap));
}

/*
 * __wt_fprintf --
 *	ANSI C fprintf.
 */
static inline int
__wt_fprintf(WT_SESSION_IMPL *session, WT_FH *fh, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vfprintf(session, fh, fmt, ap);
	va_end(ap);

	return (ret);
}
