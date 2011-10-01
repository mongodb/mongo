/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	SYSCALL_RETRY(call, ret) do {					\
	int __retry;							\
	for (__retry = 0; __retry < 5; ++__retry) {			\
		if (((ret) = (call)) == 0)				\
			break;						\
		/* Return the errno, not the call's failure return. */	\
		ret = errno;						\
		if (errno != EAGAIN &&					\
		    errno != EBUSY && errno != EINTR && errno != EIO)	\
			break;						\
		__wt_sleep(1L, 0L);					\
	}								\
} while (0)

struct __wt_fh {
	TAILQ_ENTRY(__wt_fh) q;			/* List of open handles */

	off_t	file_size;			/* File size */

	char	*name;				/* File name */
	int	fd;				/* POSIX file handle */

	u_int	refcnt;				/* Reference count */
};
