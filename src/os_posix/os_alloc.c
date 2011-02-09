/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __wt_free_overwrite(uint8_t *, size_t, const char *, int);
static void __wt_mtrack(
    ENV *env, const void *, const void *, const char *, int);
#endif

/*
 * There's no malloc interface, WiredTiger never calls malloc.  The problem is
 * an application might: allocate memory, write secret stuff into it, free the
 * memory, then we allocate the memory and use it for a file page or log record,
 * and then write it to disk.  That would result in the secret stuff being
 * protected by the WiredTiger permission mechanisms, potentially inappropriate
 * for the secret stuff.
 */

/*
 * __wt_calloc_func --
 *	ANSI calloc function.
 */
int
__wt_calloc_func(ENV *env, size_t number, size_t size, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	WT_ASSERT(env, number != 0 && size != 0);

	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC);

	if ((p = calloc(number, (size_t)size)) == NULL) {
		__wt_api_env_err(env, errno, "memory allocation");
		return (WT_ERROR);
	}
	*(void **)retp = p;

#ifdef	HAVE_DIAGNOSTIC
	__wt_mtrack(env, NULL, p, file, line);
#endif
	return (0);
}

/*
 * __wt_realloc_func --
 *	ANSI realloc function.
 */
int
__wt_realloc_func(ENV *env,
    uint32_t *bytes_allocated_ret, size_t bytes_to_allocate, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	void *p;
	size_t bytes_allocated;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	WT_ASSERT(env, bytes_to_allocate != 0);

	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC);

	p = *(void **)retp;

	bytes_allocated = *bytes_allocated_ret;
	WT_ASSERT(env, bytes_allocated < bytes_to_allocate);

	if ((p = realloc(p, bytes_to_allocate)) == NULL) {
		__wt_api_env_err(env, errno, "memory allocation");
		return (WT_ERROR);
	}

	/*
	 * Clear the allocated memory -- an application might: allocate memory,
	 * write secret stuff into it, free the memory, then we re-allocate the
	 * memory and use it for a file page or log record, and then write it to
	 * disk.  That would result in the secret stuff being protected by the
	 * WiredTiger permission mechanisms, potentially inappropriate for the
	 * secret stuff.
	 */
	memset((uint8_t *)
	    p + bytes_allocated, 0, bytes_to_allocate - bytes_allocated);

	/* Update caller's bytes allocated value. */
	if (bytes_allocated_ret != NULL) {
		WT_ASSERT(env,
		    bytes_to_allocate == (uint32_t)bytes_to_allocate);
		*bytes_allocated_ret = (uint32_t)bytes_to_allocate;
	}

#ifdef	HAVE_DIAGNOSTIC
	__wt_mtrack(env, *(void **)retp, p, file, line);
#endif

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_strdup_func --
 *	ANSI strdup function.
 */
int
__wt_strdup_func(ENV *env, const char *str, void *retp
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	size_t len;
	void *p;

	if (str == NULL) {
		*(void **)retp = NULL;
		return (0);
	}

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMALLOC);

	len = strlen(str) + 1;
#ifdef HAVE_DIAGNOSTIC
	WT_RET(__wt_calloc_func(env, len, 1, &p, file, line));
#else
	WT_RET(__wt_calloc_func(env, len, 1, &p));
#endif

	memcpy(p, str, len);

	*(void **)retp = p;
	return (0);
}

/*
 * __wt_free_func --
 *	ANSI free function.
 */
void
__wt_free_func(ENV *env, void *p_arg
#ifdef HAVE_DIAGNOSTIC
    , size_t len, const char *file, int line
#endif
    )
{
	void *p;

	/*
	 * !!!
	 * This function MUST handle a NULL ENV structure reference.
	 */
	if (env != NULL && env->ienv != NULL && env->ienv->stats != NULL)
		WT_STAT_INCR(env->ienv->stats, MEMFREE);

	/*
	 * If there's a serialization bug we might race with another thread.
	 * We can't avoid the race (and we aren't willing to flush memory),
	 * but we minimize the window by clearing the free address atomically,
	 * hoping a racing thread will see, and won't free, a NULL pointer.
	 */
	p = *(void **)p_arg;
	*(void **)p_arg = NULL;

	if (p == NULL)			/* ANSI C free semantics */
		return;

#ifdef HAVE_DIAGNOSTIC
	/*
	 * If we know how long the object is, overwrite it with an easily
	 * recognizable value for debugging.
	 */
	if (len != 0)
		__wt_free_overwrite(p, len, file, line);

	__wt_mtrack(env, p, NULL, NULL, 0);
#endif

	free(p);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_free_overwrite --
 *	Overwrite free'd memory with an easily recognizable value for debugging.
 */
static void
__wt_free_overwrite(uint8_t *m, size_t mlen, const char *file, int line)
{
	const char *p;
	size_t lfile, lline;
	char lbuf[10];

	/*
	 * Move a pointer to the file name, we don't need the whole path, and
	 * the smaller the identifying chunk, the better off we are.
	 */
	if ((p = strrchr(file, '/')) == NULL)
		p = file;
	lfile = strlen(p);
	lline = (size_t)snprintf(lbuf, sizeof(lbuf), "/%d", line);

	/* Repeatedly copy the file/line information into the free'd memory. */
	for (; mlen >= lfile + lline; mlen -= (lfile + lline)) {
		memcpy(m, p, lfile);
		m += lfile;
		memcpy(m, lbuf, lline);
		m += lline;
	}
	for (; mlen > 0; --mlen)
		*m++ = WT_DEBUG_BYTE;
}

/*
 * __wt_mtrack_alloc --
 *	Allocate memory tracking structures.
 */
int
__wt_mtrack_alloc(ENV *env)
{
	IENV *ienv;
	WT_MTRACK *p;

	ienv = env->ienv;

	/*
	 * Use a temporary variable -- assigning memory to ienv->mtrack turns
	 * on memory object tracking, and we need to set up the rest of the
	 * structure first.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_MTRACK), &p));
	WT_RET(__wt_calloc(env, 1000, sizeof(WT_MEM), &p->list));
	p->next = p->list;
	p->slots = 1000;
	ienv->mtrack = p;
	return (0);
}

/*
 * __wt_mtrack_free --
 *	Free memory tracking structures.
 */
void
__wt_mtrack_free(ENV *env)
{
	IENV *ienv;
	WT_MTRACK *p;

	ienv = env->ienv;

	/*
	 * Clear ienv->mtrack (to turn off memory object tracking) before the
	 * free.
	 */
	if ((p = ienv->mtrack) == NULL)
		return;
	ienv->mtrack = NULL;

	__wt_free(env, p->list, 0);
	__wt_free(env, p, 0);
}

/*
 * __wt_mtrack_free --
 *	Track memory allocations and frees.
 */
static void
__wt_mtrack(ENV *env, const void *f, const void *a, const char *file, int line)
{
	WT_MEM *mp, *t, *mp_end;
	WT_MTRACK *mtrack;
	int slot_check;

	if (env == NULL ||
	    env->ienv == NULL || (mtrack = env->ienv->mtrack) == NULL)
		return;

	/*
	 * Remove freed memory from the list.  If it's a free/alloc pair (that
	 * is, if __wt_realloc was called), re-use the slot.
	 */
	if (f != NULL) {
		if ((mp = mtrack->next) > mtrack->list)
			do {
				if ((--mp)->addr == f)
					goto enter;
			} while (mp > mtrack->list);

		__wt_api_env_errx(env, "mtrack: %p: not found", f);
		__wt_attach(env);
	}

	if (a == NULL)
		return;

	/*
	 * Add allocated memory to the list.
	 *
	 * First, see if there's a slot close by we can re-use (the assumption
	 * is that when memory is allocated and quickly freed we re-use the
	 * slots instead of leaving lots of free spots in the array.
	 */
	if ((mp = mtrack->next) > mtrack->list)
		for (slot_check = 0; slot_check < 10; ++slot_check) {
			if ((--mp)->addr == NULL)
				goto enter;
			if (mp == mtrack->list)
				break;
		}

	mp_end = mtrack->list + mtrack->slots;

	/* If there's an empty slot, use it. */
	if (mtrack->next < mp_end)
		goto next;

	/* Try to compress the array. */
	for (mp = mtrack->list, t = NULL;; ++mp, ++t) {
		while (mp < mp_end && mp->addr != NULL)
			++mp;
		if (mp == mp_end)
			break;
		if (t == NULL)
			t = mp + 1;
		while (t < mp_end && t->addr == NULL)
			++t;
		if (t == mp_end)
			break;
		*mp++ = *t;
		t->addr = NULL;
	}
	mtrack->next = mp;

	/* If there's an empty slot, use it. */
	if (mtrack->next < mp_end)
		goto next;

	/* Re-allocate the array and use the next empty slot. */
	if ((mtrack->list = realloc(mtrack->list,
	    mtrack->slots * 2 * sizeof(WT_MEM))) == NULL)
		return;
	mtrack->next = mtrack->list + mtrack->slots;
	mtrack->slots *= 2;

next:	mp = mtrack->next++;
enter:	mp->addr = a;
	mp->file = file;
	mp->line = line;
}

/*
 * __wt_mtrack_dump --
 *	Complain about any memory allocated but never freed.
 */
void
__wt_mtrack_dump(ENV *env)
{
	WT_MTRACK *mtrack;
	WT_MEM *mp;

	if ((mtrack = env->ienv->mtrack) == NULL)
		return;

	for (mp = mtrack->list; mp < mtrack->next; ++mp)
		if (mp->addr != NULL)
			__wt_api_env_errx(env,
			    "mtrack: %p {%s/%d}: never freed",
				mp->addr, mp->file, mp->line);
}
#endif
