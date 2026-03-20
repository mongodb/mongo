/*
 * $Id: debug.h,v 1.5 2006/01/30 23:07:57 mclark Exp $
 *
 * Copyright (c) 2004, 2005 Metaparadigm Pte. Ltd.
 * Michael Clark <michael@metaparadigm.com>
 * Copyright (c) 2009 Hewlett-Packard Development Company, L.P.
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See COPYING for details.
 *
 */

/**
 * @file
 * @brief Do not use, json-c internal, may be changed or removed at any time.
 */
#ifndef _JSON_C_DEBUG_H_
#define _JSON_C_DEBUG_H_

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JSON_EXPORT
#if defined(_MSC_VER) && defined(JSON_C_DLL)
#define JSON_EXPORT __declspec(dllexport)
#else
#define JSON_EXPORT extern
#endif
#endif

JSON_EXPORT void mc_set_debug(int debug);
JSON_EXPORT int mc_get_debug(void);

JSON_EXPORT void mc_set_syslog(int syslog);

JSON_EXPORT void mc_debug(const char *msg, ...);
JSON_EXPORT void mc_error(const char *msg, ...);
JSON_EXPORT void mc_info(const char *msg, ...);

#ifndef __STRING
#define __STRING(x) #x
#endif

#ifndef PARSER_BROKEN_FIXED

#define JASSERT(cond) \
	do            \
	{             \
	} while (0)

#else

#define JASSERT(cond)                                                                              \
	do                                                                                         \
	{                                                                                          \
		if (!(cond))                                                                       \
		{                                                                                  \
			mc_error("cjson assert failure %s:%d : cond \"" __STRING(cond) "failed\n", \
			         __FILE__, __LINE__);                                              \
			*(int *)0 = 1;                                                             \
			abort();                                                                   \
		}                                                                                  \
	} while (0)

#endif

#define MC_ERROR(x, ...) mc_error(x, ##__VA_ARGS__)

#ifdef MC_MAINTAINER_MODE
#define MC_SET_DEBUG(x) mc_set_debug(x)
#define MC_GET_DEBUG() mc_get_debug()
#define MC_SET_SYSLOG(x) mc_set_syslog(x)
#define MC_DEBUG(x, ...) mc_debug(x, ##__VA_ARGS__)
#define MC_INFO(x, ...) mc_info(x, ##__VA_ARGS__)
#else
#define MC_SET_DEBUG(x) \
	if (0)          \
	mc_set_debug(x)
#define MC_GET_DEBUG() (0)
#define MC_SET_SYSLOG(x) \
	if (0)           \
	mc_set_syslog(x)
#define MC_DEBUG(x, ...) \
	if (0)           \
	mc_debug(x, ##__VA_ARGS__)
#define MC_INFO(x, ...) \
	if (0)          \
	mc_info(x, ##__VA_ARGS__)
#endif

#ifdef __cplusplus
}
#endif

#endif
