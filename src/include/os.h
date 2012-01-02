/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	SYSCALL_RETRY(call, ret) do {					\
	int __retry;							\
	for (__retry = 0; __retry < 10; ++__retry) {			\
		if ((call)) {						\
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
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

	off_t	file_size;			/* File size */

	char	*name;				/* File name */
	int	fd;				/* POSIX file handle */

	u_int	refcnt;				/* Reference count */
};
