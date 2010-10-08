/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_env_toc --
 *	ENV.toc method.
 */
int
__wt_env_toc(ENV *env, WT_TOC **tocp)
{
	IENV *ienv;
	WT_TOC *toc;
	u_int32_t slot;

	ienv = env->ienv;
	*tocp = NULL;

	/* Check to see if there's an available WT_TOC slot. */
	if (ienv->toc_cnt == env->toc_size - 1) {
		__wt_api_env_errx(env,
		    "WiredTiger only configured to support %d thread contexts",
		    env->toc_size);
		return (WT_ERROR);
	}

	/*
	 * The WT_TOC reference list is compact, the WT_TOC array is not.  Find
	 * the first empty WT_TOC slot.
	 */
	for (slot = 0, toc = ienv->toc_array; toc->env != NULL; ++toc, ++slot)
		;

	/* Clear previous contents of the WT_TOC entry, they get re-used. */
	memset(toc, 0, sizeof(WT_TOC));

	toc->env = env;
	toc->gen = UINT32_MAX;
	toc->hazard = ienv->hazard + slot * env->hazard_size;

	WT_RET(__wt_mtx_alloc(env, "toc", 1, &toc->mtx));

	__wt_methods_wt_toc_lockout(toc);
	__wt_methods_wt_toc_init_transition(toc);

	/* Make the entry visible to the workQ. */
	ienv->toc[ienv->toc_cnt++] = toc;
	WT_MEMORY_FLUSH;

	*tocp = toc;
	return (0);
}

/*
 * __wt_wt_toc_close --
 *	WT_TOC.close method.
 */
int
__wt_wt_toc_close(WT_TOC *toc)
{
	ENV *env;
	IENV *ienv;
	WT_TOC **tp;
	int ret;

	env = toc->env;
	ienv = env->ienv;
	ret = 0;

	WT_ENV_FCHK_RET(
	    env, "WT_TOC.close", toc->flags, WT_APIMASK_WT_TOC, ret);

	/* Discard DBT memory. */
	__wt_free(env, toc->key.data, toc->key.mem_size);
	__wt_free(env, toc->data.data, toc->data.mem_size);
	__wt_free(env, toc->tmp1.data, toc->tmp1.mem_size);
	__wt_free(env, toc->tmp2.data, toc->tmp2.mem_size);

	/* Unlock and destroy the thread's mutex. */
	if (toc->mtx != NULL) {
		__wt_unlock(env, toc->mtx);
		(void)__wt_mtx_destroy(env, toc->mtx);
	}

	/*
	 * Replace the WT_TOC reference we're closing with the last entry in
	 * the table, then clear the last entry.  As far as the walk of the
	 * workQ is concerned, it's OK if the WT_TOC appears twice, or if it
	 * doesn't appear at all, so these lines can race all they want.
	 */
	for (tp = ienv->toc; *tp != toc; ++tp)
		;
	--ienv->toc_cnt;
	*tp = ienv->toc[ienv->toc_cnt];
	ienv->toc[ienv->toc_cnt] = NULL;

	/* Make the WT_TOC array entry available for re-use. */
	toc->env = NULL;
	WT_MEMORY_FLUSH;

	return (ret);
}

/*
 * __wt_toc_api_set --
 *	Pair WT_TOC and DB handle, allocating the WT_TOC as necessary.
 */
int
__wt_toc_api_set(ENV *env, const char *name, DB *db, WT_TOC **tocp)
{
	WT_TOC *toc;

	/*
	 * We pass around WT_TOCs internally in the Btree, (rather than a DB),
	 * because the DB's are free-threaded, and the WT_TOCs are per-thread.
	 * Lots of the API calls don't require the application to allocate and
	 * manage the WT_TOC, which means we have to do it for them.
	 *
	 * WT_TOCs always reference a DB handle, and we do that here, as well.
	 */
	if ((toc = *tocp) == NULL) {
		WT_RET(env->toc(env, 0, tocp));
		toc = *tocp;
	}
	toc->db = db;
	toc->name = name;
	WT_TOC_GEN_SET(toc);
	return (0);
}

/*
 * __wt_toc_api_clr --
 *	Clear the WT_TOC, freeing it if it was allocated by the library.
 */
int
__wt_toc_api_clr(WT_TOC *toc, const char *name, int islocal)
{
	/*
	 * The WT_TOC should hold no more hazard references; this is a
	 * diagnostic check, but it's cheap so we do it all the time.
	 */
	__wt_hazard_empty(toc, name);

	if (islocal)
		return (toc->close(toc, 0));

	WT_TOC_GEN_CLR(toc);

	toc->db = NULL;
	toc->name = NULL;
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
static const char *__wt_toc_print_state(WT_TOC *);

int
__wt_toc_dump(ENV *env)
{
	IENV *ienv;
	WT_MBUF mb;
	WT_TOC *toc, **tp;
	WT_PAGE **hp;

	ienv = env->ienv;
	__wt_mb_init(env, &mb);

	__wt_mb_add(&mb, "%s\n", ienv->sep);
	for (tp = ienv->toc; (toc = *tp) != NULL; ++tp) {
		__wt_mb_add(&mb,
		    "toc: %p (gen: %lu) {\n\tworkq func: ",
		    toc, (u_long)toc->gen);
		if (toc->wq_func == NULL)
			__wt_mb_add(&mb, "none");
		else
			__wt_mb_add(&mb, "%p", toc->wq_func);

		__wt_mb_add(&mb, " state: %s", __wt_toc_print_state(toc));

		__wt_mb_add(&mb, "\n\thazard: ");
		for (hp = toc->hazard;
		    hp < toc->hazard + env->hazard_size; ++hp)
			__wt_mb_add(&mb, "%p ", *hp);

		__wt_mb_add(&mb, "\n}");
		if (toc->name != NULL)
			__wt_mb_add(&mb, " %s", toc->name);
		__wt_mb_write(&mb);
	}

	__wt_mb_discard(&mb);
	return (0);
}

/*
 * __wt_toc_print_state --
 *	Return the WT_TOC state as a string.
 */
static const char *
__wt_toc_print_state(WT_TOC *toc)
{
	switch (toc->wq_state) {
	case WT_WORKQ_READ:
		return ("read");
	case WT_WORKQ_READ_SCHED:
		return ("read scheduled");
	case WT_WORKQ_SPIN:
		return ("spin");
	case WT_WORKQ_NONE:
		return ("none");
	}
	return ("unknown");
	/* NOTREACHED */
}
#endif
