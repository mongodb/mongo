/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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

struct __wt_fh {
	u_int	refcnt;				/* Reference count */
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

	char	*name;				/* File name */
	off_t	file_size;			/* File size */
	int	fd;				/* POSIX file handle */

	int	direct_io;			/* O_DIRECT configured */
};
