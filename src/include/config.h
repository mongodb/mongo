/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_config {
	WT_SESSION_IMPL *session;
	const char *orig;
	const char *end;
	const char *cur;

	int depth, top;
	const int8_t *go;
};

struct __wt_config_item {
	const char *str;
	size_t len;
	int64_t val;
	enum { ITEM_STRING, ITEM_BOOL, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

struct __wt_config_check {
	const char *name;
	const char *type;
	const char *checks;
	const WT_CONFIG_CHECK *subconfigs;
};
