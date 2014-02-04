/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Quiet compiler warnings about unused function parameters and variables,
 * and unused function return values.
 */
#define	WT_UNUSED(var)		(void)(var)
#ifdef __GNUC__
#define	WT_UNUSED_RET(var)						\
	({ __typeof__(var) __ret = var; (void)sizeof __ret; })
#else
#define	WT_UNUSED_RET(var)	(void)(var)
#endif

/* Add GCC-specific attributes to types and function declarations. */
#ifdef __GNUC__
#define	WT_GCC_ATTRIBUTE(x)	__attribute__(x)
#else
#define	WT_GCC_ATTRIBUTE(x)
#endif

/*
 * Attribute are only permitted on function declarations, not definitions.
 * This macro is a marker for function definitions that is rewritten by
 * dist/s_prototypes to create extern.h.
 */
#define	WT_GCC_FUNC_ATTRIBUTE(x)
