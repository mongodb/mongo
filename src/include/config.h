/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_config {
	WT_SESSION_IMPL *session;
	const char *orig;
	const char *end;
	const char *cur;

	int depth, top;
	int8_t *go;
};

struct __wt_config_item {
	const char *str;
	size_t len;
	uint64_t val;
	enum { ITEM_STRING, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};
