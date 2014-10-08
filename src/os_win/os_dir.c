/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_dirlist --
 *	Get a list of files from a directory, optionally filtered by
 *	a given prefix.
 */
int
__wt_dirlist(WT_SESSION_IMPL *session, const char *dir, const char *prefix,
    uint32_t flags, char ***dirlist, u_int *countp)
{
	WT_DECL_RET;
	WIN32_FIND_DATA finddata;
	HANDLE findhandle = INVALID_HANDLE_VALUE;
	size_t dirallocsz;
	u_int count, dirsz;
	int match;
	char **entries, *path;
	int pathlen;

	count = 0;
	*dirlist = NULL;
	*countp = 0;

	WT_RET(__wt_filename(session, dir, &path));

	pathlen = strlen(path);
	if (path[pathlen - 1] == '\\') {
		path[pathlen - 1] = '\0';
	}

	dirallocsz = 0;
	dirsz = 0;
	entries = NULL;
	if (flags == 0)
	    LF_SET(WT_DIRLIST_INCLUDE);

	WT_ERR(__wt_verbose(session, WT_VERB_FILEOPS,
	    "wt_dirlist of %s %s prefix %s",
	    path, LF_ISSET(WT_DIRLIST_INCLUDE) ? "include" : "exclude",
	    prefix == NULL ? "all" : prefix));

	findhandle = FindFirstFile(path, &finddata);

	if (INVALID_HANDLE_VALUE == findhandle)
		WT_ERR_MSG(session, __wt_errno(), "%s: FindFirstFile", path);
	else {
		do {
			/*
			 * Skip . and ..
			 */
			if (strcmp(finddata.cFileName, ".") == 0 ||
			    strcmp(finddata.cFileName, "..") == 0)
				continue;
			match = 0;
			if (prefix != NULL &&
			    ((LF_ISSET(WT_DIRLIST_INCLUDE) &&
			    WT_PREFIX_MATCH(finddata.cFileName, prefix)) ||
			    (LF_ISSET(WT_DIRLIST_EXCLUDE) &&
			    !WT_PREFIX_MATCH(finddata.cFileName, prefix))))
				match = 1;
			if (prefix == NULL || match) {
				/*
				 * We have a file name we want to return.
				 */
				count++;
				if (count > dirsz) {
					dirsz += WT_DIR_ENTRY;
					WT_ERR(__wt_realloc_def(
					    session, &dirallocsz, dirsz, &entries));
				}
				WT_ERR(__wt_strdup(
				    session, finddata.cFileName, &entries[count - 1]));
			}
		} while (FindNextFile(findhandle, &finddata) != 0);
	}

	if (count > 0)
		*dirlist = entries;
	*countp = count;
err:
	if (findhandle != NULL)
		(void)FindClose(findhandle);
	__wt_free(session, path);

	if (ret == 0)
		return (0);

	if (*dirlist != NULL) {
		for (count = dirsz; count > 0; count--)
			__wt_free(session, entries[count]);
		__wt_free(session, entries);
	}

	WT_RET_MSG(session, ret, "dirlist %s prefix %s", dir, prefix);
}
