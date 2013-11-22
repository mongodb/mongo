/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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

/* Assign the src config to the dest.
 * Any storage allocated in dest is freed as a result.
 */
int
config_assign(CONFIG *dest, const CONFIG *src)
{
	size_t i, len;
	char *newstr, **pstr;

	config_free(dest);
	memcpy(dest, src, sizeof(CONFIG));

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

/* Free any storage allocated in the config struct.
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
			if (*pstr != NULL) {
				free(*pstr);
				*pstr = NULL;
			}
		}

	free(cfg->uri);
}

/*
 * Check a single key=value returned by the config parser
 * against our table of valid keys, along with the expected type.
 * If everything is okay, set the value.
 */
static int
config_opt(CONFIG *cfg, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	CONFIG_OPT *popt;
	char *newstr, **strp;
	size_t i, nopt;
	uint64_t newlen;
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

/* Parse a configuration file.
 * We recognize comments '#' and continuation via lines ending in '\'.
 */
int
config_opt_file(CONFIG *cfg, WT_SESSION *parse_session, const char *filename)
{
	FILE *fp;
	size_t linelen, optionpos;
	int contline, linenum, ret;
	char line[256], option[1024];
	char *comment, *ltrim, *rtrim;

	if ((fp = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "wtperf: %s: %s\n", filename, strerror(errno));
		return (errno);
	}

	ret = 0;
	optionpos = 0;
	linenum = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
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
			if ((ret = config_opt_line(cfg,
				    parse_session, option)) != 0) {
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
	}

	(void)fclose(fp);
	return (ret);
}

/* Parse a single line of config options.
 * Continued lines have already been joined.
 */
int
config_opt_line(CONFIG *cfg, WT_SESSION *parse_session, const char *optstr)
{
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;
	WT_CONNECTION *conn;
	WT_EXTENSION_API *wt_api;
	int ret, t_ret;

	conn = parse_session->connection;
	wt_api = conn->get_extension_api(conn);

	if ((ret = wt_api->config_scan_begin(wt_api, parse_session, optstr,
	    strlen(optstr), &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

	while (ret == 0) {
		if ((ret =
		    wt_api->config_scan_next(wt_api, scan, &k, &v)) != 0) {
			/* Any parse error has already been reported. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			break;
		}
		ret = config_opt(cfg, &k, &v);
	}
	if ((t_ret = wt_api->config_scan_end(wt_api, scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_end");
		if (ret == 0)
			ret = t_ret;
	}

	return (ret);
}

/* Set a single string config option */
int
config_opt_str(CONFIG *cfg, WT_SESSION *parse_session,
    const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name="value" */
	if ((optstr = malloc(strlen(name) + strlen(value) + 4)) == NULL)
		return (enomem(cfg));
	sprintf(optstr, "%s=\"%s\"", name, value);
	ret = config_opt_line(cfg, parse_session, optstr);
	free(optstr);
	return (ret);
}

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
		typestr = "?";
		defaultval = config_opts[i].defaultval;
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
			typestr = "string";
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

int
config_sanity(CONFIG *cfg)
{
	/* Run-time or operation count, must be one or the other. */
	if ((cfg->run_time == 0 && cfg->run_ops == 0) ||
	    (cfg->run_time != 0 && cfg->run_ops != 0)) {
		fprintf(stderr,
		    "one of either run-time or run-ops must be set, "
		    "but not both\n");
		return (EINVAL);
	}

	/* Various intervals should be less than the run-time. */
	if (cfg->run_time > 0 &&
	    (cfg->checkpoint_interval > cfg->run_time ||
	    cfg->report_interval > cfg->run_time ||
	    cfg->sample_interval > cfg->run_time)) {
		fprintf(stderr, "interval value longer than the run-time\n");
		return (EINVAL);
	}

	/* Job mix shouldn't be more than 100%. */
	if (cfg->run_mix_inserts + cfg->run_mix_updates > 100) {
		fprintf(stderr,
		    "job mix percentages cannot be more than 100\n");
		return (EINVAL);
	}
	return (0);
}

void
config_print(CONFIG *cfg)
{
	printf("Workload configuration:\n");
	printf("\thome: %s\n", cfg->home);
	printf("\ttable_name: %s\n", cfg->table_name);
	printf("\tConnection configuration: %s\n", cfg->conn_config);
	printf("\tTable configuration: %s (%s)\n",
	    cfg->table_config, cfg->create ? "creating new" : "using existing");
	printf("\tWorkload period/operations: %" PRIu32 "/%" PRIu32 "\n",
	    cfg->run_time, cfg->run_ops);
	printf(
	    "\tCheckpoint interval: %" PRIu32 "\n", cfg->checkpoint_interval);
	printf("\tReporting interval: %" PRIu32 "\n", cfg->report_interval);
	printf("\tSampling interval: %" PRIu32 "\n", cfg->sample_interval);
	if (cfg->create) {
		printf("\tInsert count: %" PRIu32 "\n", cfg->icount);
		printf("\tNumber populate threads: %" PRIu32 "\n",
		    cfg->populate_threads);
	}
	if (cfg->run_mix_inserts == 0 && cfg->run_mix_updates == 0) {
		printf("\tNumber read threads: %" PRIu32 "\n",
		    cfg->read_threads);
		printf(
		    "\tNumber insert threads: %" PRIu32 "%s\n",
		    cfg->insert_threads,
		    cfg->insert_rmw ? " (inserts are RMW)" : "");
		printf("\tNumber update threads: %" PRIu32 "\n",
		    cfg->update_threads);
	} else
		printf("\tOperation mix is %"
		    PRIu32 "reads, %" PRIu32 " inserts, %" PRIu32 " updates\n",
		    100 - (cfg->run_mix_inserts + cfg->run_mix_updates),
		    cfg->run_mix_inserts, cfg->run_mix_updates);
	printf("\tkey size: %" PRIu32 " data size: %" PRIu32 "\n",
	    cfg->key_sz, cfg->data_sz);
	printf("\tVerbosity: %" PRIu32 "\n", cfg->verbose);
}

void
usage(void)
{
	printf("wtperf [-LMSv] [-C config] "
	    "[-h home] [-O file] [-o option] [-T config]\n");
	printf("\t-L Use a large default configuration\n");
	printf("\t-M Use a medium default configuration\n");
	printf("\t-S Use a small default configuration\n");
	printf("\t-C <string> additional connection configuration\n");
	printf("\t            (added to option conn_config)\n");
	printf("\t-h <string> Wired Tiger home must exist, default WT_TEST\n");
	printf("\t-O <file> file contains options as listed below\n");
	printf("\t-o option=val[,option=val,...] set options listed below\n");
	printf("\t-T <string> additional table configuration\n");
	printf("\t            (added to option table_config)\n");
	printf("\n");
	config_opt_usage();
}
