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
 *
 * ex_config_parse.c
 *	This is an example demonstrating how to parse WiredTiger compatible
 *	configuration strings.
 */

#include <stdio.h>
#include <string.h>

#include <wiredtiger.h>

const char *home = NULL;

int main(void)
{
	int ret;
	{
	/*! [WT_CONFIG_PARSER search] */
	WT_CONFIG_ITEM v;
	WT_CONFIG_PARSER *parser;
	int64_t my_page_size;

	const char *config_string = "path=/dev/loop,page_size=1024";

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (ret);
	}

	/*
	 * Retrieve the value of the integer configuration string "page_size".
	 */
	if ((ret = parser->search(parser, "page_size", &v)) != 0) {
		fprintf(stderr,
		    "page_size configuration: %s", wiredtiger_strerror(ret));
		return (ret);
	}
	my_page_size = v.val;
	/*! [WT_CONFIG_PARSER search] */

	(void)my_page_size;
	}

	{
	/*! [WT_CONFIG_PARSER scan] */
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *parser;

	const char *config_string = "path=/dev/loop,page_size=1024";

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (ret);
	}
	/*
	 * Retrieve the values of the configuration strings.
	 */
	while ((ret = parser->next(parser, &k, &v)) == 0) {
		printf("%.*s:", (int)k.len, k.str);
		if (v.type == WT_CONFIG_ITEM_STRING)
			printf("%.*s\n", (int)v.len, v.str);
		else if (v.type == WT_CONFIG_ITEM_NUM)
			printf("%d\n", (int)v.val);
	}
	ret = parser->close(parser);
	/*! [WT_CONFIG_PARSER scan] */
	}

	return (ret);
}
