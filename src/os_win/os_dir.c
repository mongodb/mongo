/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_win_directory_list --
 *	Get a list of files from a directory, MSVC version.
 */
int
__wt_win_directory_list(WT_SESSION_IMPL *session, const char *dir,
    const char *prefix, uint32_t flags, char ***dirlist, u_int *countp)
{
	HANDLE findhandle;
	WIN32_FIND_DATA finddata;
	WT_DECL_ITEM(pathbuf);
	WT_DECL_RET;
	size_t dirallocsz, pathlen;
	u_int count, dirsz;
	bool match;
	char **entries, *path;

	*dirlist = NULL;
	*countp = 0;

	WT_RET(__wt_filename(session, dir, &path));

	pathlen = strlen(path);
	if (path[pathlen - 1] == '\\')
		path[pathlen - 1] = '\0';
	WT_ERR(__wt_scr_alloc(session, pathlen + 3, &pathbuf));
	WT_ERR(__wt_buf_fmt(session, pathbuf, "%s\\*", path));

	findhandle = INVALID_HANDLE_VALUE;
	dirallocsz = 0;
	dirsz = 0;
	entries = NULL;

	findhandle = FindFirstFileA(pathbuf->data, &finddata);
	if (findhandle == INVALID_HANDLE_VALUE)
		WT_ERR_MSG(session, __wt_getlasterror(),
		    "%s: directory-list: FindFirstFile", pathbuf->data);

	count = 0;
	do {
		/*
		 * Skip . and ..
		 */
		if (strcmp(finddata.cFileName, ".") == 0 ||
		    strcmp(finddata.cFileName, "..") == 0)
			continue;

		/* The list of files is optionally filtered by a prefix. */
		match = false;
		if (prefix != NULL &&
		    ((LF_ISSET(WT_DIRLIST_INCLUDE) &&
		    WT_PREFIX_MATCH(finddata.cFileName, prefix)) ||
		    (LF_ISSET(WT_DIRLIST_EXCLUDE) &&
		    !WT_PREFIX_MATCH(finddata.cFileName, prefix))))
			match = true;
		if (prefix == NULL || match) {
			/*
			 * We have a file name we want to return.
			 */
			count++;
			if (count > dirsz) {
				dirsz += WT_DIR_ENTRY;
				WT_ERR(__wt_realloc_def(session,
				    &dirallocsz, dirsz, &entries));
			}
			WT_ERR(__wt_strdup(session,
			    finddata.cFileName, &entries[count - 1]));
		}
	} while (FindNextFileA(findhandle, &finddata) != 0);
	if (count > 0)
		*dirlist = entries;
	*countp = count;

err:	if (findhandle != INVALID_HANDLE_VALUE)
		(void)FindClose(findhandle);
	__wt_free(session, path);
	__wt_scr_free(session, &pathbuf);

	if (ret == 0)
		return (0);

	if (*dirlist != NULL) {
		for (count = dirsz; count > 0; count--)
			__wt_free(session, entries[count]);
		__wt_free(session, entries);
	}

	WT_RET_MSG(session, ret,
	    "%s: directory-list, prefix \"%s\"",
	    dir, prefix == NULL ? "" : prefix);
}
