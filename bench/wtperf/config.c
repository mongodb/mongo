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
	size_t i, len;
	char *newstr, **pstr;

	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

	if (src->uris != NULL) {
		dest->uris = calloc(src->table_count, sizeof(char *));
		if (dest->uris == NULL)
			return (enomem(dest));
		for (i = 0; i < src->table_count; i++)
			dest->uris[i] = strdup(src->uris[i]);
	}
	dest->ckptthreads = NULL;
	dest->popthreads = NULL;
	dest->workers = NULL;

	if (src->base_uri != NULL)
		dest->base_uri = strdup(src->base_uri);
	if (src->workload != NULL) {
		dest->workload = calloc(WORKLOAD_MAX, sizeof(WORKLOAD));
		if (dest->workload == NULL)
			return (enomem(dest));
		memcpy(dest->workload,
		    src->workload, WORKLOAD_MAX * sizeof(WORKLOAD));
	}

	for (i = 0; i < sizeof(config_opts) / sizeof(config_opts[0]); i++)
		if (config_opts[i].type == STRING_TYPE ||
		    config_opts[i].type == CONFIG_STRING_TYPE) {
			pstr = (char **)
			    ((u_char *)dest + config_opts[i].offset);
			if (*pstr != NULL) {
				len = strlen(*pstr) + 1;
				if ((newstr = malloc(len)) == NULL)
					return (enomem(src));
				strncpy(newstr, *pstr, len);
				*pstr = newstr;
			}
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
	size_t i;
	char **pstr;

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
	/* Allocate the workload array. */
	if ((cfg->workload = calloc(WORKLOAD_MAX, sizeof(WORKLOAD))) == NULL)
		return (enomem(cfg));
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
				if ((workp->throttle = v.val) < 0)
					goto err;
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

		if (workp->insert == 0 &&
		    workp->read == 0 && workp->update == 0)
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
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
			strncpy(newstr, v->str, v->len);
		} else {
			newlen += (strlen(*strp) + 1);
			if ((newstr = calloc(newlen, sizeof(char))) == NULL)
				return (enomem(cfg));
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
		if ((newstr = malloc(v->len + 1)) == NULL)
			return (enomem(cfg));
		strncpy(newstr, v->str, v->len);
		newstr[v->len] = '\0';
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
	file_buf = calloc(buf_size + 2, 1);
	if (file_buf == NULL) {
		ret = ENOMEM;
		goto err;
	}
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
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *scan;
	int ret, t_ret;

	if ((ret = wiredtiger_config_parser_open(
	    NULL, optstr, strlen(optstr), &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

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
	if ((optstr = malloc(strlen(name) + strlen(value) + 4)) == NULL)
		return (enomem(cfg));
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
			    ", reads=%" PRId64 ", updates=%" PRId64 ")\n",
			    workp->threads,
			    workp->insert, workp->read, workp->update);
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
