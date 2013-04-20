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
 *
 * ex_data_source.c
 * 	demonstrates how to create and access a data source
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wiredtiger.h>

/*! [WT_EXTENSION_API declaration] */
#include <wiredtiger_ext.h>

static WT_EXTENSION_API *wt_api;

static void
my_data_source_init(WT_CONNECTION *conn)
{
	wt_api = conn->get_extension_api(conn);
}
/*! [WT_EXTENSION_API declaration] */

/*! [WT_DATA_SOURCE create] */
static int
my_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE create] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)uri;
	(void)config;

	{
	const char *msg = "string";
	/*! [WT_EXTENSION_API err_printf] */
	(void)wt_api->err_printf(
	    wt_api, session, "extension error message: %s", msg);
	/*! [WT_EXTENSION_API err_printf] */
	}

	{
	const char *msg = "string";
	/*! [WT_EXTENSION_API msg_printf] */
	(void)wt_api->msg_printf(wt_api, session, "extension message: %s", msg);
	/*! [WT_EXTENSION_API msg_printf] */
	}

	{
	int ret = 0;
	/*! [WT_EXTENSION_API strerror] */
	(void)wt_api->err_printf(wt_api,
	    session, "WiredTiger error return: %s", wt_api->strerror(ret));
	/*! [WT_EXTENSION_API strerror] */
	}

	{
	/*! [WT_EXTENSION_API scr_alloc] */
	void *buffer;
	if ((buffer = wt_api->scr_alloc(wt_api, session, 512)) == NULL) {
		(void)wt_api->err_printf(wt_api, session,
		    "buffer allocation: %s", wiredtiger_strerror(ENOMEM));
		return (ENOMEM);
	}
	/*! [WT_EXTENSION_API scr_alloc] */

	/*! [WT_EXTENSION_API scr_free] */
	wt_api->scr_free(wt_api, session, buffer);
	/*! [WT_EXTENSION_API scr_free] */
	}

	return (0);
}

/*! [WT_DATA_SOURCE compact] */
static int
my_compact(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE compact] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;

	return (0);
}

/*! [WT_DATA_SOURCE drop] */
static int
my_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE drop] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;

	return (0);
}

static int
data_source_cursor(void)
{
	return (0);
}

static const char *
data_source_error(int v)
{
	return (v == 0 ? "one" : "two");
}

/*! [WT_DATA_SOURCE open_cursor] */
static int
my_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config, WT_CURSOR **new_cursor)
/*! [WT_DATA_SOURCE open_cursor] */
{
	int ret;

	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;
	(void)new_cursor;

	{
	/*! [WT_EXTENSION_CONFIG boolean] */
	WT_CONFIG_ITEM v;
	int my_data_source_overwrite;

	/*
	 * Retrieve the value of the boolean type configuration string
	 * "overwrite".
	 */
	if ((ret = wt_api->config_get(
	    wt_api, session, config, "overwrite", &v)) != 0) {
		(void)wt_api->err_printf(wt_api, session,
		    "overwrite configuration: %s", wiredtiger_strerror(ret));
		return (ret);
	}
	my_data_source_overwrite = v.val != 0;
	/*! [WT_EXTENSION_CONFIG boolean] */

	(void)my_data_source_overwrite;
	}

	{
	/*! [WT_EXTENSION_CONFIG integer] */
	WT_CONFIG_ITEM v;
	int64_t my_data_source_page_size;

	/*
	 * Retrieve the value of the integer type configuration string
	 * "page_size".
	 */
	if ((ret = wt_api->config_get(
	    wt_api, session, config, "page_size", &v)) != 0) {
		(void)wt_api->err_printf(wt_api, session,
		    "page_size configuration: %s", wiredtiger_strerror(ret));
		return (ret);
	}
	my_data_source_page_size = v.val;
	/*! [WT_EXTENSION_CONFIG integer] */

	(void)my_data_source_page_size;
	}

	{
	/*! [WT_EXTENSION config_get] */
	WT_CONFIG_ITEM v;
	const char *my_data_source_key;

	/*
	 * Retrieve the value of the string type configuration string
	 * "key_format".
	 */
	if ((ret = wt_api->config_get(
	    wt_api, session, config, "key_format", &v)) != 0) {
		(void)wt_api->err_printf(wt_api, session,
		    "key_format configuration: %s", wiredtiger_strerror(ret));
		return (ret);
	}

	/*
	 * Values returned from WT_EXTENSION_API::config in the str field are
	 * not nul-terminated; the associated length must be used instead.
	 */
	if (v.len == 1 && v.str[0] == 'r')
		my_data_source_key = "recno";
	else
		my_data_source_key = "bytestring";
	/*! [WT_EXTENSION config_get] */

	(void)my_data_source_key;
	}

	{
	/*! [WT_EXTENSION config scan] */
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_SCAN *scan;

	/*
	 * Retrieve the value of the list type configuration string "paths".
	 */
	if ((ret = wt_api->config_get(
	    wt_api, session, config, "paths", &v)) != 0) {
		(void)wt_api->err_printf(wt_api, session,
		    "paths configuration: %s", wiredtiger_strerror(ret));
		return (ret);
	}

	/*
	 * Step through the list of entries.
	 */
	ret = wt_api->config_scan_begin(wt_api, session, v.str, v.len, &scan);
	while ((ret = wt_api->config_scan_next(wt_api, scan, &k, &v)) == 0)
		printf("%.*s\n", (int)k.len, k.str);
	ret = wt_api->config_scan_end(wt_api, scan);
	/*! [WT_EXTENSION config scan] */
	}

	/*! [WT_DATA_SOURCE error message] */
	/*
	 * If an underlying function fails, log the error and then return an
	 * error within WiredTiger's name space.
	 */
	if ((ret = data_source_cursor()) != 0) {
		(void)wt_api->err_printf(wt_api,
		    session, "my_open_cursor: %s", data_source_error(ret));
		return (WT_ERROR);
	}
	/*! [WT_DATA_SOURCE error message] */

	return (0);
}

/*! [WT_DATA_SOURCE rename] */
static int
my_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, const char *newname, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE rename] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)newname;
	(void)config;

	return (0);
}

/*! [WT_DATA_SOURCE salvage] */
static int
my_salvage(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE salvage] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;

	return (0);
}

/*! [WT_DATA_SOURCE truncate] */
static int
my_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE truncate] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;

	return (0);
}

/*! [WT_DATA_SOURCE verify] */
static int
my_verify(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *uri, WT_CONFIG_ARG *config)
/*! [WT_DATA_SOURCE verify] */
{
	/* Unused parameters */
	(void)dsrc;
	(void)session;
	(void)uri;
	(void)config;

	return (0);
}

int
main(void)
{
	WT_CONNECTION *conn;
	int ret;

	ret = wiredtiger_open(NULL, NULL, "create", &conn);

	my_data_source_init(conn);

	{
	/*! [WT_DATA_SOURCE register] */
	static WT_DATA_SOURCE my_dsrc = {
		my_create,
		my_compact,
		my_drop,
		my_open_cursor,
		my_rename,
		my_salvage,
		my_truncate,
		my_verify
	};
	ret = conn->add_data_source(conn, "dsrc:", &my_dsrc, NULL);
	/*! [WT_DATA_SOURCE register] */
	}

	/*! [WT_DATA_SOURCE configure boolean] */
	/* my_boolean defaults to true. */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "my_boolean=true", "boolean", NULL);
	/*! [WT_DATA_SOURCE configure boolean] */

	/*! [WT_DATA_SOURCE configure integer] */
	/* my_integer defaults to 5. */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "my_integer=5", "int", NULL);
	/*! [WT_DATA_SOURCE configure integer] */

	/*! [WT_DATA_SOURCE configure string] */
	/* my_string defaults to "name". */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "my_string=name", "string", NULL);
	/*! [WT_DATA_SOURCE configure string] */

	/*! [WT_DATA_SOURCE configure list] */
	/* my_list defaults to "first" and "second". */
	ret = conn->configure_method(conn,
	    "session.open_cursor",
	    NULL, "my_list=[first, second]", "list", NULL);
	/*! [WT_DATA_SOURCE configure list] */

	/*! [WT_DATA_SOURCE configure integer with checking] */
	/*
	 * Limit the number of devices to between 1 and 30; the default is 5.
	 */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "devices=5", "int", "min=1, max=30");
	/*! [WT_DATA_SOURCE configure integer with checking] */

	/*! [WT_DATA_SOURCE configure string with checking] */
	/*
	 * Limit the target string to one of /device, /home or /target; default
	 * to /home.
	 */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "target=\"/home\"", "string",
	    "choices=[\"/device\", \"/home\", \"/target\"]");
	/*! [WT_DATA_SOURCE configure string with checking] */

	/*! [WT_DATA_SOURCE configure list with checking] */
	/*
	 * Limit the paths list to one or more of /device, /home, /mnt or
	 * /target; default to /mnt.
	 */
	ret = conn->configure_method(conn,
	    "session.open_cursor", NULL, "paths=[\"/mnt\"]", "list",
	    "choices=[\"/device\", \"/home\", \"/mnt\", \"/target\"]");
	/*! [WT_DATA_SOURCE configure list with checking] */

	/*! [WT_EXTENSION_API default_session] */
	(void)wt_api->msg_printf(wt_api, NULL, "configuration complete");
	/*! [WT_EXTENSION_API default_session] */

	(void)conn->close(conn, NULL);

	return (ret);
}
