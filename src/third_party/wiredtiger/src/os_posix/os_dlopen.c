/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_dlopen --
 *     Open a dynamic library.
 */
int
__wt_dlopen(WT_SESSION_IMPL *session, const char *path, WT_DLH **dlhp)
{
    WT_DECL_RET;
    WT_DLH *dlh;

    WT_RET(__wt_calloc_one(session, &dlh));
    WT_ERR(__wt_strdup(session, path == NULL ? "local" : path, &dlh->name));

    if ((dlh->handle = dlopen(path, RTLD_LAZY)) == NULL)
        WT_ERR_MSG(session, __wt_errno(), "dlopen(%s): %s", path, dlerror());

    *dlhp = dlh;
    if (0) {
err:
        __wt_free(session, dlh->name);
        __wt_free(session, dlh);
    }
    return (ret);
}

/*
 * __wt_dlsym --
 *     Lookup a symbol in a dynamic library.
 */
int
__wt_dlsym(WT_SESSION_IMPL *session, WT_DLH *dlh, const char *name, bool fail, void *sym_ret)
{
    void *sym;

    *(void **)sym_ret = NULL;
    if ((sym = dlsym(dlh->handle, name)) == NULL) {
        if (fail)
            WT_RET_MSG(session, __wt_errno(), "dlsym(%s in %s): %s", name, dlh->name, dlerror());
        return (0);
    }

    *(void **)sym_ret = sym;
    return (0);
}

/*
 * __wt_dlclose --
 *     Close a dynamic library
 */
int
__wt_dlclose(WT_SESSION_IMPL *session, WT_DLH *dlh)
{
    WT_DECL_RET;

/*
 * FreeBSD dies inside __cxa_finalize when closing handles.
 *
 * For now, just skip the dlclose: this may leak some resources until the process exits, but that is
 * preferable to hard-to-debug crashes during exit.
 */
#ifndef __FreeBSD__
    if (dlclose(dlh->handle) != 0) {
        ret = __wt_errno();
        __wt_err(session, ret, "dlclose: %s", dlerror());
    }
#endif

    __wt_free(session, dlh->name);
    __wt_free(session, dlh);
    return (ret);
}
