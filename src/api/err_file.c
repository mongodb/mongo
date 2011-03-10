/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __handle_error_default --
 *	Default WT_ERROR_HANDLER->handle_error implementation.
 */
static int
__handle_error_default(WT_ERROR_HANDLER *handler, int error, const char *errmsg)
{
	WT_UNUSED(handler);
	WT_UNUSED(error);

	fprintf(stderr, "%s\n", errmsg);

	return (0);
}

/*
 * __get_messages_default --
 *	Default WT_ERROR_HANDLER->get_messages implementation.
 */
static int
__get_messages_default(WT_ERROR_HANDLER *handler, const char **errmsgp)
{
	WT_UNUSED(handler);
	WT_UNUSED(errmsgp);

	return (ENOTSUP);
}

/*
 * __clear_messages_default --
 *	Default WT_ERROR_HANDLER->clear_messages implementation.
 */
static int
__clear_messages_default(WT_ERROR_HANDLER *handler)
{
	WT_UNUSED(handler);

	return (ENOTSUP);
}

static WT_ERROR_HANDLER __error_handler_default = {
	__handle_error_default,
	__get_messages_default,
	__clear_messages_default,
};

WT_ERROR_HANDLER *__wt_error_handler_default = &__error_handler_default;
