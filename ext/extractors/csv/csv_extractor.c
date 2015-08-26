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
#include <limits.h>
#include <errno.h>
#include <stdlib.h>

#include <wiredtiger_ext.h>

/*
 * A simple WiredTiger extractor that separates a single string field,
 * interpreted as column separated values (CSV), into component pieces.
 * When an index is configured with this extractor and app_metadata
 * set to a number N, the Nth field is returned as a string.
 *
 * For example, if a value in the primary table is
 *   "Paris,France,CET,2273305"
 * and this extractor is configured with app_metadata=2, then
 * the extractor for this value would return "CET".
 */

/* Local extractor structure. */
typedef struct {
	WT_EXTRACTOR extractor;		/* Must come first */
	WT_EXTENSION_API *wt_api;	/* Extension API */
	int field_num;			/* Field to extract */
} CSV_EXTRACTOR;

/*
 * csv_extract --
 *	WiredTiger CSV extraction.
 */
static int
csv_extract(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const WT_ITEM *key, const WT_ITEM *value, WT_CURSOR *result_cursor)
{
	char *copy, *p, *pend, *valstr;
	const CSV_EXTRACTOR *cvs_extractor;
	int i, ret;
	size_t len;
	WT_EXTENSION_API *wtapi;

	(void)key;				/* Unused parameters */

	cvs_extractor = (const CSV_EXTRACTOR *)extractor;
	wtapi = cvs_extractor->wt_api;

	/* Unpack the value. */
	if ((ret = wtapi->struct_unpack(wtapi,
	    session, value->data, value->size, "S", &valstr)) != 0)
		return (ret);

	p = valstr;
	pend = strchr(p, ',');
	for (i = 0; i < cvs_extractor->field_num && pend != NULL; i++) {
		p = pend + 1;
		pend = strchr(p, ',');
	}
	if (i == cvs_extractor->field_num) {
		if (pend == NULL)
			pend = p + strlen(p);
		/*
		 * The key we must return is a null terminated string, but p
		 * is not necessarily NULL-terminated.  So make a copy, just
		 * for the duration of the insert.
		 */
		len = (size_t)(pend - p);
		if ((copy = malloc(len + 1)) == NULL)
			return (errno);
		strncpy(copy, p, len);
		copy[len] = '\0';
		result_cursor->set_key(result_cursor, copy);
		ret = result_cursor->insert(result_cursor);
		free(copy);
		if (ret != 0)
			return (ret);
	}
	return (0);
}

/*
 * csv_customize --
 *	The customize function creates a customized extractor,
 *	needed to save the field number.
 */
static int
csv_customize(WT_EXTRACTOR *extractor, WT_SESSION *session,
    const char *uri, WT_CONFIG_ITEM *appcfg, WT_EXTRACTOR **customp)
{
	const CSV_EXTRACTOR *orig;
	CSV_EXTRACTOR *csv_extractor;
	long field_num;

	(void)session;				/* Unused parameters */
	(void)uri;				/* Unused parameters */

	orig = (const CSV_EXTRACTOR *)extractor;
	field_num = strtol(appcfg->str, NULL, 10);
	if (field_num < 0 || field_num > INT_MAX)
		return (EINVAL);
	if ((csv_extractor = calloc(1, sizeof(CSV_EXTRACTOR))) == NULL)
		return (errno);

	*csv_extractor = *orig;
	csv_extractor->field_num = (int)field_num;
	*customp = (WT_EXTRACTOR *)csv_extractor;
	return (0);
}

/*
 * csv_terminate --
 *	Terminate is called to free the CSV and any associated memory.
 */
static int
csv_terminate(WT_EXTRACTOR *extractor, WT_SESSION *session)
{
	(void)session;				/* Unused parameters */

	/* Free the allocated memory. */
	free(extractor);
	return (0);
}

/*
 * wiredtiger_extension_init --
 *	WiredTiger CSV extraction extension.
 */
int
wiredtiger_extension_init(WT_CONNECTION *connection, WT_CONFIG_ARG *config)
{
	CSV_EXTRACTOR *csv_extractor;

	(void)config;				/* Unused parameters */

	if ((csv_extractor = calloc(1, sizeof(CSV_EXTRACTOR))) == NULL)
		return (errno);

	csv_extractor->extractor.extract = csv_extract;
	csv_extractor->extractor.customize = csv_customize;
	csv_extractor->extractor.terminate = csv_terminate;
	csv_extractor->wt_api = connection->get_extension_api(connection);

	return (connection->add_extractor(
	    connection, "csv", (WT_EXTRACTOR *)csv_extractor, NULL));
}
