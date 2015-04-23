/*-
 * Public Domain 2014-2015 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wiredtiger.h>
#include "test_util.h"

/*
 * die --
 *	Report an error and quit.
 */
void
die(int e, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (e != 0)
		fprintf(stderr, ": %s", wiredtiger_strerror(e));
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

/*
 * testutil_work_dir_from_path --
 *	Takes a buffer, its size and the intended work directory.
 *	Creates the full intended work directory in buffer.
 */
int
testutil_work_dir_from_path(char *buffer, size_t inputSize, char *dir)
{
	char default_dir[2] = ".";

	if (dir == NULL) {
		dir = default_dir;
	}

	/* 9 bytes for the directory and WT_TEST */
	if (inputSize < strlen(dir) + 9)
		return (1);

	/* Is this windows or *nix? */
#ifdef _WIN32
	snprintf(buffer, inputSize, "%s\\WT_TEST", dir);
#else
	snprintf(buffer, inputSize, "%s/WT_TEST", dir);
#endif
	return (0);
}

/*
 * testutil_clean_work_dir --
 *	Remove any existing work directories
 */
void
testutil_clean_work_dir(char *dir)
{
	char *buffer;
	size_t inputSize;
	int ret;
	/* 10 bytes for the Windows rd command */
	inputSize = strlen(dir) + 19;
	buffer = (char*) malloc (inputSize);

#ifdef _WIN32
	snprintf(buffer, inputSize, "rd /s /q %s", dir);
#else
	snprintf(buffer, inputSize, "rm -rf %s", dir);
#endif
	if ((ret = system(buffer)) != 0)
		die(ret, "directory cleanup call failed");
}

/*
 * testutil_make_work_dir --
 *	Delete the existing work directory if it exists, then create a new one.
 */
void
testutil_make_work_dir(char *dir)
{
	char *buffer;
	size_t inputSize;
	int ret;

	testutil_clean_work_dir(dir);

	/* 7 bytes for the mkdir command */
	inputSize = strlen(dir) + 7;
	buffer = (char*) malloc (inputSize);

	/* mkdir shares syntax between Windows and Linux */
	snprintf(buffer, inputSize, "mkdir %s", dir);
	if ((ret = system(buffer)) != 0)
		die(ret, "directory create call failed");
}
