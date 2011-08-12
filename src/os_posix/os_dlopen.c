/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_dlopen --
 *	Open a dynamic library.
 */
int
__wt_dlopen(WT_SESSION_IMPL *session, const char *path, WT_DLH **dlhp)
{
	WT_DLH *dlh;
	int ret;

	WT_RET(__wt_calloc_def(session, 1, &dlh));
	WT_ERR(__wt_strdup(session, path, &dlh->name));

	if ((dlh->handle = dlopen(path, RTLD_LAZY)) == NULL) {
		__wt_errx(session, "dlopen(%s): %s", path, dlerror());
		ret = WT_ERROR;
		goto err;
	}

	*dlhp = dlh;
	if (0) {
err:		__wt_free(session, dlh->name);
		__wt_free(session, dlh);
	}
	return (ret);
}

/*
 * __wt_dlsym --
 *	Lookup a symbol in a dynamic library.
 */
int
__wt_dlsym(
    WT_SESSION_IMPL *session, WT_DLH *dlh, const char *name, void *sym_ret)
{
	void *sym;

	if ((sym = dlsym(dlh->handle, name)) == NULL) {
		__wt_err(session, errno, "dlsym(%s in %s): %s",
		    name, dlh->name, dlerror());
		return (WT_ERROR);
	}

	*(void **)sym_ret = sym;
	return (0);
}

/*
 * __wt_dlclose --
 *	Close a dynamic library
 */
int
__wt_dlclose(WT_SESSION_IMPL *session, WT_DLH *dlh)
{
	int ret;

	ret = 0;

	if (dlclose(dlh->handle) != 0) {
		__wt_err(session, errno, "dlclose: %s", dlerror());
		ret = WT_ERROR;
	}

	__wt_free(session, dlh->name);
	__wt_free(session, dlh);
	return (ret);
}
