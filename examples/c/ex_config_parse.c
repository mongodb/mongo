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
 *
 * ex_config_parse.c
 *	This is an example demonstrating how to parse WiredTiger compatible
 *	configuration strings.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wiredtiger.h>

int
main(void)
{
	int ret;

	/*! [Create a configuration parser] */
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *parser;
	const char *config_string =
	    "path=/dev/loop,page_size=1024,log=(archive=true,file_max=20MB)";

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	if ((ret = parser->close(parser)) != 0) {
		fprintf(stderr, "Error closing configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	/*! [Create a configuration parser] */

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	{
	/*! [get] */
	int64_t my_page_size;
	/*
	 * Retrieve the value of the integer configuration string "page_size".
	 */
	if ((ret = parser->get(parser, "page_size", &v)) != 0) {
		fprintf(stderr,
		    "page_size configuration: %s", wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	my_page_size = v.val;
	/*! [get] */

	ret = parser->close(parser);

	(void)my_page_size;
	}

	{
	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	/*! [next] */
	/*
	 * Retrieve and print the values of the configuration strings.
	 */
	while ((ret = parser->next(parser, &k, &v)) == 0) {
		printf("%.*s:", (int)k.len, k.str);
		if (v.type == WT_CONFIG_ITEM_NUM)
			printf("%" PRId64 "\n", v.val);
		else
			printf("%.*s\n", (int)v.len, v.str);
	}
	/*! [next] */
	ret = parser->close(parser);
	}

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}

	/*! [nested get] */
	/*
	 * Retrieve the value of the nested log file_max configuration string
	 * using dot shorthand. Utilize the configuration parsing automatic
	 * conversion of value strings into an integer.
	 */
	v.type = WT_CONFIG_ITEM_NUM;
	if ((ret = parser->get(parser, "log.file_max", &v)) != 0) {
		fprintf(stderr,
		    "log.file_max configuration: %s", wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	printf("log file max: %" PRId64 "\n", v.val);
	/*! [nested get] */
	ret = parser->close(parser);

	if ((ret = wiredtiger_config_parser_open(
	    NULL, config_string, strlen(config_string), &parser)) != 0) {
		fprintf(stderr, "Error creating configuration parser: %s\n",
		    wiredtiger_strerror(ret));
		return (EXIT_FAILURE);
	}
	/*! [nested traverse] */
	{
	WT_CONFIG_PARSER *sub_parser;
	while ((ret = parser->next(parser, &k, &v)) == 0) {
		if (v.type == WT_CONFIG_ITEM_STRUCT) {
			printf("Found nested configuration: %.*s\n",
			    (int)k.len, k.str);
			if ((ret = wiredtiger_config_parser_open(
			    NULL, v.str, v.len, &sub_parser)) != 0) {
				fprintf(stderr,
				    "Error creating nested configuration "
				    "parser: %s\n",
				    wiredtiger_strerror(ret));
				break;
			}
			while ((ret =
			    sub_parser->next(sub_parser, &k, &v)) == 0)
				printf("\t%.*s\n", (int)k.len, k.str);
			ret = sub_parser->close(sub_parser);
		}
	}
	/*! [nested traverse] */
	ret = parser->close(parser);
	}

	return (ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
