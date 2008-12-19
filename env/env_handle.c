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
int
wt_env_create(ENV **envp, u_int32_t flags)
{
	static int build_verified = 0;
	ENV *env;
	IENV *ienv;
	int ret;

	/*
	 * No matter what we're doing, we end up here before we do any
	 * real work.   The very first time, check the build itself.
	 */
	if (!build_verified) {
		if ((ret = __wt_env_build_verify()) != 0)
			return (ret);
		build_verified = 1;
	}

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

	/* Connect everything together. */
	env->ienv = ienv;
	ienv->env = env;

	ENV_FLAG_CHK_NOTFATAL(
	    ienv, "wt_env_create", flags, WT_APIMASK_WT_ENV_CREATE, ret);
	if (ret != 0)
		goto err;

	__wt_env_config_default(env);
	__wt_env_config_methods(env);

	*envp = env;
	return (0);

err:	__wt_free(NULL, env->ienv);
	__wt_free(NULL, env);
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

	ENV_FLAG_CHK_NOTFATAL(
	    ienv, "Env.destroy", flags, WT_APIMASK_ENV_DESTROY, ret);

	/*
	 * !!!
	 * For part of this function we don't have a valid IENV structure
	 * when calling other library functions.  The only such functions
	 * that can handle a NULL IENV structure are the memory allocation
	 * and free functions, no other functions may be called.
	 *
	 * Discard the underlying IENV structure.
	 */
	__wt_ienv_destroy(env, 0);

	memset(env, OVERWRITE_BYTE, sizeof(env));
	__wt_free(NULL, env);

	return (ret);
}

/*
 * __wt_ienv_destroy --
 *	Destroy the ENV's underlying IENV structure.
 */
void
__wt_ienv_destroy(ENV *env, int refresh)
{
	IENV *ienv;

	ienv = env->ienv;

	/* Free the actual structure. */
	__wt_free(NULL, ienv);
	env->ienv = NULL;

	if (!refresh)
		return;

	/*
	 * Allocate a new IENV structure on request.
	 *
	 * This is the guts of the split between the public/private, ENV/IENV
	 * handles.  If an Env.open fails for any reason, the user may use the
	 * ENV structure again, but the IENV structure may have been modified
	 * in the attempt.  So, we swap out the IENV structure for a new one.
	 * This requires three things:
	 *
	 * 1)	the IENV structure is never touched by any ENV configuration,
	 *	we'd lose it here;
	 * 2)	if this fails for any reason, there's no way back, kill the
	 *	ENV handle itself;
	 * 3)	our caller can't depend on an IENV handle existing after we
	 *	return, so this only gets called in a few, nasty error paths,
	 *	immediately before returning to the user.
	 *
	 * !!!
	 * For part of this function we don't have a valid IENV structure
	 * when calling other library functions.  The only such functions
	 * that can handle a NULL IENV structure are the memory allocation
	 * and free functions, no other functions may be called.
	 */
	if (__wt_calloc(NULL, 1, sizeof(IDB), &ienv) != 0)
		__wt_env_config_methods_lockout(env);
	else {
		env->ienv = ienv;
		ienv->env = env;
	}
}

/*
 * __wt_env_config_default --
 *	Set default configuration for a just-created ENV handle.
 */
void
__wt_env_config_default(ENV *env)
{
	LINTQUIET(env);
}

int
__wt_env_lockout_err(ENV *env)
{
	__wt_env_errx(env,
	    "This Env handle has failed for some reason, and can no longer"
	    " be used; the only method permitted on it is Env.destroy");
	return (WT_ERROR);
}
