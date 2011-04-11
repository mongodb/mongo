/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __handle_error_default --
 *	Default WT_EVENT_HANDLER->handle_error implementation: send to stderr.
 */
static void
__handle_error_default(WT_EVENT_HANDLER *handler, int error, const char *errmsg)
{
	WT_UNUSED(handler);
	WT_UNUSED(error);

	fprintf(stderr, "%s\n", errmsg);
}

/*
 * __handle_message_default --
 *	Default WT_EVENT_HANDLER->handle_message implementation: ignore.
 */
static int
__handle_message_default(WT_EVENT_HANDLER *handler, const char *message)
{
	WT_UNUSED(handler);
	WT_UNUSED(message);

	return (0);
}

/*
 * __handle_message_verbose --
 *	Verbose WT_EVENT_HANDLER->handle_message implementation: send to stdout.
 */
static int
__handle_message_verbose(WT_EVENT_HANDLER *handler, const char *message)
{
	WT_UNUSED(handler);

	(void)printf("%s\n", message);
	return (0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
__handle_progress_default(WT_EVENT_HANDLER *handler,
     const char *operation, uint64_t progress)
{
	WT_UNUSED(handler);
	WT_UNUSED(operation);
	WT_UNUSED(progress);

	return (0);
}

/*
 * __handle_progress_verbose --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
__handle_progress_verbose(WT_EVENT_HANDLER *handler,
     const char *operation, uint64_t progress)
{
	WT_UNUSED(handler);

	(void)printf("\r\t%s %-20llu", operation, (unsigned long long)progress);
	return (0);
}

static WT_EVENT_HANDLER __event_handler_default = {
	__handle_error_default,
	__handle_message_default,
	__handle_progress_default
};

static WT_EVENT_HANDLER __event_handler_verbose = {
	__handle_error_default,
	__handle_message_verbose,
	__handle_progress_verbose
};

WT_EVENT_HANDLER *__wt_event_handler_default = &__event_handler_default;
WT_EVENT_HANDLER *__wt_event_handler_verbose = &__event_handler_verbose;
