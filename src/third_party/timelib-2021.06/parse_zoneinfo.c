/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2018 MongoDB, Inc.
 * Copyright (c) 2015 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "timelib.h"
#include "timelib_private.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#ifdef _WIN32
# include "win_dirent.h"
#endif

#ifndef MAXPATHLEN
# ifdef _WIN32
#  define MAXPATHLEN 2048
# elif PATH_MAX
#  define MAXPATHLEN PATH_MAX
# elif defined(MAX_PATH)
#  define MAXPATHLEN MAX_PATH
# else
#  define MAXPATHLEN 256
# endif
#endif

#if _MSC_VER
# define TIMELIB_DIR_SEPARATOR "\\"
#else
# define TIMELIB_DIR_SEPARATOR "/"
#endif

#define TIMELIB_NAME_SEPARATOR "/"

/* Filter out some non-tzdata files and the posix/right databases, if
 * present. */
static int index_filter(const struct dirent *ent)
{
	return strcmp(ent->d_name, ".") != 0
		&& strcmp(ent->d_name, "..") != 0
		&& strcmp(ent->d_name, "posix") != 0
		&& strcmp(ent->d_name, "posixrules") != 0
		&& strcmp(ent->d_name, "right") != 0
		&& strcmp(ent->d_name, "Etc") != 0
		&& strcmp(ent->d_name, "localtime") != 0
		&& strstr(ent->d_name, ".list") == NULL
		&& strstr(ent->d_name, ".tab") == NULL;
}

static int timelib_alphasort(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}


static int sysdbcmp(const void *first, const void *second)
{
	const timelib_tzdb_index_entry *alpha = first, *beta = second;

	return timelib_strcasecmp(alpha->id, beta->id);
}

/* Returns true if the passed-in stat structure describes a
 * probably-valid timezone file. */
static int is_valid_tzfile(const struct stat *st, int fd)
{
	if (fd) {
		char buf[20];
		if (read(fd, buf, 20) != 20) {
			return 0;
		}
		lseek(fd, SEEK_SET, 0);
		if (memcmp(buf, "TZif", 4)) {
			return 0;
		}
	}
	return S_ISREG(st->st_mode) && st->st_size > 20;
}

/* Return the contents of the tzfile if found, else NULL.  On success, the
 * length of the mapped data is placed in *length. */
static char *read_tzfile(const char *directory, const char *timezone, size_t *length)
{
	char *fname;
	size_t fname_len;
	char *buffer;
	struct stat st;
	int fd;
#ifdef _WIN32
	int read_bytes;
#else
	ssize_t read_bytes;
#endif

	if (timezone[0] == '\0' || strstr(timezone, "..") != NULL) {
		return NULL;
	}

	fname_len = strlen(directory) + strlen(TIMELIB_DIR_SEPARATOR) + strlen(timezone) + 1;
	fname = timelib_malloc(fname_len);
	if (snprintf(fname, fname_len, "%s%s%s", directory, TIMELIB_DIR_SEPARATOR, timezone) < 0) {
		timelib_free(fname);
		return NULL;
	}

	/* O_BINARY is required to properly read the file on windows */
#ifdef _WIN32
	fd = open(fname, O_RDONLY | O_BINARY);
#else
	fd = open(fname, O_RDONLY);
#endif
	timelib_free(fname);

	if (fd == -1) {
		return NULL;
	} else if (fstat(fd, &st) != 0 || !is_valid_tzfile(&st, fd)) {
		close(fd);
		return NULL;
	}

	*length = st.st_size;

	buffer = timelib_calloc(1, *length + 1);
	read_bytes = read(fd, buffer, *length);
	close(fd);

	if (read_bytes == -1 || read_bytes != st.st_size) {
		return NULL;
	}

	return buffer;
}

static int timelib_scandir(const char *directory_name, struct dirent ***namelist, int (*filter)(const struct dirent *), int (*compar)(const struct dirent **, const struct dirent **))
{
	DIR *dir;
	struct dirent **entries = NULL;
	int entries_size = 0;
	int entries_count = 0;
	char entry_container[sizeof(struct dirent) + MAXPATHLEN];
	struct dirent *entry = (struct dirent *)&entry_container;

	dir = opendir(directory_name);
	if (!dir) {
		return -1;
	}

	while ((entry = readdir(dir)) != NULL) {
		int new_entry_size = 0;
		struct dirent *new_entry = NULL;

		/* Skip if the filter matches */
		if (filter && (*filter)(entry) == 0) {
			continue;
		}

		/* Make sure our entries array is large enough */
		if (entries_count == entries_size) {
			struct dirent **new_entries;

			if (entries_size == 0) {
				entries_size = 16;
			} else {
				entries_size *= 2;
			}

			new_entries = (struct dirent **) timelib_realloc(entries, entries_size * sizeof(struct dirent *));
			if (!new_entries) {
				return -1;
			}
			entries = new_entries;
		}

		/* Add our new entry to the list */
		new_entry_size = sizeof(struct dirent) + (strlen(entry->d_name) + 1);
		new_entry = (struct dirent *) timelib_malloc(new_entry_size);

		if (new_entry == NULL) {
			goto cleanup;
		}

		entries[entries_count++] = (struct dirent *) memcpy(new_entry, entry, new_entry_size);
	}

	closedir(dir);

	*namelist = entries;

	if (compar) {
		qsort(*namelist, entries_count, sizeof(struct dirent *), (int (*) (const void *, const void *)) compar);
	}

	return entries_count;

cleanup:
	while (entries_count-- > 0) {
		timelib_free(entries[entries_count]);
	}
	timelib_free(entries);
	return -1;
}

/* Create the zone identifier index by trawling the filesystem. */
static int create_zone_index(const char *directory, timelib_tzdb *db)
{
	size_t dirstack_size,  dirstack_top;
	size_t index_size, index_next;
	timelib_tzdb_index_entry *db_index;
	char **dirstack;
	size_t data_size = 0;
	unsigned char *tmp_data = NULL;

	/* LIFO stack to hold directory entries to scan; each slot is a
	 * directory name relative to the zoneinfo prefix. */
	dirstack_size = 32;
	dirstack = timelib_malloc(dirstack_size * sizeof(*dirstack));
	dirstack_top = 1;
	dirstack[0] = timelib_strdup("");

	/* Index array. */
	index_size = 64;
	db_index = timelib_malloc(index_size * sizeof(timelib_tzdb_index_entry));
	index_next = 0;

	do {
		struct dirent **ents;
		char name[PATH_MAX], *top;
		int count;

		/* Pop the top stack entry, and iterate through its contents. */
		top = dirstack[--dirstack_top];
		snprintf(name, sizeof(name), "%s/%s", directory, top);

		count = timelib_scandir(name, &ents, index_filter, timelib_alphasort);
		if (count == -1) {
			timelib_free(dirstack);
			timelib_free(db_index);
			return -errno;
		}

		while (count > 0) {
			struct stat st;
			const char *leaf = ents[count - 1]->d_name;

			snprintf(name, sizeof(name), "%s%s%s%s%s", directory, TIMELIB_NAME_SEPARATOR, top, TIMELIB_NAME_SEPARATOR, leaf);

			if (strlen(name) && stat(name, &st) == 0) {
				/* Name, relative to the zoneinfo prefix. */
				const char *root = top;

				if (root[0] == '/') {
					root++;
				}

				snprintf(name, sizeof(name), "%s%s%s", root, *root ? TIMELIB_NAME_SEPARATOR : "", leaf);

				if (S_ISDIR(st.st_mode)) {
					if (dirstack_top == dirstack_size) {
						dirstack_size *= 2;
						dirstack = timelib_realloc(dirstack, dirstack_size * sizeof(*dirstack));
					}
					dirstack[dirstack_top++] = timelib_strdup(name);
				} else {
					if (index_next == index_size) {
						index_size *= 2;
						db_index = timelib_realloc(db_index, index_size * sizeof(timelib_tzdb_index_entry));
					}

					db_index[index_next].id = timelib_strdup(name);

					{
						size_t length;
						char *tzfile_data = read_tzfile(directory, name, &length);

						if (tzfile_data) {
							tmp_data = timelib_realloc(tmp_data, data_size + length);
							memcpy(tmp_data + data_size, tzfile_data, length);
							db_index[index_next].pos = data_size;
							data_size += length;
							timelib_free(tzfile_data);

							index_next++;
						} else {
							timelib_free(db_index[index_next].id);
						}
					}
				}
			}

			timelib_free(ents[--count]);
		}

		if (count != -1) {
			timelib_free(ents);
		}
		timelib_free(top);
	} while (dirstack_top);

	qsort(db_index, index_next, sizeof(*db_index), sysdbcmp);

	db->index = db_index;
	db->index_size = index_next;
	db->data = tmp_data;

	timelib_free(dirstack);

	return 0;
}

timelib_tzdb *timelib_zoneinfo(const char *directory)
{
	timelib_tzdb *tmp = timelib_malloc(sizeof(timelib_tzdb));

	tmp->version = "0.system";
	tmp->data = NULL;
	if (create_zone_index(directory, tmp) < 0) {
		timelib_free(tmp);
		return NULL;
	}
	return tmp;
}

void timelib_zoneinfo_dtor(timelib_tzdb *tzdb)
{
	int i;

	for (i = 0; i < tzdb->index_size; i++) {
		timelib_free(tzdb->index[i].id);
	}
	timelib_free((timelib_tzdb_index_entry*) tzdb->index);
	timelib_free((char*) tzdb->data);
	timelib_free(tzdb);
}
