/*
 * Copyright (c) 2019-2020, [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_FILE_UTILS_H_
#define RNP_FILE_UTILS_H_

#include "config.h"
#include <stdint.h>
#include <stdio.h>
#include <dirent.h>
#include <string>

bool    rnp_file_exists(const char *path);
bool    rnp_dir_exists(const char *path);
int64_t rnp_filemtime(const char *path);
int     rnp_open(const char *filename, int oflag, int pmode);
FILE *  rnp_fopen(const char *filename, const char *mode);
FILE *  rnp_fdopen(int fildes, const char *mode);
int     rnp_access(const char *path, int mode);
#if defined(_WIN32) && !defined(HAVE_WIN_STAT)
/* Default to __stat64 structure unless defined by mingw since commit [07e345] */
#define stat __stat64
#endif
int rnp_stat(const char *filename, struct stat *statbuf);
int rnp_rename(const char *oldpath, const char *newpath);
int rnp_unlink(const char *path);

#ifdef _WIN32
#define rnp_closedir _wclosedir
int         rnp_mkdir(const char *path);
_WDIR *     rnp_opendir(const char *path);
std::string rnp_readdir_name(_WDIR *dir);
#else
#define rnp_closedir closedir
DIR *       rnp_opendir(const char *path);
std::string rnp_readdir_name(DIR *dir);
#endif
#ifdef _WIN32
#define RNP_MKDIR(pathname, mode) rnp_mkdir(pathname)
#else
#define RNP_MKDIR(pathname, mode) mkdir(pathname, mode)
#endif

#ifdef _MSC_VER
#define R_OK 4 /* Test for read permission.  */
#define W_OK 2 /* Test for write permission.  */
#define F_OK 0 /* Test for existence.  */
#endif

/** @private
 *  generate a temporary file name based on TMPL.  TMPL must match the
 *  rules for mk[s]temp (i.e. end in "XXXXXX").  The name constructed
 *  does not exist at the time of the call to mkstemp.  TMPL is
 *  overwritten with the result.get the list item at specified index
 *
 *  @param tmpl filename template
 *  @return file descriptor of newly created and opened file, or -1 on error
 **/
int rnp_mkstemp(char *tmpl);

namespace rnp {
namespace path {
inline char separator();
bool        exists(const std::string &path, bool is_dir = false);
bool        empty(const std::string &path);
std::string HOME(const std::string &sdir = "");
std::string append(const std::string &path, const std::string &name);
} // namespace path
} // namespace rnp

#endif
