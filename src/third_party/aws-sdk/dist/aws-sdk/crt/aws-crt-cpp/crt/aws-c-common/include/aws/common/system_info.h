#ifndef AWS_COMMON_SYSTEM_INFO_H
#define AWS_COMMON_SYSTEM_INFO_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_platform_os {
    AWS_PLATFORM_OS_WINDOWS,
    AWS_PLATFORM_OS_MAC,
    AWS_PLATFORM_OS_UNIX,
};

struct aws_cpu_info {
    int32_t cpu_id;
    bool suspected_hyper_thread;
};

struct aws_system_environment;

AWS_EXTERN_C_BEGIN

/**
 * Allocates and initializes information about the system the current process is executing on.
 * If successful returns an instance of aws_system_environment. If it fails, it will return NULL.
 *
 * Note: This api is used internally and is still early in its evolution.
 * It may change in incompatible ways in the future.
 */
AWS_COMMON_API
struct aws_system_environment *aws_system_environment_load(struct aws_allocator *allocator);

AWS_COMMON_API
struct aws_system_environment *aws_system_environment_acquire(struct aws_system_environment *env);

AWS_COMMON_API
void aws_system_environment_release(struct aws_system_environment *env);

/**
 * Returns the virtualization vendor for the specified compute environment, e.g. "Xen, Amazon EC2, etc..."
 *
 * The return value may be empty and in that case no vendor was detected.
 */
AWS_COMMON_API
struct aws_byte_cursor aws_system_environment_get_virtualization_vendor(const struct aws_system_environment *env);

/**
 * Returns the product name for the specified compute environment. For example, the Amazon EC2 Instance type.
 *
 * The return value may be empty and in that case no vendor was detected.
 */
AWS_COMMON_API
struct aws_byte_cursor aws_system_environment_get_virtualization_product_name(const struct aws_system_environment *env);

/**
 * Returns the number of processors for the specified compute environment.
 */
AWS_COMMON_API
size_t aws_system_environment_get_processor_count(struct aws_system_environment *env);

/**
 * Returns the number of separate cpu groupings (multi-socket configurations or NUMA).
 */
AWS_COMMON_API
size_t aws_system_environment_get_cpu_group_count(const struct aws_system_environment *env);

/* Returns the OS this was built under */
AWS_COMMON_API
enum aws_platform_os aws_get_platform_build_os(void);

/* Returns the number of online processors available for usage. */
AWS_COMMON_API
size_t aws_system_info_processor_count(void);

/**
 * Returns the logical processor groupings on the system (such as multiple numa nodes).
 */
AWS_COMMON_API
uint16_t aws_get_cpu_group_count(void);

/**
 * For a group, returns the number of CPUs it contains.
 */
AWS_COMMON_API
size_t aws_get_cpu_count_for_group(uint16_t group_idx);

/**
 * Fills in cpu_ids_array with the cpu_id's for the group. To obtain the size to allocate for cpu_ids_array
 * and the value for argument for cpu_ids_array_length, call aws_get_cpu_count_for_group().
 */
AWS_COMMON_API
void aws_get_cpu_ids_for_group(uint16_t group_idx, struct aws_cpu_info *cpu_ids_array, size_t cpu_ids_array_length);

/* Returns true if a debugger is currently attached to the process. */
AWS_COMMON_API
bool aws_is_debugger_present(void);

/* If a debugger is attached to the process, trip a breakpoint. */
AWS_COMMON_API
void aws_debug_break(void);

#if defined(AWS_HAVE_EXECINFO) || defined(_WIN32) || defined(__APPLE__)
#    define AWS_BACKTRACE_STACKS_AVAILABLE
#endif

/*
 * Records a stack trace from the call site.
 * Returns the number of stack entries/stack depth captured, or 0 if the operation
 * is not supported on this platform
 */
AWS_COMMON_API
size_t aws_backtrace(void **stack_frames, size_t num_frames);

/*
 * Converts stack frame pointers to symbols, if symbols are available
 * Returns an array up to stack_depth long, that needs to be free()ed.
 * stack_depth should be the length of frames.
 * Returns NULL if the platform does not support stack frame translation
 * or an error occurs
 */
char **aws_backtrace_symbols(void *const *stack_frames, size_t stack_depth);

/*
 * Converts stack frame pointers to symbols, using all available system
 * tools to try to produce a human readable result. This call will not be
 * quick, as it shells out to addr2line or similar tools.
 * On Windows, this is the same as aws_backtrace_symbols()
 * Returns an array up to stack_depth long that needs to be free()ed. Missing
 * frames will be NULL.
 * Returns NULL if the platform does not support stack frame translation
 * or an error occurs
 */
char **aws_backtrace_addr2line(void *const *stack_frames, size_t stack_depth);

/**
 * Print a backtrace from either the current stack, or (if provided) the current exception/signal
 *  call_site_data is siginfo_t* on POSIX, and LPEXCEPTION_POINTERS on Windows, and can be null
 */
AWS_COMMON_API
void aws_backtrace_print(FILE *fp, void *call_site_data);

/* Log the callstack from the current stack to the currently configured aws_logger */
AWS_COMMON_API
void aws_backtrace_log(int log_level);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_SYSTEM_INFO_H */
