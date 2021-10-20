/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_absolute_path --
 *     Return if a filename is an absolute path.
 */
bool
__wt_absolute_path(const char *path)
{
    /*
     * https://msdn.microsoft.com/en-us/library/windows/desktop/aa365247
     *
     * For Windows API functions that manipulate files, file names can often
     * be relative to the current directory, while some APIs require a fully
     * qualified path. A file name is relative to the current directory if
     * it does not begin with one of the following:
     *
     * -- A UNC name of any format, which always start with two backslash
     *    characters ("\\").
     * -- A disk designator with a backslash, for example "C:\" or "d:\".
     * -- A single backslash, for example, "\directory" or "\file.txt". This
     *    is also referred to as an absolute path.
     *
     * If a file name begins with only a disk designator but not the
     * backslash after the colon, it is interpreted as a relative path to
     * the current directory on the drive with the specified letter. Note
     * that the current directory may or may not be the root directory
     * depending on what it was set to during the most recent "change
     * directory" operation on that disk.
     *
     * -- "C:tmp.txt" refers to a file named "tmp.txt" in the current
     *    directory on drive C.
     * -- "C:tempdir\tmp.txt" refers to a file in a subdirectory to the
     *    current directory on drive C.
     */
    if (strlen(path) >= 3 && __wt_isalpha(path[0]) && path[1] == ':')
        path += 2;
    return (path[0] == '/' || path[0] == '\\');
}

/*
 * __wt_path_separator --
 *     Return the path separator string.
 */
const char *
__wt_path_separator(void)
{
    return ("\\");
}
