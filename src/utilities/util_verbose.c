/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

/*
 * __handle_error_verbose --
 *	Verbose WT_EVENT_HANDLER->handle_error implementation: send to stderr.
 */
static int
__handle_error_verbose(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, int error, const char *errmsg)
{
	WT_UNUSED(handler);
	WT_UNUSED(session);
	WT_UNUSED(error);

	return (fprintf(stderr, "%s\n", errmsg) < 0 ? EIO : 0);
}

/*
 * __handle_message_verbose --
 *	Verbose WT_EVENT_HANDLER->handle_message implementation: send to stdout.
 */
static int
__handle_message_verbose(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *message)
{
	WT_UNUSED(handler);
	WT_UNUSED(session);

	return (printf("%s\n", message) < 0 ? EIO : 0);
}

/*
 * __handle_progress_verbose --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
__handle_progress_verbose(WT_EVENT_HANDLER *handler,
    WT_SESSION *session, const char *operation, uint64_t progress)
{
	WT_UNUSED(handler);
	WT_UNUSED(session);

	return (
	    printf("\r\t%s %-20" PRIu64, operation, progress) < 0 ? EIO : 0);
}

static WT_EVENT_HANDLER __event_handler_verbose = {
	__handle_error_verbose,
	__handle_message_verbose,
	__handle_progress_verbose,
	NULL	/* Close handler. */

};

WT_EVENT_HANDLER *verbose_handler = &__event_handler_verbose;
