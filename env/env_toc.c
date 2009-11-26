/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_toc_link_op(ENV *, WT_TOC *, int);

/*
 * __wt_env_toc --
 *	WT_TOC constructor.
 */
int
__wt_env_toc(ENV *env, u_int32_t flags, WT_TOC **tocp)
{
	WT_TOC *toc;

	*tocp = NULL;

	WT_RET(__wt_calloc(env, 1, sizeof(WT_TOC), &toc));
	toc->env = env;

	/* Initialize the methods. */
	__wt_methods_wt_toc_lockout(toc);
	__wt_methods_wt_toc_init_transition(toc);

	__wt_toc_link_op(env, toc, 1);		/* Add to the ENV's list */

	*tocp = toc;
	return (0);
}

/*
 * __wt_wt_toc_close --
 *	toc.close method (WT_TOC close + destructor).
 */
int
__wt_wt_toc_close(WT_TOC *toc, u_int32_t flags)
{
	ENV *env;
	int ret;

	env = toc->env;
	ret = 0;

	WT_FREE_AND_CLEAR(env, toc->key.data);
	WT_FREE_AND_CLEAR(env, toc->data.data);

	__wt_toc_link_op(env, toc, 0);		/* Delete from the ENV's list */

	return (ret);
}

/*
 * __wt_toc_link_op --
 *	Add/delete the WT_TOC to/from the environment's list of WT_TOCs.
 */
static void
__wt_toc_link_op(ENV *env, WT_TOC *toc, int add_op)
{
	IENV *ienv;

	ienv = env->ienv;

	/*
	 * The workQ thread is walking the WT_TOC queue without acquiring any
	 * kind of lock.  The way we do it is to acquire the IENV mutex and
	 * set the toc_add/toc_del field, then return, leaving the IENV locked.
	 * At some point, the workQ thread wakes up, adds/deletes the toc
	 * to/from the queue, clears the toc_add/toc_del field, and releases
	 * the mutex.
	 */
	__wt_lock(env, &ienv->mtx);
	if (add_op)
		ienv->toc_add = toc;
	else
		ienv->toc_del = toc;
	WT_MEMORY_FLUSH;
}

#ifdef HAVE_DIAGNOSTIC
int
__wt_toc_dump(ENV *env, const char *ofile, FILE *fp)
{
	IENV *ienv;
	WT_TOC *toc;
	int do_close;

	ienv = env->ienv;

	WT_RET(__wt_diag_set_fp(ofile, &fp, &do_close));

	fprintf(fp, "%s\n", ienv->sep);
	TAILQ_FOREACH(toc, &ienv->tocqh, q) {
		fprintf(fp, "toc: %lx {\n\tapi_gen: %lu, serialize: ",
		    WT_ADDR_TO_ULONG(toc), toc->api_gen);
		if (toc->serialize == NULL)
			fprintf(fp, "none");
		else if (toc->serialize == WT_TOC_WAITER)
			fprintf(fp, "WAIT");
		else
			fprintf(fp, "%lx (%lu)",
			    WT_ADDR_TO_ULONG(toc->serialize),
			    (u_long)*toc->serialize_private);
		fprintf(fp, "\n}");
		if (toc->name != NULL)
			fprintf(fp, " %s", toc->name);
		fprintf(fp, "\n");
	}
	if (do_close)
		(void)fclose(fp);
	return (0);
}
#endif
