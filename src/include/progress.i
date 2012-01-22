/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_progress --
 *	Send a progress message to stdout.
 */
static inline void
__wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v)
{
	WT_EVENT_HANDLER *handler;

	if (s == NULL)
		s = session->name;

	handler = session->event_handler;
	if (handler->handle_progress != NULL)
		(void)handler->handle_progress(handler, s, v);
}
