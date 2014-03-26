/*-
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

#include "wtperf.h"

int
enomem(const CONFIG *cfg)
{
	const char *msg;

	msg = "Unable to allocate memory";
	if (cfg->logf == NULL)
		fprintf(stderr, "%s\n", msg);
	else
		lprintf(cfg, ENOMEM, 0, "%s", msg);
	return (ENOMEM);
}

/*
 * setup_path --
 *	Open a file in the monitor directory.
 */
int
setup_path(CONFIG *cfg, const char *suffix, FILE **fpp)
{
	FILE *fp;
	size_t len;
	int ret;
	char *name;

	*fpp = NULL;

	len = strlen(cfg->monitor_dir) + strlen("/") +
	    strlen(cfg->table_name) + strlen(".") + strlen(suffix) + 1;
	if ((name = calloc(len, 1)) == NULL)
		return (enomem(cfg));

	snprintf(
	    name, len, "%s/%s.%s", cfg->monitor_dir, cfg->table_name, suffix);
	if ((fp = fopen(name, "a")) == NULL) {
		ret = errno;
		fprintf(stderr, "%s: %s\n", name, strerror(ret));
	}
	free(name);
	if (fp == NULL)
		return (ret);

	/* Configure line buffering for the file. */
	(void)setvbuf(fp, NULL, _IOLBF, 0);
	*fpp = fp;

	return (0);
}

/* Setup the logging output mechanism. */
int
setup_log_file(CONFIG *cfg)
{
	if (cfg->verbose < 1)
		return (0);

	return (setup_path(cfg, "stat", &cfg->logf));
}

/*
 * conn_stats_print --
 *	Dump out the final connection statistics from the run.
 */
void
conn_stats_print(CONFIG *cfg)
{
	static int report;
	WT_CONNECTION *conn;
	WT_SESSION *session;
	WT_CURSOR *cursor;
	FILE *fp;
	uint64_t v;
	int ret;
	char buf[64];
	const char *pval, *desc;

	snprintf(buf, sizeof(buf), "pstat.%d", ++report);
	if (setup_path(cfg, buf, &fp) != 0)
		return;

	conn = cfg->conn;
	if ((ret = conn->open_session(conn, NULL, NULL, &session)) != 0) {
		lprintf(cfg, ret, 0, "statistics: WT_CONNECTION.open_session");
		return;
	}
	if ((ret = session->open_cursor(session,
	    "statistics:", NULL, NULL, &cursor)) != 0 && ret != EINVAL)
		lprintf(cfg, ret, 0, "statistics: WT_SESSION.open_cursor");
	if (ret != 0)
		return;

	while ((ret = cursor->next(cursor)) == 0 &&
	    (ret = cursor->get_value(cursor, &desc, &pval, &v)) == 0)
		if (fprintf(fp, "%s=%s\n", desc, pval) < 0) {
			lprintf(cfg, errno, 0, "fprintf");
			break;
		}
	if (ret != WT_NOTFOUND)
		lprintf(cfg, ret, 0, "WT_CURSOR.next");

	(void)session->close(session, NULL);
}

/*
 * Log printf - output a log message.
 */
void
lprintf(const CONFIG *cfg, int err, uint32_t level, const char *fmt, ...)
{
	va_list ap;

	if (err == 0 && level <= cfg->verbose) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, "\n");

		if (level < cfg->verbose) {
			va_start(ap, fmt);
			vprintf(fmt, ap);
			va_end(ap);
			printf("\n");
		}
	}
	if (err == 0)
		return;

	/* We are dealing with an error. */
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, " Error: %s\n", wiredtiger_strerror(err));
	if (cfg->logf != NULL) {
		va_start(ap, fmt);
		vfprintf(cfg->logf, fmt, ap);
		va_end(ap);
		fprintf(cfg->logf, " Error: %s\n", wiredtiger_strerror(err));
	}

	/* Never attempt to continue if we got a panic from WiredTiger. */
	if (err == WT_PANIC)
		abort();
}
