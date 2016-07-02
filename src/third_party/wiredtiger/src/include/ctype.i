/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <ctype.h>

/*
 * __wt_isalnum --
 *	Wrap the ctype function without sign extension.
 */
static inline bool
__wt_isalnum(u_char c)
{
	return (isalnum(c) != 0);
}

/*
 * __wt_isalpha --
 *	Wrap the ctype function without sign extension.
 */
static inline bool
__wt_isalpha(u_char c)
{
	return (isalpha(c) != 0);
}

/*
 * __wt_isdigit --
 *	Wrap the ctype function without sign extension.
 */
static inline bool
__wt_isdigit(u_char c)
{
	return (isdigit(c) != 0);
}

/*
 * __wt_isprint --
 *	Wrap the ctype function without sign extension.
 */
static inline bool
__wt_isprint(u_char c)
{
	return (isprint(c) != 0);
}

/*
 * __wt_isspace --
 *	Wrap the ctype function without sign extension.
 */
static inline bool
__wt_isspace(u_char c)
{
	return (isspace(c) != 0);
}

/*
 * __wt_tolower --
 *	Wrap the ctype function without sign extension.
 */
static inline u_char
__wt_tolower(u_char c)
{
	return ((u_char)tolower(c));
}
