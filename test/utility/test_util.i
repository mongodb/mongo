/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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
#include "wt_internal.h"			/* For __wt_XXX */

#ifdef _WIN32
#include "windows_shim.h"
#endif

#ifdef _WIN32
	#define DIR_DELIM '\\'
	#define RM_COMMAND "rd /s /q "
#else
	#define	DIR_DELIM '/'
	#define RM_COMMAND "rm -rf "
#endif

#define	DEFAULT_DIR "WT_TEST"
#define	MKDIR_COMMAND "mkdir "

/*
 * die --
 *	Report an error and quit.
 */
static inline void
testutil_die(int e, const char *fmt, ...)
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
static inline void
testutil_work_dir_from_path(char *buffer, size_t inputSize, const char *dir)
{
	/* If no directory is provided, use the default. */
	if (dir == NULL)
		dir = DEFAULT_DIR;

	if (inputSize < strlen(dir) + 1)
		testutil_die(ENOMEM,
		    "Not enough memory in buffer for directory %s", dir);

	strcpy(buffer, dir);
}

/*
 * testutil_clean_work_dir --
 *	Remove any existing work directories, can optionally fail on error
 */
static inline void
testutil_clean_work_dir(char *dir)
{
	size_t inputSize;
	int ret;
	bool exist;
	char *buffer;

	/* Additional bytes for the Windows rd command. */
	inputSize = strlen(dir) + sizeof(RM_COMMAND);
	if ((buffer = malloc(inputSize)) == NULL)
		testutil_die(ENOMEM, "Failed to allocate memory");

	snprintf(buffer, inputSize, "%s%s", RM_COMMAND, dir);

	exist = 0;
	if ((ret = __wt_exist(NULL, dir, &exist)) != 0)
		testutil_die(ret,
		    "Unable to check if directory exists");
	if (exist == 1 && (ret = system(buffer)) != 0)
		testutil_die(ret,
		    "System call to remove directory failed");
	free(buffer);
}

/*
 * testutil_make_work_dir --
 *	Delete the existing work directory if it exists, then create a new one.
 */
static inline void
testutil_make_work_dir(char *dir)
{
	size_t inputSize;
	int ret;
	char *buffer;

	testutil_clean_work_dir(dir);

	/* Additional bytes for the mkdir command */
	inputSize = strlen(dir) + sizeof(MKDIR_COMMAND);
	if ((buffer = malloc(inputSize)) == NULL)
		testutil_die(ENOMEM, "Failed to allocate memory");

	/* mkdir shares syntax between Windows and Linux */
	snprintf(buffer, inputSize, "%s%s", MKDIR_COMMAND, dir);
	if ((ret = system(buffer)) != 0)
		testutil_die(ret, "directory create call of '%s%s' failed",
		    MKDIR_COMMAND, dir);
	free(buffer);
}
