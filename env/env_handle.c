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
wt_env_create(ENV **envp, WT_TOC *toc, u_int32_t flags)
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
	if ((ret = __wt_calloc(NULL, 1, sizeof(ENV), &env)) != 0)
		return (ret);
	if ((ret = __wt_calloc(NULL, 1, sizeof(IENV), &ienv)) != 0) {
		__wt_free(NULL, env);
		return (ret);
	}

	/* Connect everything together. */
	env->ienv = ienv;
	ienv->env = env;
	env->toc = ienv->toc = toc;

	/* We have an environment -- check the API flags. */
	ENV_FLAG_CHK_NOTFATAL(
	    env, "wt_env_create", flags, WT_APIMASK_WT_ENV_CREATE, ret);
	if (ret != 0)
		goto err;

	/* Configure the ENV and the IENV. */
	if ((ret = __wt_env_config_default(env)) != 0)
		goto err;
	if ((ret = __wt_ienv_config_default(env)) != 0)
		goto err;

	*envp = env;
	return (0);

err:	(void)__wt_env_destroy_int(env, 0);
	return (ret);
}

/*
 * __wt_env_destroy --
 *	Env.destroy method (ENV destructor).
 */
int
__wt_env_destroy(wt_args_env_destroy *argp)
{
	wt_args_env_destroy_unpack;

	return (__wt_env_destroy_int(env, flags));
}

/*
 * __wt_env_destroy_int --
 *	Env.destroy method (ENV destructor), internal version.
 */
int
__wt_env_destroy_int(ENV *env, u_int32_t flags)
{
	int ret, tret;

	ret = 0;

	ENV_FLAG_CHK_NOTFATAL(
	    env, "Env.destroy", flags, WT_APIMASK_ENV_DESTROY, ret);

	/*
	 * !!!
	 * For part of this function we don't have valid ENV/IENV structures
	 * when calling other library functions.  The only such functions
	 * that can handle NULL structures are the memory allocation and free
	 * functions, no other functions may be called.
	 *
	 * Discard the underlying IENV structure.
	 */
	if ((tret = __wt_ienv_destroy(env, 0)) != 0 && ret == 0)
		ret = tret;

	/* Free any allocated memory. */
	WT_FREE_AND_CLEAR(env, env->stats);

	/* Complain if DB handles weren't closed. */
	if (TAILQ_FIRST(&env->dbqh) != NULL) {
		__wt_env_errx(env,
		    "This Env handle has open Db handles attached to it");
		if (ret == 0)
			ret = WT_ERROR;
	}

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
	int ret;

	__wt_env_config_methods(env);

	TAILQ_INIT(&env->dbqh);

	if ((ret = __wt_stat_alloc_env(env, &env->stats)) != 0)
		return (ret);

	if ((ret = env->set_cachesize(env, WT_CACHE_DEFAULT_SIZE)) != 0)
		return (ret);

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

	/* Check that there's something to destroy. */
	if (ienv == NULL)
		return (0);

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
	if ((ret = __wt_ienv_config_default(env)) != 0)
		return (ret);

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
