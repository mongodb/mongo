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

#include "wtperf.h"

/* All options changeable on command line using -o or -O are listed here. */
static CONFIG_OPT config_opts[] = {
#define	OPT_DEFINE_DESC
#include "wtperf_opt.i"
#undef OPT_DEFINE_DESC
};

static int  config_opt(CONFIG *, WT_CONFIG_ITEM *, WT_CONFIG_ITEM *);
static void config_opt_usage(void);

/*
 * STRING_MATCH --
 *	Return if a string matches a bytestring of a specified length.
 */
#undef	STRING_MATCH
#define	STRING_MATCH(str, bytes, len)					\
	(strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0')

/*
 * config_assign --
 *	Assign the src config to the dest, any storage allocated in dest is
 * freed as a result.
 */
int
config_assign(CONFIG *dest, const CONFIG *src)
{
	CONFIG_QUEUE_ENTRY *conf_line, *tmp_line;
	size_t i;
	char *newstr, **pstr;

	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

	if (src->uris != NULL) {
		dest->uris = dcalloc(src->table_count, sizeof(char *));
		for (i = 0; i < src->table_count; i++)
			dest->uris[i] = dstrdup(src->uris[i]);
	}
	dest->ckptthreads = NULL;
	dest->popthreads = NULL;
	dest->workers = NULL;

	if (src->base_uri != NULL)
		dest->base_uri = dstrdup(src->base_uri);
	if (src->workload != NULL) {
		dest->workload = dcalloc(WORKLOAD_MAX, sizeof(WORKLOAD));
		memcpy(dest->workload,
		    src->workload, WORKLOAD_MAX * sizeof(WORKLOAD));
	}

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((u_char *)dest + config_opts[i].offset);
			if (*pstr != NULL) {
				newstr = dstrdup(*pstr);
				*pstr = newstr;
			}
		}

	TAILQ_INIT(&dest->stone_head);
	TAILQ_INIT(&dest->config_head);

	/* Clone the config string information into the new cfg object */
	TAILQ_FOREACH(conf_line, &src->config_head, c) {
		tmp_line = dcalloc(sizeof(CONFIG_QUEUE_ENTRY), 1);
		tmp_line->string = dstrdup(conf_line->string);
		TAILQ_INSERT_TAIL(&dest->config_head, tmp_line, c);
	}
	return (0);
}

/*
 * config_free --
 *	Free any storage allocated in the config struct.
 */
void
config_free(CONFIG *cfg)
{
	CONFIG_QUEUE_ENTRY *config_line;
	size_t i;
	char **pstr;

	while (!TAILQ_EMPTY(&cfg->config_head)) {
		config_line = TAILQ_FIRST(&cfg->config_head);
		TAILQ_REMOVE(&cfg->config_head, config_line, c);
		free(config_line->string);
		free(config_line);
	}

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((unsigned char *)cfg + config_opts[i].offset);
			free(*pstr);
			*pstr = NULL;
		}
	if (cfg->uris != NULL) {
		for (i = 0; i < cfg->table_count; i++)
			free(cfg->uris[i]);
		free(cfg->uris);
	}

	cleanup_truncate_config(cfg);
	free(cfg->ckptthreads);
	free(cfg->popthreads);
	free(cfg->base_uri);
	free(cfg->workers);
	free(cfg->workload);
}

/*
 * config_compress --
 *	Parse the compression configuration.
 */
int
config_compress(CONFIG *cfg)
{
	int ret;
	const char *s;

	ret = 0;
	s = cfg->compression;
	if (strcmp(s, "none") == 0) {
		cfg->compress_ext = NULL;
		cfg->compress_table = NULL;
	} else if (strcmp(s, "bzip") == 0) {
		cfg->compress_ext = BZIP_EXT;
		cfg->compress_table = BZIP_BLK;
	} else if (strcmp(s, "lz4") == 0) {
		cfg->compress_ext = LZ4_EXT;
		cfg->compress_table = LZ4_BLK;
	} else if (strcmp(s, "snappy") == 0) {
		cfg->compress_ext = SNAPPY_EXT;
		cfg->compress_table = SNAPPY_BLK;
	} else if (strcmp(s, "zlib") == 0) {
		cfg->compress_ext = ZLIB_EXT;
		cfg->compress_table = ZLIB_BLK;
	} else {
		fprintf(stderr,
	    "invalid compression configuration: %s\n", s);
		ret = EINVAL;
	}
	return (ret);

}

/*
 * config_threads --
 *	Parse the thread configuration.
 */
static int
config_threads(CONFIG *cfg, const char *config, size_t len)
{
	WORKLOAD *workp;
	WT_CONFIG_ITEM groupk, groupv, k, v;
	WT_CONFIG_PARSER *group, *scan;
	int ret;

	group = scan = NULL;
	if (cfg->workload != NULL) {
		/*
		 * This call overrides an earlier call.  Free and
		 * reset everything.
		 */
		free(cfg->workload);
		cfg->workload = NULL;
		cfg->workload_cnt = 0;
		cfg->workers_cnt = 0;
	}
	/* Allocate the workload array. */
	cfg->workload = dcalloc(WORKLOAD_MAX, sizeof(WORKLOAD));
	cfg->workload_cnt = 0;

	/*
	 * The thread configuration may be in multiple groups, that is, we have
	 * to handle configurations like:
	 *	threads=((count=2,reads=1),(count=8,inserts=2,updates=1))
	 *
	 * Start a scan on the original string, then do scans on each string
	 * returned from the original string.
	 */
	if ((ret =
	    wiredtiger_config_parser_open(NULL, config, len, &group)) != 0)
		goto err;
	while ((ret = group->next(group, &groupk, &groupv)) == 0) {
		if ((ret = wiredtiger_config_parser_open(
		    NULL, groupk.str, groupk.len, &scan)) != 0)
			goto err;

		/* Move to the next workload slot. */
		if (cfg->workload_cnt == WORKLOAD_MAX) {
			fprintf(stderr,
			    "too many workloads configured, only %d workloads "
			    "supported\n",
			    WORKLOAD_MAX);
			return (EINVAL);
		}
		workp = &cfg->workload[cfg->workload_cnt++];

		while ((ret = scan->next(scan, &k, &v)) == 0) {
			if (STRING_MATCH("count", k.str, k.len)) {
				if ((workp->threads = v.val) <= 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("throttle", k.str, k.len)) {
				workp->throttle = (uint64_t)v.val;
				continue;
			}
			if (STRING_MATCH("insert", k.str, k.len) ||
			    STRING_MATCH("inserts", k.str, k.len)) {
				if ((workp->insert = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("ops_per_txn", k.str, k.len)) {
				if ((workp->ops_per_txn = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("read", k.str, k.len) ||
			    STRING_MATCH("reads", k.str, k.len)) {
				if ((workp->read = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("update", k.str, k.len) ||
			    STRING_MATCH("updates", k.str, k.len)) {
				if ((workp->update = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("truncate", k.str, k.len)) {
				if ((workp->truncate = v.val) != 1)
					goto err;
				/* There can only be one Truncate thread. */
				if (cfg->has_truncate != 0) {
					goto err;
				}
				cfg->has_truncate = 1;
				continue;
			}
			if (STRING_MATCH("truncate_pct", k.str, k.len)) {
				if (v.val <= 0)
					goto err;
				workp->truncate_pct = (uint64_t)v.val;
				continue;
			}
			if (STRING_MATCH("truncate_count", k.str, k.len)) {
				if (v.val <= 0)
					goto err;
				workp->truncate_count = (uint64_t)v.val;
				continue;
			}
			goto err;
		}
		if (ret == WT_NOTFOUND)
			ret = 0;
		if (ret != 0 )
			goto err;
		ret = scan->close(scan);
		scan = NULL;
		if (ret != 0)
			goto err;
		if (workp->insert == 0 && workp->read == 0 &&
		    workp->update == 0 && workp->truncate == 0)
			goto err;
		/* Why run with truncate if we don't want any truncation. */
		if (workp->truncate != 0 &&
		    workp->truncate_pct == 0 && workp->truncate_count == 0)
			goto err;
		if (workp->truncate != 0 &&
		    (workp->truncate_pct < 1 || workp->truncate_pct > 99))
			goto err;
		/* Truncate should have its own exclusive thread. */
		if (workp->truncate != 0 && workp->threads > 1)
			goto err;
		if (workp->truncate != 0 &&
		    (workp->insert > 0 || workp->read > 0 || workp->update > 0))
			goto err;
		cfg->workers_cnt += (u_int)workp->threads;
	}

	ret = group->close(group);
	group = NULL;
	if (ret != 0)
		goto err;

	return (0);

err:	if (group != NULL)
		(void)group->close(group);
	if (scan != NULL)
		(void)scan->close(scan);

	fprintf(stderr,
	    "invalid thread configuration or scan error: %.*s\n",
	    (int)len, config);
	return (EINVAL);
}

/*
 * config_opt --
 *	Check a single key=value returned by the config parser against our table
 * of valid keys, along with the expected type.  If everything is okay, set the
 * value.
 */
static int
config_opt(CONFIG *cfg, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	CONFIG_OPT *popt;
	char *newstr, **strp;
	size_t i, newlen, nopt;
	void *valueloc;

	popt = NULL;
	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++)
		if (strlen(config_opts[i].name) == k->len &&
		    strncmp(config_opts[i].name, k->str, k->len) == 0) {
			popt = &config_opts[i];
			break;
		}
	if (popt == NULL) {
		fprintf(stderr, "wtperf: Error: "
		    "unknown option \'%.*s\'\n", (int)k->len, k->str);
		fprintf(stderr, "Options:\n");
		for (i = 0; i < nopt; i++)
			fprintf(stderr, "\t%s\n", config_opts[i].name);
		return (EINVAL);
	}
	valueloc = ((unsigned char *)cfg + popt->offset);
	switch (popt->type) {
	case BOOL_TYPE:
		if (v->type != WT_CONFIG_ITEM_BOOL) {
			fprintf(stderr, "wtperf: Error: "
			    "bad bool value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case INT_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad int value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val > INT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "int value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case UINT32_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad uint32 value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val < 0 || v->val > UINT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "uint32 value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(uint32_t *)valueloc = (uint32_t)v->val;
		break;
	case CONFIG_STRING_TYPE:
		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		newlen = v->len + 1;
		if (*strp == NULL) {
			newstr = dstrdup(v->str);
		} else {
			newlen += (strlen(*strp) + 1);
			newstr = dcalloc(newlen, sizeof(char));
			snprintf(newstr, newlen,
			    "%s,%*s", *strp, (int)v->len, v->str);
			/* Free the old value now we've copied it. */
			free(*strp);
		}
		*strp = newstr;
		break;
	case STRING_TYPE:
		/*
		 * Thread configuration is the one case where the type isn't a
		 * "string", it's a "struct".
		 */
		if (v->type == WT_CONFIG_ITEM_STRUCT &&
		    STRING_MATCH("threads", k->str, k->len))
			return (config_threads(cfg, v->str, v->len));

		if (v->type != WT_CONFIG_ITEM_STRING) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		free(*strp);
		/*
		 * We duplicate the string to len rather than len+1 as we want
		 * to truncate the trailing quotation mark.
		 */
		newstr = dstrndup(v->str,  v->len);
		*strp = newstr;
		break;
	}
	return (0);
}

/*
 * config_opt_file --
 *	Parse a configuration file.  We recognize comments '#' and continuation
 * via lines ending in '\'.
 */
int
config_opt_file(CONFIG *cfg, const char *filename)
{
	struct stat sb;
	ssize_t read_size;
	size_t buf_size, linelen, optionpos;
	int contline, fd, linenum, ret;
	char option[1024];
	char *comment, *file_buf, *line, *ltrim, *rtrim;

	file_buf = NULL;

	if ((fd = open(filename, O_RDONLY)) == -1) {
		fprintf(stderr, "wtperf: %s: %s\n", filename, strerror(errno));
		return (errno);
	}
	if ((ret = fstat(fd, &sb)) != 0) {
		fprintf(stderr, "wtperf: stat of %s: %s\n",
		    filename, strerror(errno));
		ret = errno;
		goto err;
	}
	buf_size = (size_t)sb.st_size;
	file_buf = dcalloc(buf_size + 2, 1);
	read_size = read(fd, file_buf, buf_size);
	if (read_size == -1
#ifndef _WIN32
	/* Windows automatically translates \r\n -> \n so counts will be off */
	|| (size_t)read_size != buf_size
#endif
	) {
		fprintf(stderr,
		    "wtperf: read unexpected amount from config file\n");
		ret = EINVAL;
		goto err;
	}
	/* Make sure the buffer is terminated correctly. */
	file_buf[read_size] = '\0';

	ret = 0;
	optionpos = 0;
	linenum = 0;
	/*
	 * We should switch this from using strtok to generating a single
	 * WiredTiger configuration string compatible string, and using
	 * the WiredTiger configuration parser to parse it at once.
	 */
#define	WTPERF_CONFIG_DELIMS	"\n\\"
	for (line = strtok(file_buf, WTPERF_CONFIG_DELIMS);
	    line != NULL;
	    line = strtok(NULL, WTPERF_CONFIG_DELIMS)) {
		linenum++;
		/* trim the line */
		for (ltrim = line; *ltrim && isspace(*ltrim); ltrim++)
			;
		rtrim = &ltrim[strlen(ltrim)];
		if (rtrim > ltrim && rtrim[-1] == '\n')
			rtrim--;

		contline = (rtrim > ltrim && rtrim[-1] == '\\');
		if (contline)
			rtrim--;

		comment = strchr(ltrim, '#');
		if (comment != NULL && comment < rtrim)
			rtrim = comment;
		while (rtrim > ltrim && isspace(rtrim[-1]))
			rtrim--;

		linelen = (size_t)(rtrim - ltrim);
		if (linelen == 0)
			continue;

		if (linelen + optionpos + 1 > sizeof(option)) {
			fprintf(stderr, "wtperf: %s: %d: line overflow\n",
			    filename, linenum);
			ret = EINVAL;
			break;
		}
		*rtrim = '\0';
		strncpy(&option[optionpos], ltrim, linelen);
		option[optionpos + linelen] = '\0';
		if (contline)
			optionpos += linelen;
		else {
			if ((ret = config_opt_line(cfg, option)) != 0) {
				fprintf(stderr, "wtperf: %s: %d: parse error\n",
				    filename, linenum);
				break;
			}
			optionpos = 0;
		}
	}
	if (ret == 0 && optionpos > 0) {
		fprintf(stderr, "wtperf: %s: %d: last line continues\n",
		    filename, linenum);
		ret = EINVAL;
		goto err;
	}

err:	if (fd != -1)
		(void)close(fd);
	free(file_buf);
	return (ret);
}

/*
 * config_opt_line --
 *	Parse a single line of config options.  Continued lines have already
 * been joined.
 */
int
config_opt_line(CONFIG *cfg, const char *optstr)
{
	CONFIG_QUEUE_ENTRY *config_line;
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *scan;
	size_t len;
	int ret, t_ret;

	len = strlen(optstr);
	if ((ret = wiredtiger_config_parser_open(
	    NULL, optstr, len, &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

	/*
	 * Append the current line to our copy of the config. The config is
	 * stored in the order it is processed, so added options will be after
	 * any parsed from the original config. We allocate len + 1 to allow for
	 * a null byte to be added.
	 */
	config_line = dcalloc(sizeof(CONFIG_QUEUE_ENTRY), 1);
	config_line->string = dstrdup(optstr);
	TAILQ_INSERT_TAIL(&cfg->config_head, config_line, c);

	while (ret == 0) {
		if ((ret = scan->next(scan, &k, &v)) != 0) {
			/* Any parse error has already been reported. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			break;
		}
		ret = config_opt(cfg, &k, &v);
	}
	if ((t_ret = scan->close(scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_end");
		if (ret == 0)
			ret = t_ret;
	}

	return (ret);
}

/*
 * config_opt_str --
 *	Set a single string config option.
 */
int
config_opt_str(CONFIG *cfg, const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name="value" */
	optstr = dmalloc(strlen(name) + strlen(value) + 4);
	sprintf(optstr, "%s=\"%s\"", name, value);
	ret = config_opt_line(cfg, optstr);
	free(optstr);
	return (ret);
}

/*
 * config_sanity --
 *	Configuration sanity checks.
 */
int
config_sanity(CONFIG *cfg)
{
	/* Various intervals should be less than the run-time. */
	if (cfg->run_time > 0 &&
	    ((cfg->checkpoint_threads != 0 &&
	    cfg->checkpoint_interval > cfg->run_time) ||
	    cfg->report_interval > cfg->run_time ||
	    cfg->sample_interval > cfg->run_time)) {
		fprintf(stderr, "interval value longer than the run-time\n");
		return (EINVAL);
	}
	/* The maximum is here to keep file name construction simple. */
	if (cfg->table_count < 1 || cfg->table_count > 99999) {
		fprintf(stderr,
		    "invalid table count, less than 1 or greater than 99999\n");
		return (EINVAL);
	}
	if (cfg->database_count < 1 || cfg->database_count > 99) {
		fprintf(stderr,
		    "invalid database count, less than 1 or greater than 99\n");
		return (EINVAL);
	}

	if (cfg->pareto > 100) {
		fprintf(stderr,
		    "Invalid pareto distribution - should be a percentage\n");
		return (EINVAL);
	}
	return (0);
}

/*
 * config_consolidate --
 *	Consolidate repeated configuration settings so that it only appears
 *	once in the configuration output file.
 */
void
config_consolidate(CONFIG *cfg)
{
	CONFIG_QUEUE_ENTRY *conf_line, *test_line, *tmp;
	char *string_key;

	/* 
	 * This loop iterates over the config queue and for entry checks if an
	 * entry later in the queue has the same key. If a match is found then
	 * the current queue entry is removed and we continue.
	 */
	conf_line = TAILQ_FIRST(&cfg->config_head);
	while (conf_line != NULL) {
		string_key = strchr(conf_line->string, '=');
		tmp = test_line = TAILQ_NEXT(conf_line, c);
		while (test_line != NULL) {
			/*
			 * The + 1 here forces the '=' sign to be matched
			 * ensuring we don't match keys that have a common
			 * prefix such as "table_count" and "table_count_idle"
			 * as being the same key.
			 */
			if (strncmp(conf_line->string, test_line->string,
			    (size_t)(string_key - conf_line->string + 1))
			    == 0) {
				TAILQ_REMOVE(&cfg->config_head, conf_line, c);
				free(conf_line->string);
				free(conf_line);
				break;
			}
			test_line = TAILQ_NEXT(test_line, c);
		}
		conf_line = tmp;
	}
}

/*
 * config_to_file --
 *	Write the final config used in this execution to a file.
 */
void
config_to_file(CONFIG *cfg)
{
	CONFIG_QUEUE_ENTRY *config_line;
	FILE *fp;
	size_t req_len;
	char *path;

	fp = NULL;

	/* Backup the config */
	req_len = strlen(cfg->home) + strlen("/CONFIG.wtperf") + 1;
	path = dcalloc(req_len, 1);
	snprintf(path, req_len, "%s/CONFIG.wtperf", cfg->home);
	if ((fp = fopen(path, "w")) == NULL) {
		lprintf(cfg, errno, 0, "%s", path);
		goto err;
	}

	/* Print the config dump */
	fprintf(fp,"# Warning. This config includes "
	    "unwritten, implicit configuration defaults.\n"
	    "# Changes to those values may cause differences in behavior.\n");
	config_consolidate(cfg);
	config_line = TAILQ_FIRST(&cfg->config_head);
	while (config_line != NULL) {
		fprintf(fp, "%s\n", config_line->string);
		config_line = TAILQ_NEXT(config_line, c);
	}

err:	free(path);
	if (fp != NULL)
		(void)fclose(fp);
}

/*
 * config_print --
 *	Print out the configuration in verbose mode.
 */
void
config_print(CONFIG *cfg)
{
	WORKLOAD *workp;
	u_int i;

	printf("Workload configuration:\n");
	printf("\t" "Home: %s\n", cfg->home);
	printf("\t" "Table name: %s\n", cfg->table_name);
	printf("\t" "Connection configuration: %s\n", cfg->conn_config);
	if (cfg->sess_config != NULL)
		printf("\t" "Session configuration: %s\n", cfg->sess_config);

	printf("\t%s table: %s\n",
	    cfg->create ? "Creating new" : "Using existing",
	    cfg->table_config);
	printf("\t" "Key size: %" PRIu32 ", value size: %" PRIu32 "\n",
	    cfg->key_sz, cfg->value_sz);
	if (cfg->create)
		printf("\t" "Populate threads: %" PRIu32 ", inserting %" PRIu32
		    " rows\n",
		    cfg->populate_threads, cfg->icount);

	printf("\t" "Workload seconds, operations: %" PRIu32 ", %" PRIu32 "\n",
	    cfg->run_time, cfg->run_ops);
	if (cfg->workload != NULL) {
		printf("\t" "Workload configuration(s):\n");
		for (i = 0, workp = cfg->workload;
		    i < cfg->workload_cnt; ++i, ++workp)
			printf("\t\t%" PRId64 " threads (inserts=%" PRId64
			    ", reads=%" PRId64 ", updates=%" PRId64
			    ", truncates=% " PRId64 ")\n",
			    workp->threads,
			    workp->insert, workp->read,
			    workp->update, workp->truncate);
	}

	printf("\t" "Checkpoint threads, interval: %" PRIu32 ", %" PRIu32 "\n",
	    cfg->checkpoint_threads, cfg->checkpoint_interval);
	printf("\t" "Reporting interval: %" PRIu32 "\n", cfg->report_interval);
	printf("\t" "Sampling interval: %" PRIu32 "\n", cfg->sample_interval);

	printf("\t" "Verbosity: %" PRIu32 "\n", cfg->verbose);
}

/*
 * pretty_print --
 *	Print out lines of text for a 80 character window.
 */
static void
pretty_print(const char *p, const char *indent)
{
	const char *t;

	for (;; p = t + 1) {
		if (strlen(p) <= 70)
			break;
		for (t = p + 70; t > p && *t != ' '; --t)
			;
		if (t == p)			/* No spaces? */
			break;
		printf("%s%.*s\n",
		    indent == NULL ? "" : indent, (int)(t - p), p);
	}
	if (*p != '\0')
		printf("%s%s\n", indent == NULL ? "" : indent, p);
}

/*
 * config_opt_usage --
 *	Configuration usage error message.
 */
static void
config_opt_usage(void)
{
	size_t i, nopt;
	const char *defaultval, *typestr;

	pretty_print(
	    "The following are options settable using -o or -O, showing the "
	    "type and default value.\n", NULL);
	pretty_print(
	    "String values must be enclosed in \" quotes, boolean values must "
	    "be either true or false.\n", NULL);

	nopt = sizeof(config_opts)/sizeof(config_opts[0]);
	for (i = 0; i < nopt; i++) {
		defaultval = config_opts[i].defaultval;
		typestr = "string";
		switch (config_opts[i].type) {
		case BOOL_TYPE:
			typestr = "boolean";
			if (strcmp(defaultval, "0") == 0)
				defaultval = "false";
			else
				defaultval = "true";
			break;
		case CONFIG_STRING_TYPE:
		case STRING_TYPE:
			break;
		case INT_TYPE:
			typestr = "int";
			break;
		case UINT32_TYPE:
			typestr = "unsigned int";
			break;
		}
		printf("%s (%s, default=%s)\n",
		    config_opts[i].name, typestr, defaultval);
		pretty_print(config_opts[i].description, "\t");
	}
}

/*
 * usage --
 *	wtperf usage print, no error.
 */
void
usage(void)
{
	printf("wtperf [-C config] "
	    "[-H mount] [-h home] [-O file] [-o option] [-T config]\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-H <mount> configure Helium volume mount point\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-O <file> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\n");
	config_opt_usage();
}
