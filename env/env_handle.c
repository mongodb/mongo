/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_env_config_default(ENV *);
static int __wt_ienv_config_default(ENV *);

/*
 * __wt_env_create --
 *	ENV constructor.
 */
int
__wt_env_create(u_int32_t flags, ENV **envp)
{
	ENV *env;
	IENV *ienv;
	int ret;

	/*
	 * !!!
	 * We don't yet have valid ENV/IENV structures to use to call other
	 * functions.  The only functions that can handle NULL ENV handles
	 * are the memory allocation and free functions, no other functions
	 * may be called.
	 */
	WT_RET(__wt_calloc(NULL, 1, sizeof(ENV), &env));
	WT_ERR(__wt_calloc(NULL, 1, sizeof(IENV), &ienv));

	/* Connect everything together. */
	env->ienv = ienv;

	/* Set flags. */
	if (LF_ISSET(WT_MEMORY_CHECK))
		F_SET(env, WT_MEMORY_CHECK);

	/* Configure the ENV and the IENV. */
	WT_ERR(__wt_env_config_default(env));
	WT_ERR(__wt_ienv_config_default(env));

	*envp = env;
	return (0);

err:	(void)__wt_env_close(env);
	return (ret);
}

/*
 * __wt_env_config_default --
 *	Set default configuration for a just-created ENV handle.
 */
static int
__wt_env_config_default(ENV *env)
{
	env->cache_size = WT_CACHE_SIZE_DEFAULT;
	env->hazard_size = WT_HAZARD_SIZE_DEFAULT;
	env->toc_size = WT_TOC_SIZE_DEFAULT;

	__wt_methods_env_lockout(env);
	__wt_methods_env_init_transition(env);
	return (0);
}


/*
 * __wt_ienv_config_default --
 *	Set default configuration for a just-created IENV handle.
 */
static int
__wt_ienv_config_default(ENV *env)
{
	IENV *ienv;

	ienv = env->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* If we're tracking memory, initialize those structures first. */
	if (F_ISSET(env, WT_MEMORY_CHECK))
		WT_RET(__wt_mtrack_alloc(env));
#endif
						/* Global mutex */
	WT_RET(__wt_mtx_alloc(env, "IENV", 0, &ienv->mtx));

	TAILQ_INIT(&ienv->dbqh);		/* DB list */
	TAILQ_INIT(&ienv->fhqh);		/* File list */

	/* WT_TOC and hazard arrays. */
	WT_RET(__wt_calloc(env, env->toc_size, sizeof(WT_TOC *), &ienv->toc));
	WT_RET(
	    __wt_calloc(env, env->toc_size, sizeof(WT_TOC), &ienv->toc_array));
	WT_RET(__wt_calloc(env,
	   env->toc_size * env->hazard_size, sizeof(WT_PAGE *), &ienv->hazard));

	TAILQ_INIT(&ienv->flistq);		/* Free memory list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_env_stats(env, &ienv->stats));
	WT_RET(__wt_stat_alloc_method_stats(env, &ienv->method_stats));

	/* Diagnostic output separator. */
	ienv->sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=";

	return (0);
}

/*
 * __wt_ienv_destroy --
 *	Destroy the ENV's underlying IENV structure.
 */
int
__wt_ienv_destroy(ENV *env, int refresh)
{
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	/* Check there's something to destroy. */
	if (ienv == NULL)
		return (0);

	/* Diagnostic check: check flags against approved list. */
	WT_ENV_FCHK_RET(env, "Env.close", ienv->flags, WT_APIMASK_IENV, ret);

	(void)__wt_mtx_destroy(env, ienv->mtx);

	/* Free allocated memory. */
	__wt_free(env, ienv->toc, 0);
	__wt_free(env, ienv->toc_array, 0);
	__wt_free(env, ienv->hazard, 0);
	__wt_free(env, ienv->stats, 0);
	__wt_free(env, ienv->method_stats, 0);

#ifdef HAVE_DIAGNOSTIC
	/* If we're tracking memory, check to see if everything was free'd. */
	__wt_mtrack_dump(env);
	__wt_mtrack_free(env);
#endif

	/*
	 * This is the guts of the split between the public/private, ENV/IENV
	 * handles.  If an Env.open fails for any reason, the user may use the
	 * ENV structure again, but the IENV structure may have been modified
	 * in the attempt.  So, we overwrite the IENV structure, as if it was
	 * just allocated.  This requires the IENV structure never be modified
	 * by ENV configuration, we'd lose that configuration here.
	 */
	if (refresh) {
		memset(ienv, 0, sizeof(ienv));
		WT_RET(__wt_ienv_config_default(env));
		return (ret);
	}

	/* If we're truly done, discard the actual memory. */
	__wt_free(NULL, ienv, sizeof(IENV));
	env->ienv = NULL;
	return (0);
}
