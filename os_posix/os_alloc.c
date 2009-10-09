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
/*
The memory debugger included in the WiredTiger sources tracks allocations and
frees, but doesn't do any kind of overrun detection.

1. Build the WiredTiger library with the --enable-diagnostic_memory 
   configuration option.
2. Run your test program.
3. A file will have been created named "memory.out".
4. Run the python script dist/memory.py.
5. Any memory problems it finds will be displayed.

Debuggers can change memory pattern allocations, so it can be hard to get
a repeatable pattern of allocations.  To get a stack trace to review:

1. Set the environment variable WT_MEMORY_VALUE or the debug_addr variable
   to the interesting address (for example, env WT_MEMORY_VALUE=0x1e502fb3).
2. Set the environment variable WT_MEMORY_N or the debug_count variable to
   the number of touches (for example, env WT_MEMORY_N=3).
3. Run the test program, and it will drop core when the address is touched
   for the N'th time.

If your debugger doesn't change memory allocation patterns from run to run,
you can also set a breakpoint in the __wt_debug_loadme function and run the
program under a debugger.
*/

#define	WT_MEMORY_FILE	"memory.out"

static void __wt_debug_loadme(ENV *, const char *, void *);
static void  __wt_open_mfp(ENV *);

static FILE *__wt_mfp;
static void *debug_addr;
static int debug_count;

static void
__wt_debug_loadme(ENV *env, const char *msg, void *addr)
{
	fprintf(stderr, "memory: %lx: %s\n", (u_long)addr, msg);
	if (debug_count > 0 && --debug_count == 0)
		__wt_abort(env);
}

static void
__wt_open_mfp(ENV *env)
{
	char *v;

	if ((__wt_mfp = fopen(WT_MEMORY_FILE, "w")) == NULL) {
		__wt_env_err(env, errno, "%s: open", WT_MEMORY_FILE);
		__wt_abort(env);
	}

	if (debug_addr == 0 && (v = getenv("WT_MEMORY_VALUE")) != NULL)
		debug_addr = (void *)strtoul(v, NULL, 0);
	if (debug_count == 0 && (v = getenv("WT_MEMORY_N")) != NULL)
		debug_count = (int)atoi(v);
}
#endif

/*
 * __wt_malloc --
 *
 * There's no malloc interface, WiredTiger never calls malloc.  The problem is
 * an application might: allocate memory, write secret stuff into it, free the
 * memory, we allocate the memory, and then use it for a database page or log
 * record and write it to disk.  That would result in the secret stuff being
 * protected by the WiredTiger permission mechanisms, potentially inappropriate
 * for the secret stuff.
 *
 * __wt_calloc --
 *	ANSI calloc function.
 */
int
__wt_calloc(ENV *env, u_int32_t number, u_int32_t size, void *retp)
{
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC, "memory allocations");

	if ((p = calloc(number, (size_t)size)) == NULL) {
		__wt_env_err(env, errno, "memory allocation");
		return (WT_ERROR);
	}
	*(void **)retp = p;

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (__wt_mfp == NULL)
		__wt_open_mfp(env);
	if (debug_addr == p)
		__wt_debug_loadme(env, "allocation", p);
	fprintf(__wt_mfp, "A\t%lx\n", (u_long)p);
#endif

	return (0);
}

/*
 * __wt_realloc --
 *	ANSI realloc function.
 */
int
__wt_realloc(ENV *env,
    u_int32_t bytes_allocated, u_int32_t bytes_to_allocate, void *retp)
{
	void *p;

	p = *(void **)retp;

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (__wt_mfp == NULL)
		__wt_open_mfp(env);
#endif
	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC, NULL);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (p != NULL) {
		if (debug_addr == p)
			__wt_debug_loadme(env, "free", p);
		fprintf(__wt_mfp, "F\t%lx\n", (u_long)p);
	}
#endif
	if ((p = realloc(p, (size_t)bytes_to_allocate)) == NULL) {
		__wt_env_err(env, errno, "memory allocation");
		return (WT_ERROR);
	}

	/*
	 * Clear allocated memory -- see comment above concerning __wt_malloc
	 * as to why this is required.
	 */
	memset((u_int8_t *)
	    p + bytes_allocated, 0, bytes_to_allocate - bytes_allocated);

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (debug_addr == p)
		__wt_debug_loadme(env, "allocation", p);
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
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC, NULL);

	len = strlen(str) + 1;
	WT_RET(__wt_calloc(env, len, 1, &p));

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
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMFREE, "memory frees");

#ifdef HAVE_DIAGNOSTIC_MEMORY
	if (debug_addr == p)
		__wt_debug_loadme(env, "free", p);
	fprintf(__wt_mfp, "F\t%lx\n", (u_long)p);
#endif

	if (p != NULL)			/* ANSI C free semantics */
		free(p);
}
