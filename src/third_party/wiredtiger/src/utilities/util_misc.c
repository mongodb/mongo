/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT

#ifndef _WIN32
#error "Manual TCMalloc integration supported only on Windows."
#endif

/*
 * Include the TCMalloc header with the "-Wundef" diagnostic flag disabled. Compiling with strict
 * (where the 'Wundef' diagnostic flag is enabled), generates compilation errors where the
 * '__cplusplus' CPP macro is not defined. This being employed by the TCMalloc header to
 * differentiate C & C++ compilation environments. We don't want to define '__cplusplus' when
 * compiling C sources.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#include <gperftools/tcmalloc.h>
#pragma GCC diagnostic pop
#endif

/*
 * util_cerr --
 *     Report an error for a cursor operation.
 */
int
util_cerr(WT_CURSOR *cursor, const char *op, int ret)
{
    return (util_err(cursor->session, ret, "%s: cursor.%s", cursor->uri, op));
}

/*
 * util_err --
 *     Report an error.
 */
int
util_err(WT_SESSION *session, int e, const char *fmt, ...)
{
    va_list ap;

    (void)fprintf(stderr, "%s: ", progname);
    if (fmt != NULL) {
        va_start(ap, fmt);
        (void)vfprintf(stderr, fmt, ap);
        va_end(ap);
        if (e != 0)
            (void)fprintf(stderr, ": ");
    }
    if (e != 0)
        (void)fprintf(
          stderr, "%s", session == NULL ? wiredtiger_strerror(e) : session->strerror(session, e));
    (void)fprintf(stderr, "\n");
    return (1);
}

/*
 * util_read_line --
 *     Read a line from stdin into a ULINE.
 */
int
util_read_line(WT_SESSION *session, ULINE *l, bool eof_expected, bool *eofp)
{
    static uint64_t line = 0;
    size_t len;
    int ch;

    ++line;
    *eofp = false;

    if (l->memsize == 0) {
        if ((l->mem = util_realloc(l->mem, l->memsize + 1024)) == NULL)
            return (util_err(session, errno, NULL));
        l->memsize = 1024;
    }
    for (len = 0;; ++len) {
        if ((ch = getchar()) == EOF) {
            if (len == 0) {
                if (eof_expected) {
                    *eofp = true;
                    return (0);
                }
                return (util_err(session, 0, "line %" PRIu64 ": unexpected end-of-file", line));
            }
            return (util_err(session, 0, "line %" PRIu64 ": no newline terminator", line));
        }
        if (ch == '\n')
            break;
        /*
         * We nul-terminate the string so it's easier to convert the line into a record number, that
         * means we always need one extra byte at the end.
         */
        if (len >= l->memsize - 1) {
            if ((l->mem = util_realloc(l->mem, l->memsize + 1024)) == NULL)
                return (util_err(session, errno, NULL));
            l->memsize += 1024;
        }
        ((uint8_t *)l->mem)[len] = (uint8_t)ch;
    }

    ((uint8_t *)l->mem)[len] = '\0'; /* nul-terminate */

    return (0);
}

/*
 * util_str2num --
 *     Convert a string to a number.
 */
int
util_str2num(WT_SESSION *session, const char *p, bool endnul, uint64_t *vp)
{
    uint64_t v;
    char *endptr;

    /*
     * strtouq takes lots of things like hex values, signs and so on and so forth -- none of them
     * are OK with us. Check the string starts with digit, that turns off the special processing.
     */
    if (!__wt_isdigit((u_char)p[0]))
        goto format;

    errno = 0;
    v = __wt_strtouq(p, &endptr, 0);
    if (v == ULLONG_MAX && errno == ERANGE)
        return (util_err(session, ERANGE, "%s: invalid number", p));

    /*
     * In most cases we expect the number to be a string and end with a nul byte (and we want to
     * confirm that because it's a user-entered command-line argument), but we allow the caller to
     * configure that test off.
     */
    if (endnul && endptr[0] != '\0')
format:
        return (util_err(session, EINVAL, "%s: invalid number", p));

    *vp = v;
    return (0);
}

/*
 * util_flush --
 *     Flush the database successfully, or drop the file.
 */
int
util_flush(WT_SESSION *session, const char *uri)
{
    WT_DECL_RET;
    ret = session->checkpoint(session, NULL);

    if (ret == 0)
        return (0);

    (void)util_err(session, ret, "%s: session.checkpoint", uri);
    if ((ret = session->drop(session, uri, NULL)) != 0)
        (void)util_err(session, ret, "%s: session.drop", uri);
    return (1);
}

/*
 * util_usage --
 *     Display a usage statement.
 */
void
util_usage(const char *usage, const char *tag, const char *list[])
{
    const char **p;

    if (usage != NULL)
        fprintf(stderr, "usage: %s %s %s\n", progname, usage_prefix, usage);
    if (tag != NULL)
        fprintf(stderr, "%s\n", tag);
    if (list != NULL)
        for (p = list; *p != NULL; p += 2)
            fprintf(stderr, "    %s%s%s\n", p[0], strlen(p[0]) > 2 ? "\n        " : "  ", p[1]);
}

/*
 * util_malloc --
 *     Convenience and correctness wrapper for memory allocations. Pairs with util_free.
 */
void *
util_malloc(size_t len)
{
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT
    return (tc_malloc(len));
#else
    return (malloc(len));
#endif
}

/*
 * util_calloc --
 *     Convenience and correctness wrapper for array allocations. Pairs with util_free.
 */
void *
util_calloc(size_t members, size_t sz)
{
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT
    return (tc_calloc(members, sz));
#else
    return (calloc(members, sz));
#endif
}

/*
 * util_realloc --
 *     Convenience and correctness wrapper for memory reallocations. Pairs with util_free.
 */
void *
util_realloc(void *p, size_t len)
{
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT
    return (tc_realloc(p, len));
#else
    return (realloc(p, len));
#endif
}

/*
 * util_free --
 *     Convenience and correctness wrapper for freeing memory. Pairs with
 *     util_malloc/util_realloc/util_calloc.
 */
void
util_free(void *p)
{
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT
    tc_free(p);
#else
    free(p);
#endif
}

/*
 * util_strdup --
 *     Convenience and correctness wrapper for strdup. Free allocated memory with util_free.
 */
char *
util_strdup(const char *s)
{
#ifdef ENABLE_WINDOWS_TCMALLOC_COMMUNITY_SUPPORT
    char *new = util_malloc(strlen(s) + 1);
    if (new == NULL)
        return (NULL);

    strcpy(new, s);

    return (new);
#else
    return (strdup(s));
#endif
}

/*
 * util_open_output_file --
 *     Open custom output file, return `stdout` if no file name provided.
 */
FILE *
util_open_output_file(const char *ofile)
{
    if (ofile == NULL)
        return (stdout);

    return (fopen(ofile, "w"));
}

/*
 * util_close_output_file --
 *     Close custom output file if one was provided.
 */
int
util_close_output_file(FILE *fp)
{
    if (fp != stdout)
        return (fclose(fp));

    return (0);
}
