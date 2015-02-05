/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_dlopen --
 *	Open a dynamic library.
 */
int
__wt_dlopen(WT_SESSION_IMPL *session, const char *path, WT_DLH **dlhp)
{
	WT_DECL_RET;
	WT_DLH *dlh;

	WT_RET(__wt_calloc_one(session, &dlh));
	WT_ERR(__wt_strdup(session, path, &dlh->name));

	/* NULL means load from the current binary */
	if (path == NULL) {
		ret = GetModuleHandleExA(0, NULL, &dlh->handle);
		if (ret == FALSE)
			WT_ERR_MSG(session,
			    __wt_errno(), "GetModuleHandleEx(%s): %s", path, 0);
	} else {
		// TODO: load dll here
		DebugBreak();
	}

	/* Windows returns 0 on failure, WT expects 0 on success */
	ret = !ret;

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
__wt_dlsym(WT_SESSION_IMPL *session,
    WT_DLH *dlh, const char *name, int fail, void *sym_ret)
{
	void *sym;

	*(void **)sym_ret = NULL;

	sym = GetProcAddress(dlh->handle, name);
	if (sym == NULL && fail) {
		WT_RET_MSG(session, __wt_errno(),
		    "GetProcAddress(%s in %s): %s", name, dlh->name, 0);
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
	WT_DECL_RET;

	if ((ret = FreeLibrary(dlh->handle)) == FALSE) {
		__wt_err(session, __wt_errno(), "FreeLibrary");
	}

	/* Windows returns 0 on failure, WT expects 0 on success */
	ret = !ret;

	__wt_free(session, dlh->name);
	__wt_free(session, dlh);
	return (ret);
}
