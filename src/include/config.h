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
	int8_t *go;
};

struct WT_CONFIG_ITEM
{
	const char *str;
	size_t len;
	uint64_t val;
	enum { ITEM_STRING, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

#define	CONFIG_LOOP(isession, cstr, cvalue) do {			\
	WT_CONFIG __conf;						\
	WT_CONFIG_ITEM __ckey;						\
	int __ret;							\
									\
	WT_RET(__wt_config_init(&__conf, (cstr), strlen(cstr)));	\
	while ((__ret = 						\
	    __wt_config_next(&__conf, &__ckey, &(cvalue))) == 0) {	\
		if (__ckey.type != ITEM_STRING &&			\
		    __ckey.type != ITEM_ID) {				\
			__wt_err(NULL, (isession), EINVAL,		\
			    "Configuration key not a string");		\
			return (EINVAL);				\
		}

#define	CONFIG_ITEM(k)							\
		else if (strncasecmp(k, __ckey.str, __ckey.len) == 0)

#define	CONFIG_END(isession)						\
		else {							\
			__wt_err(NULL, (isession), EINVAL,		\
			    "Unknown configuration key '%.*s'",		\
			    __ckey.len, __ckey.str);			\
			return (EINVAL);				\
		}							\
	}								\
									\
	if (__ret != WT_NOTFOUND)					\
		return (__ret);						\
} while (0) /* Keep s_style happy. */ ;
