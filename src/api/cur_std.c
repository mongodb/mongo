/* Copyright (c) 2010 WiredTiger, Inc.  All rights reserved. */

#include <sys/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wt_int.h"

static int
__curstd_get_key(WT_CURSOR *cursor, ...)
{
	return ENOTSUP;
}

static int
__curstd_get_value(WT_CURSOR *cursor, ...)
{
	return ENOTSUP;
}

static int
__curstd_set_key(WT_CURSOR *cursor, ...)
{
	return ENOTSUP;
}

static int
__curstd_set_value(WT_CURSOR *cursor, ...)
{
	return ENOTSUP;
}

void
__wt_curstd_init(WT_CURSOR_STD *stdc)
{
	WT_CURSOR *c = &stdc->interface;

	c->get_key = __curstd_get_key;
	c->get_value = __curstd_get_value;
	c->set_key = __curstd_set_key;
	c->set_value = __curstd_set_value;

	stdc->key.data = NULL;
	stdc->keybufsz = 0;
	stdc->value.data = NULL;
	stdc->valuebufsz = 0;
}

void
__wt_curstd_close(WT_CURSOR_STD *c)
{
	if (c->key.data != NULL) {
		free((void *)c->key.data);
		c->key.data = NULL;
		c->keybufsz = 0;
	}
	if (c->value.data != NULL) {
		free((void *)c->value.data);
		c->value.data = NULL;
		c->valuebufsz = 0;
	}
}
