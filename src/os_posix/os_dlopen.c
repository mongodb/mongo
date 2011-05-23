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
__wt_dlopen(SESSION *session, const char *path, WT_DLH **dlhp)
{
	CONNECTION *conn;
	WT_DLH *dlh;
	int ret;

	conn = S2C(session);
	ret = 0;
	WT_RET(__wt_calloc(session, 1, sizeof(WT_DLH), &dlh));

	WT_ERR(__wt_strdup(session, path, &dlh->name));

	if ((dlh->handle = dlopen(path, RTLD_LAZY)) == NULL) {
		__wt_errx(session, "dlopen(%s): %s", path, dlerror());
		ret = WT_ERROR;
		goto err;
	}

	/* Link onto the environment's list of open libraries. */
	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->dlhqh, dlh, q);
	__wt_unlock(session, conn->mtx);

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
__wt_dlsym(SESSION *session, WT_DLH *dlh, const char *name, void **sym_ret)
{
	void *sym;

	if ((sym = dlsym(dlh->handle, name)) == NULL) {
		__wt_err(session, errno, "dlsym(%s in %s): %s",
		    name, dlh->name, dlerror());
		return (WT_ERROR);
	}

	*sym_ret = sym;
	return (0);
}

/*
 * __wt_dlclose --
 *	Close a dynamic library
 */
int
__wt_dlclose(SESSION *session, WT_DLH *dlh)
{
	CONNECTION *conn;
	int ret;

	conn = S2C(session);
	ret = 0;

	if (dlh == NULL)
		return (0);

	/* Remove from the list and discard the memory. */
	__wt_lock(session, conn->mtx);
	TAILQ_REMOVE(&conn->dlhqh, dlh, q);
	__wt_unlock(session, conn->mtx);

	if (dlclose(dlh->handle) != 0) {
		__wt_err(session, errno, "dlclose: %s", dlerror());
		ret = WT_ERROR;
	}

	__wt_free(session, dlh->name);
	__wt_free(session, dlh);
	return (ret);
}
