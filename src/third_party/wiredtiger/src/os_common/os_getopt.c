/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*	$NetBSD: getopt.c,v 1.26 2003/08/07 16:43:40 agc Exp $	*/

/*
 * Copyright (c) 1987, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "wt_internal.h"

extern int __wt_opterr, __wt_optind, __wt_optopt, __wt_optreset;
int	__wt_opterr = 1,	/* if error message should be printed */
	__wt_optind = 1,	/* index into parent argv vector */
	__wt_optopt,		/* character checked for validity */
	__wt_optreset;		/* reset getopt */

extern char *__wt_optarg;
char	*__wt_optarg;		/* argument associated with option */

#define	BADCH	(int)'?'
#define	BADARG	(int)':'
#define	EMSG	""

/*
 * __wt_getopt --
 *	Parse argc/argv argument vector.
 */
int
__wt_getopt(
    const char *progname, int nargc, char * const *nargv, const char *ostr)
{
	static const char *place = EMSG;	/* option letter processing */
	const char *oli;			/* option letter list index */

	if (__wt_optreset || *place == 0) {	/* update scanning pointer */
		__wt_optreset = 0;
		place = nargv[__wt_optind];
		if (__wt_optind >= nargc || *place++ != '-') {
			/* Argument is absent or is not an option */
			place = EMSG;
			return (-1);
		}
		__wt_optopt = *place++;
		if (__wt_optopt == '-' && *place == 0) {
			/* "--" => end of options */
			++__wt_optind;
			place = EMSG;
			return (-1);
		}
		if (__wt_optopt == 0) {
			/* Solitary '-', treat as a '-' option
			   if the program (eg su) is looking for it. */
			place = EMSG;
			if (strchr(ostr, '-') == NULL)
				return (-1);
			__wt_optopt = '-';
		}
	} else
		__wt_optopt = *place++;

	/* See if option letter is one the caller wanted... */
	if (__wt_optopt == ':' || (oli = strchr(ostr, __wt_optopt)) == NULL) {
		if (*place == 0)
			++__wt_optind;
		if (__wt_opterr && *ostr != ':')
			(void)fprintf(stderr,
			    "%s: illegal option -- %c\n", progname,
			    __wt_optopt);
		return (BADCH);
	}

	/* Does this option need an argument? */
	if (oli[1] != ':') {
		/* don't need argument */
		__wt_optarg = NULL;
		if (*place == 0)
			++__wt_optind;
	} else {
		/* Option-argument is either the rest of this argument or the
		   entire next argument. */
		if (*place)
			__wt_optarg = (char *)place;
		else if (nargc > ++__wt_optind)
			__wt_optarg = nargv[__wt_optind];
		else {
			/* option-argument absent */
			place = EMSG;
			if (*ostr == ':')
				return (BADARG);
			if (__wt_opterr)
				(void)fprintf(stderr,
				    "%s: option requires an argument -- %c\n",
				    progname, __wt_optopt);
			return (BADCH);
		}
		place = EMSG;
		++__wt_optind;
	}
	return (__wt_optopt);			/* return option letter */
}
