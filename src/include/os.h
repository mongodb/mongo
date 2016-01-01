/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * FILE handle close/open configuration.
 */
typedef enum {
	WT_FHANDLE_APPEND, WT_FHANDLE_READ, WT_FHANDLE_WRITE
} WT_FHANDLE_MODE;

#ifdef	_WIN32
/*
 * Open in binary (untranslated) mode; translations involving carriage-return
 * and linefeed characters are suppressed.
 */
#define	WT_FOPEN_APPEND		"ab"
#define	WT_FOPEN_READ		"rb"
#define	WT_FOPEN_WRITE		"wb"
#else
#define	WT_FOPEN_APPEND		"a"
#define	WT_FOPEN_READ		"r"
#define	WT_FOPEN_WRITE		"w"
#endif

#define	WT_FOPEN_FIXED		0x1	/* Path isn't relative to home */

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

struct __wt_fh {
	char	*name;				/* File name */
	uint64_t name_hash;			/* Hash of name */
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */
	TAILQ_ENTRY(__wt_fh) hashq;		/* Hashed list of handles */

	u_int	ref;				/* Reference count */

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

	bool	 direct_io;			/* O_DIRECT configured */

	enum {					/* file extend configuration */
	    WT_FALLOCATE_AVAILABLE,
	    WT_FALLOCATE_NOT_AVAILABLE,
	    WT_FALLOCATE_POSIX,
	    WT_FALLOCATE_STD,
	    WT_FALLOCATE_SYS } fallocate_available;
	bool fallocate_requires_locking;
};
