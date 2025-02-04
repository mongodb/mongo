#ifndef AWS_COMMON_PROCESS_H
#define AWS_COMMON_PROCESS_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_run_command_result {
    /* return code from running the command. */
    int ret_code;

    /**
     * captured stdout message from running the command,
     * caller is responsible for releasing the memory.
     */
    struct aws_string *std_out;

    /**
     * captured stderr message from running the command,
     * caller is responsible for releasing the memory.
     * It's currently not implemented and the value will be set to NULL.
     */
    struct aws_string *std_err;
};

struct aws_run_command_options {
    /**
     * command path and commandline options of running that command.
     */
    const char *command;
};

AWS_EXTERN_C_BEGIN

/**
 * Returns the current process's PID (process id).
 * @return PID as int
 */
AWS_COMMON_API int aws_get_pid(void);

/**
 * Returns the soft limit for max io handles (max fds in unix terminology). This limit is one more than the actual
 * limit. The soft limit can be changed up to the hard limit by any process regardless of permissions.
 */
AWS_COMMON_API size_t aws_get_soft_limit_io_handles(void);

/**
 * Returns the hard limit for max io handles (max fds in unix terminology). This limit is one more than the actual
 * limit. This limit cannot be increased without sudo permissions.
 */
AWS_COMMON_API size_t aws_get_hard_limit_io_handles(void);

/**
 * Sets the new soft limit for io_handles (max fds). This can be up to the hard limit but may not exceed it.
 *
 * This operation will always fail with AWS_ERROR_UNIMPLEMENTED error code on Windows.
 */
AWS_COMMON_API int aws_set_soft_limit_io_handles(size_t max_handles);

AWS_COMMON_API int aws_run_command_result_init(struct aws_allocator *allocator, struct aws_run_command_result *result);

AWS_COMMON_API void aws_run_command_result_cleanup(struct aws_run_command_result *result);

/**
 * Currently this API is implemented using popen on Posix system and
 * _popen on Windows to capture output from running a command. Note
 * that popen only captures stdout, and doesn't provide an option to
 * capture stderr. We will add more options, such as acquire stderr
 * in the future so probably will alter the underlying implementation
 * as well.
 */
AWS_COMMON_API int aws_run_command(
    struct aws_allocator *allocator,
    struct aws_run_command_options *options,
    struct aws_run_command_result *result);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_PROCESS_H */
