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
#include "../utility/util.h"

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

//Takes a directory as input and returns it with "WT_TEST appended"
char *
testutil_workdir_from_path(char *dir)
{
	char *buffer;
	if (dir == NULL) {
		dir = ".";
	}
	int inputSize = strlen(dir);
	//Alloc space for a new buffer
	buffer = (char*) malloc (inputSize+8);

	/* Is this windows or *nix? */
#ifdef _WIN32
	sprintf(buffer, "%s\\WT_TEST", dir);
#else
	sprintf(buffer, "%s/WT_TEST", dir);
#endif
	printf("returning buffer of %s\n", buffer);
	return (buffer);
}

void
testutil_clean_workdir(char *dir)
{
	char CMD[512];
	int ret;
#ifdef _WIN32
	sprintf(CMD, "rd /s /q %s", dir);
#else
	sprintf(CMD, "rm -rf %s", dir);
#endif
	if ((ret = system(CMD)) != 0)
		die(ret, "directory cleanup call failed");
}

void
testutil_make_workdir(char *dir)
{
	char CMD[512];
	int ret;

	testutil_clean_workdir(dir);

	/* mkdir shares syntax between windows and Linux */
	sprintf(CMD, "mkdir %s", dir);
	if ((ret = system(CMD)) != 0)
		die(ret, "directory create call failed");
}
