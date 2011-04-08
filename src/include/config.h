/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_config;		typedef struct __wt_config WT_CONFIG;
struct __wt_config_item;	typedef struct __wt_config_item WT_CONFIG_ITEM;

struct __wt_config
{
	const char *orig;
	const char *end;
	const char *cur;

	int depth, top;
	int8_t *go;
};

struct __wt_config_item
{
	const char *str;
	size_t len;
	uint64_t val;
	enum { ITEM_STRING, ITEM_ID, ITEM_NUM, ITEM_STRUCT } type;
};

#define	CONFIG_LOOP(session, cstr, cvalue) do {				\
	WT_CONFIG __conf;						\
	WT_CONFIG_ITEM __ckey;						\
	int __cret;							\
									\
	if (cstr == NULL)						\
		break;							\
	WT_RET(__wt_config_init(&__conf, (cstr)));	\
	while ((__cret = 						\
	    __wt_config_next(&__conf, &__ckey, &(cvalue))) == 0) {	\
		if (__ckey.type != ITEM_STRING &&			\
		    __ckey.type != ITEM_ID) {				\
			__wt_err((session), EINVAL,			\
			    "Configuration key not a string");		\
			return (EINVAL);				\
		}

#define	CONFIG_ITEM(k)							\
		else if (strncasecmp(k, __ckey.str, __ckey.len) == 0)

#define	CONFIG_END(session)						\
		else {							\
			__wt_err((session), EINVAL,			\
			    "Unknown configuration key '%.*s'",		\
			    __ckey.len, __ckey.str);			\
			return (EINVAL);				\
		}							\
	}								\
									\
	if (__cret != WT_NOTFOUND)					\
		return (__cret);					\
} while (0)
