#ifndef AWS_COMMON_COMMAND_LINE_PARSER_H
#define AWS_COMMON_COMMAND_LINE_PARSER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_cli_options_has_arg {
    AWS_CLI_OPTIONS_NO_ARGUMENT = 0,
    AWS_CLI_OPTIONS_REQUIRED_ARGUMENT = 1,
    AWS_CLI_OPTIONS_OPTIONAL_ARGUMENT = 2,
};

/**
 * Invoked when a subcommand is encountered. argc and argv[] begins at the command encountered.
 * command_name is the name of the command being handled.
 */
typedef int(aws_cli_options_subcommand_fn)(int argc, char *const argv[], const char *command_name, void *user_data);

/**
 * Dispatch table to dispatch cli commands from.
 * command_name should be the exact string for the command you want to handle from the command line.
 */
struct aws_cli_subcommand_dispatch {
    aws_cli_options_subcommand_fn *subcommand_fn;
    const char *command_name;
};

/* Ignoring padding since we're trying to maintain getopt.h compatibility */
/* NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) */
struct aws_cli_option {
    const char *name;
    enum aws_cli_options_has_arg has_arg;
    int *flag;
    int val;
};

AWS_EXTERN_C_BEGIN
/**
 * Initialized to 1 (for where the first argument would be). As arguments are parsed, this number is the index
 * of the next argument to parse. Reset this to 1 to parse another set of arguments, or to rerun the parser.
 */
AWS_COMMON_API extern int aws_cli_optind;

/**
 * If an option has an argument, when the option is encountered, this will be set to the argument portion.
 */
AWS_COMMON_API extern const char *aws_cli_optarg;

/**
 * If 0x02 was returned by aws_cli_getopt_long(), this value will be set to the argument encountered.
 */
AWS_COMMON_API extern const char *aws_cli_positional_arg;

/**
 * A mostly compliant implementation of posix getopt_long(). Parses command-line arguments. argc is the number of
 * command line arguments passed in argv. optstring contains the legitimate option characters. The option characters
 * correspond to aws_cli_option::val. If the character is followed by a :, the option requires an argument. If it is
 * followed by '::', the argument is optional (not implemented yet).
 *
 *  longopts, is an array of struct aws_cli_option. These are the allowed options for the program.
 *  The last member of the array must be zero initialized.
 *
 *  If longindex is non-null, it will be set to the index in longopts, for the found option.
 *
 *  Returns option val if it was found, '?' if an option was encountered that was not specified in the option string,
 * 0x02 (START_OF_TEXT) will be returned if a positional argument was encountered. returns -1 when all arguments that
 * can be parsed have been parsed.
 */
AWS_COMMON_API int aws_cli_getopt_long(
    int argc,
    char *const argv[],
    const char *optstring,
    const struct aws_cli_option *longopts,
    int *longindex);

/**
 * Resets global parser state for use in another parser run for the application.
 */
AWS_COMMON_API void aws_cli_reset_state(void);

/**
 * Dispatches the current command line arguments with a subcommand from the second input argument in argv[], if
 * dispatch table contains a command that matches the argument. When the command is dispatched, argc and argv will be
 * updated to reflect the new argument count. The cli options are required to come after the subcommand. If either, no
 * dispatch was found or there was no argument passed to the program, this function will return AWS_OP_ERR. Check
 * aws_last_error() for details on the error.
 * @param argc number of arguments passed to int main()
 * @param argv the arguments passed to int main()
 * @param parse_cb, optional, specify NULL if you don't want to handle this. This argument is for parsing "meta"
 * commands from the command line options prior to dispatch occurring.
 * @param dispatch_table table containing functions and command name to dispatch on.
 * @param table_length number of entries in dispatch_table.
 * @return AWS_OP_SUCCESS(0) on success, AWS_OP_ERR(-1) on failure
 */
AWS_COMMON_API int aws_cli_dispatch_on_subcommand(
    int argc,
    char *const argv[],
    struct aws_cli_subcommand_dispatch *dispatch_table,
    int table_length,
    void *user_data);
AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_COMMAND_LINE_PARSER_H */
