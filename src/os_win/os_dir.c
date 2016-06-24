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
__wt_win_directory_list(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, const char *directory,
    const char *prefix, char ***dirlistp, uint32_t *countp)
{
	DWORD windows_error;
	HANDLE findhandle;
	WIN32_FIND_DATA finddata;
	WT_DECL_ITEM(pathbuf);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	size_t dirallocsz, pathlen;
	uint32_t count;
	char *dir_copy, **entries;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	*dirlistp = NULL;
	*countp = 0;

	findhandle = INVALID_HANDLE_VALUE;
	dirallocsz = 0;
	entries = NULL;

	WT_ERR(__wt_strdup(session, directory, &dir_copy));
	pathlen = strlen(dir_copy);
	if (dir_copy[pathlen - 1] == '\\')
		dir_copy[pathlen - 1] = '\0';
	WT_ERR(__wt_scr_alloc(session, pathlen + 3, &pathbuf));
	WT_ERR(__wt_buf_fmt(session, pathbuf, "%s\\*", dir_copy));

	findhandle = FindFirstFileA(pathbuf->data, &finddata);
	if (findhandle == INVALID_HANDLE_VALUE) {
		windows_error = __wt_getlasterror();
		__wt_errx(session,
		    "%s: directory-list: FindFirstFile: %s",
		    pathbuf->data, __wt_formatmessage(session, windows_error));
		WT_ERR(__wt_map_windows_error(windows_error));
	}

	count = 0;
	do {
		/*
		 * Skip . and ..
		 */
		if (strcmp(finddata.cFileName, ".") == 0 ||
		    strcmp(finddata.cFileName, "..") == 0)
			continue;

		/* The list of files is optionally filtered by a prefix. */
		if (prefix != NULL &&
		    !WT_PREFIX_MATCH(finddata.cFileName, prefix))
			continue;

		WT_ERR(__wt_realloc_def(
		    session, &dirallocsz, count + 1, &entries));
		WT_ERR(__wt_strdup(
		    session, finddata.cFileName, &entries[count]));
		++count;
	} while (FindNextFileA(findhandle, &finddata) != 0);

	*dirlistp = entries;
	*countp = count;

err:	if (findhandle != INVALID_HANDLE_VALUE)
		if (FindClose(findhandle) == 0) {
			windows_error = __wt_getlasterror();
			__wt_errx(session,
			    "%s: directory-list: FindClose: %s",
			    pathbuf->data,
			    __wt_formatmessage(session, windows_error));
			if (ret == 0)
				ret = __wt_map_windows_error(windows_error);
		}

	__wt_free(session, dir_copy);
	__wt_scr_free(session, &pathbuf);

	if (ret == 0)
		return (0);

	WT_TRET(__wt_win_directory_list_free(
	    file_system, wt_session, entries, count));

	WT_RET_MSG(session, ret,
	    "%s: directory-list, prefix \"%s\"",
	    directory, prefix == NULL ? "" : prefix);
}

/*
 * __wt_win_directory_list_free --
 *	Free memory returned by __wt_win_directory_list, Windows version.
 */
int
__wt_win_directory_list_free(WT_FILE_SYSTEM *file_system,
    WT_SESSION *wt_session, char **dirlist, uint32_t count)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(file_system);

	session = (WT_SESSION_IMPL *)wt_session;

	if (dirlist != NULL) {
		while (count > 0)
			__wt_free(session, dirlist[--count]);
		__wt_free(session, dirlist);
	}
	return (0);
}
