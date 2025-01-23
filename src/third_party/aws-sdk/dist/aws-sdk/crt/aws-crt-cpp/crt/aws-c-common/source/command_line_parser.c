/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/byte_buf.h>
#include <aws/common/command_line_parser.h>

#include <ctype.h>

int aws_cli_optind = 1;
int aws_cli_opterr = -1;
int aws_cli_optopt = 0;
bool aws_cli_on_arg = false;

const char *aws_cli_optarg = NULL;
const char *aws_cli_positional_arg = NULL;

static const struct aws_cli_option *s_find_option_from_char(
    const struct aws_cli_option *longopts,
    char search_for,
    int *longindex) {
    int index = 0;
    const struct aws_cli_option *option = &longopts[index];

    while (option->val != 0 || option->name) {
        if (option->val == search_for) {
            if (longindex) {
                *longindex = index;
            }
            return option;
        }

        option = &longopts[++index];
    }

    return NULL;
}

AWS_COMMON_API void aws_cli_reset_state(void) {
    aws_cli_optind = 1;
    aws_cli_opterr = -1;
    aws_cli_optopt = 0;
    aws_cli_on_arg = false;

    aws_cli_optarg = NULL;
    aws_cli_positional_arg = NULL;
}

static const struct aws_cli_option *s_find_option_from_c_str(
    const struct aws_cli_option *longopts,
    const char *search_for,
    int *longindex) {
    int index = 0;
    const struct aws_cli_option *option = &longopts[index];

    while (option->name || option->val != 0) {
        if (option->name && !strcmp(search_for, option->name)) {
            if (longindex) {
                *longindex = index;
            }
            return option;
        }

        option = &longopts[++index];
    }

    return NULL;
}

int aws_cli_getopt_long(
    int argc,
    char *const argv[],
    const char *optstring,
    const struct aws_cli_option *longopts,
    int *longindex) {
    aws_cli_optarg = NULL;

    if (aws_cli_optind >= argc) {
        return -1;
    }

    char first_char = argv[aws_cli_optind][0];
    char second_char = argv[aws_cli_optind][1];
    char *option_start = NULL;
    const struct aws_cli_option *option = NULL;
    bool positional_arg_encountered = false;

    if (first_char == '-' && second_char != '-') {
        aws_cli_on_arg = true;
        positional_arg_encountered = false;
        option_start = &argv[aws_cli_optind][1];
        option = s_find_option_from_char(longopts, *option_start, longindex);
    } else if (first_char == '-' && second_char == '-') {
        aws_cli_on_arg = true;
        positional_arg_encountered = false;
        option_start = &argv[aws_cli_optind][2];
        option = s_find_option_from_c_str(longopts, option_start, longindex);
    } else {
        if (!aws_cli_on_arg) {
            aws_cli_positional_arg = argv[aws_cli_optind];
            positional_arg_encountered = true;
        } else {
            aws_cli_on_arg = false;
            aws_cli_positional_arg = NULL;
        }
    }

    aws_cli_optind++;
    if (option) {
        bool has_arg = false;
        aws_cli_on_arg = false;
        aws_cli_positional_arg = NULL;

        char *opt_value = memchr(optstring, option->val, strlen(optstring) + 1);
        if (!opt_value) {
            return '?';
        }

        if (opt_value[1] == ':') {
            has_arg = true;
        }

        if (has_arg) {
            if (aws_cli_optind >= argc) {
                return '?';
            }

            aws_cli_optarg = argv[aws_cli_optind++];
        }

        return option->val;
    }

    /* start of text to indicate we just have a text argument. */
    return positional_arg_encountered ? 0x02 : '?';
}

int aws_cli_dispatch_on_subcommand(
    int argc,
    char *const argv[],
    struct aws_cli_subcommand_dispatch *dispatch_table,
    int table_length,
    void *user_data) {
    if (argc >= 2) {
        struct aws_byte_cursor arg_name = aws_byte_cursor_from_c_str(argv[1]);
        for (int i = 0; i < table_length; ++i) {
            struct aws_byte_cursor cmd_name = aws_byte_cursor_from_c_str(dispatch_table[i].command_name);

            if (aws_byte_cursor_eq_ignore_case(&arg_name, &cmd_name)) {
                return dispatch_table[i].subcommand_fn(argc - 1, &argv[1], (const char *)arg_name.ptr, user_data);
            }
        }

        return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
    }

    return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
}
