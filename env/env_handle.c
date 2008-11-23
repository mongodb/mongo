/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * wt_env_create --
 *	ENV constructor.
 */
wt_env_create(ENV **envp, u_int32_t flags)
{
	ENV *env;
	IENV *ienv;
	int ret;

	/*
	 * !!!
	 * For part of this function we don't have a valid IENV structure
	 * when calling other library functions.  The only such functions
	 * that can handle a NULL IENV structure are the memory allocation
	 * and free functions, no other functions may be called.
	 */
	if ((ret = __wt_calloc(NULL, 1, sizeof(IENV), &ienv)) != 0)
		return (ret);
	if ((ret = __wt_calloc(ienv, 1, sizeof(ENV), &env)) != 0) {
		__wt_free(NULL, ienv);
		return (ret);
	}

	API_FLAG_CHK(
	    ienv, "env_create", flags, WT_APIMASK_WT_ENV_CREATE);

	/* Connect everything together. */
	env->ienv = ienv;
	ienv->env = env;

	/* Initialize getter/setters. */
	env->get_errcall = __wt_env_get_errcall;
	env->get_errfile = __wt_env_get_errfile;
	env->get_errpfx = __wt_env_get_errpfx;
	env->set_errcall = __wt_env_set_errcall;
	env->set_errfile = __wt_env_set_errfile;
	env->set_errpfx = __wt_env_set_errpfx;

	/* Initialize handle methods. */
	env->destroy = __wt_env_destroy;
	env->err = __wt_env_err;
	env->errx = __wt_env_errx;

	*envp = env;
	return (0);
}

/*
 * __wt_env_destroy --
 *	Env.destroy method (ENV destructor).
 */
int
__wt_env_destroy(ENV *env, u_int32_t flags)
{
	IENV *ienv;

	ienv = env->ienv;

	/*
	 * !!!
	 * For part of this function we don't have a valid IENV structure
	 * when calling other library functions.  The only such functions
	 * that can handle a NULL IENV structure are the memory allocation
	 * and free functions, no other functions may be called.
	 */
	API_FLAG_CHK_NOTFATAL(
	    ienv, "Env.destroy", flags, WT_APIMASK_ENV_DESTROY);

	__wt_free(NULL, env->ienv);
	__wt_free(NULL, env);
	return (0);
}

/*
 * __wt_env_get_errcall --
 *	Env.get_errcall.
 */
void
__wt_env_get_errcall(ENV *env, void (**cbp)(const ENV *, const char *))
{
	*cbp = env->errcall;
}

/*
 * __wt_env_set_errcall --
 *	Env.set_errcall.
 */
void
__wt_env_set_errcall(ENV *env, void (*cb)(const ENV *, const char *))
{
	env->errcall = cb;
}

/*
 * __wt_env_get_errfile --
 *	Env.get_errfile.
 */
void
__wt_env_get_errfile(ENV *env, FILE **fpp)
{
	*fpp = env->errfile;
}

/*
 * __wt_env_set_errfile --
 *	Env.set_errfile.
 */
void
__wt_env_set_errfile(ENV *env, FILE *fp)
{
	env->errfile = fp;
}

/*
 * __wt_env_get_errpfx --
 *	Env.get_errpfx.
 */
void
__wt_env_get_errpfx(ENV *env, const char **pfxp)
{
	*pfxp = env->errpfx;
}

/*
 * __wt_env_set_errpfx --
 *	Env.set_errpfx.
 */
void
__wt_env_set_errpfx(ENV *env, const char *pfx)
{
	env->errpfx = pfx;
}

