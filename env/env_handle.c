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
 * wt_env_create --
 *	ENV constructor.
 */
int
wt_env_create(u_int32_t flags, ENV **envp)
{
	ENV *env;
	IENV *ienv;
	int ret;

	/*
	 * !!!
	 * For part of this function we don't have valid ENV/IENV structures
	 * when calling other library functions.  The only such functions
	 * that can handle NULL structures are the memory allocation and free
	 * functions, no other functions may be called.
	 */
	WT_RET(__wt_calloc(NULL, 1, sizeof(ENV), &env));
	WT_ERR(__wt_calloc(NULL, 1, sizeof(IENV), &ienv));

	/* Connect everything together. */
	env->ienv = ienv;
	ienv->env = env;

	/* We have an environment -- check the API flags. */
	WT_ENV_FCHK_NOTFATAL(
	    env, "wt_env_create", flags, WT_APIMASK_WT_ENV_CREATE, ret);
	if (ret != 0)
		goto err;

	/* Configure the ENV and the IENV. */
	WT_ERR(__wt_env_config_default(env));
	WT_ERR(__wt_ienv_config_default(env));

	*envp = env;
	return (0);

err:	(void)__wt_env_destroy(env, 0);
	return (ret);
}

/*
 * __wt_env_destroy --
 *	Env.destroy method (ENV destructor).
 */
int
__wt_env_destroy(ENV *env, u_int32_t flags)
{
	IENV *ienv;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_ENV_FCHK_NOTFATAL(
	    env, "Env.destroy", flags, WT_APIMASK_ENV_DESTROY, ret);

	/* Complain if DB handles weren't closed. */
	if (TAILQ_FIRST(&ienv->dbqh) != NULL) {
		__wt_env_errx(env,
		    "This Env handle has open Db handles attached to it");
		if (ret == 0)
			ret = WT_ERROR;
	}

	/* Complain if files weren't closed. */
	if (TAILQ_FIRST(&ienv->fhqh) != NULL) {
		__wt_env_errx(env,
		    "This Env handle has open file handles attached to it");
		/*
		 * BUG
		 * We need a TOC handle here, and then we call __wt_close.
		 */
		if (ret == 0)
			ret = WT_ERROR;
	}

	/*
	 * !!!
	 * For part of this function we don't have valid ENV/IENV structures
	 * when calling other library functions.  The only such functions
	 * that can handle NULL structures are the memory allocation and free
	 * functions, no other functions may be called.
	 *
	 * Discard the underlying IENV structure.
	 */
	WT_TRET(__wt_ienv_destroy(env, 0));

	/* Free the Env structure. */
	memset(env, OVERWRITE_BYTE, sizeof(env));
	__wt_free(NULL, env);

	return (ret);
}

/*
 * __wt_env_config_default --
 *	Set default configuration for a just-created ENV handle.
 */
static int
__wt_env_config_default(ENV *env)
{
	__wt_env_config_methods(env);

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

	ienv = env->ienv;

	/* Check that there's something to destroy. */
	if (ienv == NULL)
		return (0);

	/* Free allocated memory. */
	__wt_free(env, ienv->stats);

	/* If we're truly done, discard the actual memory. */
	if (!refresh) {
		__wt_free(NULL, ienv);
		env->ienv = NULL;
		return (0);
	}

	/*
	 * This is the guts of the split between the public/private, ENV/IENV
	 * handles.  If an Env.open fails for any reason, the user may use the
	 * ENV structure again, but the IENV structure may have been modified
	 * in the attempt.  So, we overwrite the IENV structure, as if it was
	 * just allocated.  This requires the IENV structure never be modified
	 * by ENV configuration, we'd lose that configuration here.
	 */
	memset(ienv, 0, sizeof(ienv));
	WT_RET(__wt_ienv_config_default(env));

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

	/* Initialize the global mutex. */
	WT_RET(__wt_mtx_init(&ienv->mtx));

	TAILQ_INIT(&ienv->dbqh);		/* Database list */
	TAILQ_INIT(&ienv->fhqh);		/* File list */

	/* Statistics. */
	WT_RET(__wt_stat_alloc_ienv_stats(env, &ienv->stats));

	/* Diagnostic output separator. */
	ienv->sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=";

	return (0);
}

int
__wt_env_lockout_err(ENV *env)
{
	__wt_env_errx(env,
	    "This Env handle has failed for some reason, and can no longer "
	    "be used; the only method permitted on it is Env.destroy");
	return (WT_ERROR);
}
