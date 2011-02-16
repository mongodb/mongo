/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

typedef struct WT_CONFIG WT_CONFIG;
typedef struct WT_CONFIG_ITEM WT_CONFIG_ITEM;

struct WT_CONFIG
{
	const char *orig;
	const char *end;
	const char *cur;

	int depth, top;
	void **go;
};

struct WT_CONFIG_ITEM
{
	const char *str;
	size_t len;
	uint64_t val;
	enum { ITEM_STRING, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

int config_init(WT_CONFIG *conf, const char *confstr, int len);
int config_next(WT_CONFIG *conf, WT_CONFIG_ITEM *key, WT_CONFIG_ITEM *value);
