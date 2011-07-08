/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btconf_read --
 *	Read a file's config string from its ".conf" file.
 */
int
__wt_btconf_read(
    WT_SESSION_IMPL *session, const char *name, const char **configp)
{
	WT_FH *fh;
	off_t filesize;
	size_t namesize;
	char *config, *fullname;
	int ret;

	namesize = strlen(name) + sizeof(".conf");
	WT_RET(__wt_calloc_def(session, namesize, &fullname));
	snprintf(fullname, namesize, "%s.conf", name);

	/* Open the config file. */
	fh = NULL;
	WT_ERR(__wt_open(session, fullname, 0, 0, &fh));
	WT_ERR(__wt_filesize(session, fh, &filesize));
	WT_ERR(__wt_calloc_def(session, (size_t)filesize, &config));
	WT_ERR(__wt_read(session, fh, (off_t)0, (uint32_t)filesize, config));

	/* The file has a newline at the end, replace that with a NUL. */
	config[(uint32_t)filesize - 1] = '\0';

	/* Verify that the string is NUL-terminated. */
	if (config[filesize - 1] != '\0') {
		__wt_errx(session,
		    "Corrupt config file %s: string not NUL-terminated",
		    fullname);
		ret = EINVAL;
		goto err;
	}

	*configp = config;

err:	if (fh != NULL)
		WT_TRET(__wt_close(session, fh));
	__wt_free(session, fullname);
	return (ret);
}

/*
 * __wt_btconf_write --
 *	Write the config string to a ".conf" file.
 */
int
__wt_btconf_write(
    WT_SESSION_IMPL *session, const char *name, const char *config)
{
	WT_FH *fh;
	size_t namesize;
	char *fullname;
	int ret;

	namesize = strlen(name) + sizeof(".conf");
	WT_RET(__wt_calloc_def(session, namesize, &fullname));
	snprintf(fullname, namesize, "%s.conf", name);

        /* There is no truncate mode in __wt_open. */
        if (__wt_exist(fullname))
                WT_ERR(remove(fullname));

	/* Open the config file. */
	fh = NULL;
	WT_ERR(__wt_open(session, fullname, 0666, 1, &fh));

	/* Write the config string and a newline. */
	WT_ERR(__wt_write(session,
	    fh, (off_t)0, (uint32_t)strlen(config), config));
	WT_ERR(__wt_write(session,
	    fh, (off_t)strlen(config), 1, "\n"));

err:	if (fh != NULL)
		WT_TRET(__wt_close(session, fh));
	__wt_free(session, fullname);
	return (ret);
}
