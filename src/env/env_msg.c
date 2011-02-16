/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#define	WT_MSG(env, fmt) {						\
	extern FILE *__wt_err_stream;					\
	va_list __ap;							\
	/*								\
	 * Support messages even when we don't yet have an ENV handle,	\
	 * using the error stream.
	 */								\
	if ((env) == NULL) {						\
		va_start(__ap, fmt);					\
		__wt_msg_stream(					\
		    __wt_err_stream, NULL, NULL, 0, fmt, __ap);		\
		va_end(__ap);						\
		return;							\
	}								\
									\
	/* Application-specified callback function. */			\
	if ((env)->msgcall != NULL) {					\
		va_start(__ap, fmt);					\
		__wt_msg_call((void *)((env)->msgcall),			\
		    (void *)env, NULL, NULL, 0, fmt, __ap);		\
		va_end(__ap);						\
	}								\
									\
	/*								\
	 * If the application set an message callback function but not a\
	 * message stream, we're done.  Otherwise, write the stream.	\
	 */								\
	if ((env)->msgcall != NULL && (env)->msgfile == NULL)		\
			return;						\
									\
	va_start(__ap, fmt);						\
	__wt_msg_stream((env)->msgfile, NULL, NULL, 0, fmt, __ap);	\
	va_end(__ap);							\
}

/*
 * __wt_msg --
 *	Write a message.
 */
void
__wt_msg(ENV *env, const char *fmt, ...)
{
	WT_MSG(env, fmt);
}

/*
 * __wt_mb_init --
 *	Initialize a WT_MBUF structure for message aggregation.
 */
void
__wt_mb_init(ENV *env, WT_MBUF *mbp)
{
	mbp->env = env;
	mbp->first = mbp->next = NULL;
	mbp->len = 0;
}

/*
 * __wt_mb_discard --
 *	Discard a WT_MBUF structure.
 */
void
__wt_mb_discard(WT_MBUF *mbp)
{
	if (mbp->first == NULL)
		return;

	/* Write any remaining message. */
	if (mbp->next != mbp->first)
		__wt_mb_write(mbp);

	__wt_free(mbp->env, mbp->first, mbp->len);
}

/*
 * __wt_mb_add --
 *	Append log messages into a WT_MBUF structure.
 */
void
__wt_mb_add(WT_MBUF *mbp, const char *fmt, ...)
{
	va_list ap;
	size_t current, len, remain;

	va_start(ap, fmt);

	current = (size_t)(mbp->next - mbp->first);
	remain = mbp->len - current;
	len = 64;
	for (;;) {
		/*
		 * If we don't have at least "len" bytes allocate 2x len bytes
		 * more memory.
		 */
		if (remain <= len) {
			if (__wt_realloc(mbp->env,
			    &mbp->len, mbp->len + len * 2, &mbp->first))
				return;
			mbp->next = mbp->first + current;
			remain = mbp->len - current;
		}
		/*
		 * Format the user's information.  If it doesn't fit into the
		 * buffer we have, re-allocate enough memory and try again.
		 */
		len = (size_t)vsnprintf(mbp->next, remain, fmt, ap);
		if (len < remain) {
			mbp->next += len;
			break;
		}
	}
}

/*
 * __wt_mb_write --
 *	Write the messages from a WT_MBUF structure.
 */
void
__wt_mb_write(WT_MBUF *mbp)
{
	if (mbp->first == NULL || mbp->next == mbp->first)
		return;

	__wt_msg(mbp->env, "%s", mbp->first);

	mbp->next = mbp->first;
}
