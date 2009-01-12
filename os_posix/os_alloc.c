/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC_MEMORY
#if 0
How to use the memory debugger included in the WiredTiger sources.

1. Build the WiredTiger library with the --enable-diagnostic_memory 
   configuration option.
2. Run the test program.
3. A file will have been created named "memory.out".
4. Run the python script dist/memory.py.
5. Any memory problems will be displayed.

To show memory usage of a problematic address:

1. Set the environment variable MALLOC_DEBUG_VALUE to one of the
   problematic addresses.
2. Run the test program, and there will be messages on stdout when
   the address is used.

To debug memory usage of a problematic address:
1. Set the environment variable MALLOC_DEBUG_VALUE to one of the
   problematic addresses.
2. Start a debugger on the test program, and set a breakpoint in
   __wt_debug_loadme().
3. The debugger should stop every time the problematic address is
   being used.
#endif

#define	WT_MEMORY_FILE	"memory.out"
static FILE *__wt_mfp;
static void *debug_addr;

static void
__wt_debug_loadme(const char *msg, void *addr)
{
	fprintf(stderr, "memory: %lx: %s\n", (u_long)addr, msg);
}

static int
__wt_open_mfp(ENV *env)
{
	char *debug_value;

	if ((__wt_mfp = fopen(WT_MEMORY_FILE, "w")) == NULL) {
		__wt_env_err(env, errno, "%s: open", WT_MEMORY_FILE);
		return (1);
	}

	if ((debug_value = getenv("MALLOC_DEBUG_VALUE")) != NULL)
		debug_addr = (void *)strtol(debug_value, NULL, 0);
		
	return (0);
}
#endif

/*
 * __wt_calloc --
 *	ANSI calloc function.
 */
int
__wt_calloc(ENV *env, size_t number, size_t size, void *retp)
{
	void *p;

	/*
	 * The ENV * argument isn't used, but routines at this layer
	 * are always passed one.
	 *
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if ((p = calloc(number, size)) == NULL)
		return (WT_ERROR);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (__wt_mfp == NULL && __wt_open_mfp(env) != 0)
		return (WT_ERROR);
	if (debug_addr == p)
		__wt_debug_loadme("allocation", p);
	fprintf(__wt_mfp, "A\t%lx\n", (u_long)p);
#endif

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_malloc --
 *	ANSI malloc function.
 */
int
__wt_malloc(ENV *env, size_t bytes_to_allocate, void *retp)
{
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	WT_ASSERT(env, env == NULL || bytes_to_allocate != 0);

	if ((p = malloc(bytes_to_allocate)) == NULL)
		return (WT_ERROR);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (__wt_mfp == NULL && __wt_open_mfp(env) != 0)
		return (WT_ERROR);
	if (debug_addr == p)
		__wt_debug_loadme("allocation", p);
	fprintf(__wt_mfp, "A\t%lx\n", (u_long)p);
#endif

#ifdef HAVE_DIAGNOSTIC
	memset(p, OVERWRITE_BYTE, bytes_to_allocate);
#endif

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_realloc --
 *	ANSI realloc function.
 */
int
__wt_realloc(ENV *env, size_t bytes_to_allocate, void *retp)
{
	void *p;

	p = *(void **)retp;

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (__wt_mfp == NULL && __wt_open_mfp(env) != 0)
		return (WT_ERROR);
#endif
	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	WT_ASSERT(env, env == NULL || bytes_to_allocate != 0);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (p != NULL) {
		if (debug_addr == p)
			__wt_debug_loadme("free", p);
		fprintf(__wt_mfp, "F\t%lx\n", (u_long)p);
	}
#endif
	if ((p = realloc(p, bytes_to_allocate)) == NULL)
		return (WT_ERROR);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (debug_addr == p)
		__wt_debug_loadme("allocation", p);
	fprintf(__wt_mfp, "A\t%lx\n", (u_long)p);
#endif

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_strdup --
 *	ANSI strdup function.
 */
int
__wt_strdup(ENV *env, const char *str, void *retp)
{
	size_t len;
	int ret;
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */

	len = strlen(str) + 1;
	if ((ret = __wt_malloc(env, len, &p)) != 0)
		return (ret);

	memcpy(p, str, len);

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_free --
 *	ANSI free function.
 */
void
__wt_free(ENV *env, void *p)
{
	/*
	 * The ENV * argument isn't used, but routines at this layer
	 * are always passed one.
	 *
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	WT_ASSERT(env, env == NULL || p != NULL);

	if (p != NULL)			/* ANSI C free semantics */
		free(p);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (debug_addr == p)
		__wt_debug_loadme("free", p);
	fprintf(__wt_mfp, "F\t%lx\n", (u_long)p);
#endif
}
