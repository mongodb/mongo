/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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
			__wt_sleep(0L, 500000L);			\
			continue;					\
		default:						\
			break;						\
		}							\
		break;							\
	}								\
} while (0)

#define	WT_TIMEDIFF(end, begin)						\
	(1000000000 * (uint64_t)((end).tv_sec - (begin).tv_sec) +	\
	    (uint64_t)(end).tv_nsec - (uint64_t)(begin).tv_nsec)
#define	WT_TIMECMP(t1, t2)						\
	((t1).tv_sec < (t2).tv_sec ? -1 :				\
	     (t1).tv_sec == (t2.tv_sec) ?				\
	     (t1).tv_nsec < (t2).tv_nsec ? -1 :				\
	     (t1).tv_nsec == (t2).tv_nsec ? 0 : 1 : 1)

struct __wt_fh {
	char	*name;				/* File name */
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

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

	int	direct_io;			/* O_DIRECT configured */

	enum {					/* file extend configuration */
	    WT_FALLOCATE_AVAILABLE,
	    WT_FALLOCATE_NOT_AVAILABLE,
	    WT_FALLOCATE_POSIX,
	    WT_FALLOCATE_STD,
	    WT_FALLOCATE_SYS } fallocate_available;
	int	fallocate_requires_locking;
};

#ifndef _WIN32
#define	WT_SIZET_FMT	"zu"			/* size_t format string */
#else
#define	WT_SIZET_FMT	"Iu"			/* size_t format string */
#endif
