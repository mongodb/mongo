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
	ienv->env = env;

	/* Configure the ENV and the IENV. */
	WT_ERR(__wt_env_config_default(env));
	WT_ERR(__wt_ienv_config_default(env));

	/* If we're not single-threaded, start the workQ thread. */
	if (LF_ISSET(WT_SINGLE_THREADED))
		F_SET(ienv, WT_SINGLE_THREADED);

	*envp = env;
	return (0);

err:	(void)__wt_api_env_close(env, 0);
	return (ret);
}

/*
 * __wt_env_config_default --
 *	Set default configuration for a just-created ENV handle.
 */
static int
__wt_env_config_default(ENV *env)
{
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
	ienv->env = env;

	/* Initialize the global mutex. */
	WT_RET(__wt_mtx_init(&ienv->mtx));

	TAILQ_INIT(&ienv->tocqh);		/* WT_TOC list */
	TAILQ_INIT(&ienv->dbqh);		/* DB list */
	TAILQ_INIT(&ienv->fhqh);		/* File list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_ienv_stats(env, &ienv->stats));

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

	/* Free allocated memory. */
	__wt_free(env, ienv->stats);

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
	__wt_free(NULL, ienv);
	env->ienv = NULL;
	return (0);
}
