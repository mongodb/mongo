/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/logging.h>
#include <aws/common/math.h>
#include <aws/common/private/dlloads.h>
#include <aws/common/private/external_module_impl.h>
#include <aws/common/private/thread_shared.h>

#include <stdarg.h>
#include <stdlib.h>

#ifdef _WIN32
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

#ifdef __MACH__
#    include <CoreFoundation/CoreFoundation.h>
#endif

/* turn off unused named parameter warning on msvc.*/
#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4100)
#endif

long (*g_set_mempolicy_ptr)(int, const unsigned long *, unsigned long) = NULL;
int (*g_numa_available_ptr)(void) = NULL;
int (*g_numa_num_configured_nodes_ptr)(void) = NULL;
int (*g_numa_num_possible_cpus_ptr)(void) = NULL;
int (*g_numa_node_of_cpu_ptr)(int cpu) = NULL;

void *g_libnuma_handle = NULL;

void aws_secure_zero(void *pBuf, size_t bufsize) {
    /* don't pass NULL to memset(), it's undefined behavior */
    if (pBuf == NULL || bufsize == 0) {
        AWS_ASSERT(bufsize == 0); /* if you believe your NULL buffer has a size, then you have issues */
        return;
    }

#if defined(_WIN32)
    SecureZeroMemory(pBuf, bufsize);
#else
    /* We cannot use memset_s, even on a C11 compiler, because that would require
     * that __STDC_WANT_LIB_EXT1__ be defined before the _first_ inclusion of string.h.
     *
     * We'll try to work around this by using inline asm on GCC-like compilers,
     * and by exposing the buffer pointer in a volatile local pointer elsewhere.
     */
#    if defined(__GNUC__) || defined(__clang__)
    memset(pBuf, 0, bufsize);
    /* This inline asm serves to convince the compiler that the buffer is (somehow) still
     * used after the zero, and therefore that the optimizer can't eliminate the memset.
     */
    __asm__ __volatile__("" /* The asm doesn't actually do anything. */
                         :  /* no outputs */
                         /* Tell the compiler that the asm code has access to the pointer to the buffer,
                          * and therefore it might be reading the (now-zeroed) buffer.
                          * Without this. clang/LLVM 9.0.0 optimizes away a memset of a stack buffer.
                          */
                         : "r"(pBuf)
                         /* Also clobber memory. While this seems like it might be unnecessary - after all,
                          * it's enough that the asm might read the buffer, right? - in practice GCC 7.3.0
                          * seems to optimize a zero of a stack buffer without it.
                          */
                         : "memory");
#    else  // not GCC/clang
    /* We don't have access to inline asm, since we're on a non-GCC platform. Move the pointer
     * through a volatile pointer in an attempt to confuse the optimizer.
     */
    volatile void *pVolBuf = pBuf;
    memset(pVolBuf, 0, bufsize);
#    endif // #else not GCC/clang
#endif     // #else not windows
}

#define AWS_DEFINE_ERROR_INFO_COMMON(C, ES) [(C) - 0x0000] = AWS_DEFINE_ERROR_INFO(C, ES, "aws-c-common")
/* clang-format off */
static struct aws_error_info errors[] = {
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_SUCCESS,
        "Success."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_OOM,
        "Out of memory."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_NO_SPACE,
        "Out of space on disk."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_UNKNOWN,
        "Unknown error."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_SHORT_BUFFER,
        "Buffer is not large enough to hold result."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_OVERFLOW_DETECTED,
        "Fixed size value overflow was detected."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_UNSUPPORTED_OPERATION,
        "Unsupported operation."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_BUFFER_SIZE,
        "Invalid buffer size."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_HEX_STR,
        "Invalid hex string."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_BASE64_STR,
        "Invalid base64 string."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_INDEX,
        "Invalid index for list access."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_INVALID_SETTINGS,
        "Invalid thread settings."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_INSUFFICIENT_RESOURCE,
        "Insufficent resources for thread."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_NO_PERMISSIONS,
        "Insufficient permissions for thread operation."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_NOT_JOINABLE,
        "Thread not joinable."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_NO_SUCH_THREAD_ID,
        "No such thread ID."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_THREAD_DEADLOCK_DETECTED,
        "Deadlock detected in thread."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MUTEX_NOT_INIT,
        "Mutex not initialized."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MUTEX_TIMEOUT,
        "Mutex operation timed out."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MUTEX_CALLER_NOT_OWNER,
        "The caller of a mutex operation was not the owner."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MUTEX_FAILED,
        "Mutex operation failed."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_COND_VARIABLE_INIT_FAILED,
        "Condition variable initialization failed."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_COND_VARIABLE_TIMED_OUT,
        "Condition variable wait timed out."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_COND_VARIABLE_ERROR_UNKNOWN,
        "Condition variable unknown error."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_CLOCK_FAILURE,
        "Clock operation failed."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_LIST_EMPTY,
        "Empty list."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_DEST_COPY_TOO_SMALL,
        "Destination of copy is too small."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_LIST_EXCEEDS_MAX_SIZE,
        "A requested operation on a list would exceed it's max size."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_LIST_STATIC_MODE_CANT_SHRINK,
        "Attempt to shrink a list in static mode."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_PRIORITY_QUEUE_FULL,
        "Attempt to add items to a full preallocated queue in static mode."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_PRIORITY_QUEUE_EMPTY,
        "Attempt to pop an item from an empty queue."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_PRIORITY_QUEUE_BAD_NODE,
        "Bad node handle passed to remove."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_HASHTBL_ITEM_NOT_FOUND,
        "Item not found in hash table."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_DATE_STR,
        "Date string is invalid and cannot be parsed."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_ARGUMENT,
        "An invalid argument was passed to a function."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_RANDOM_GEN_FAILED,
        "A call to the random number generator failed. Retry later."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MALFORMED_INPUT_STRING,
        "An input string was passed to a parser and the string was incorrectly formatted."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_UNIMPLEMENTED,
        "A function was called, but is not implemented."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_STATE,
        "An invalid state was encountered."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_ENVIRONMENT_GET,
        "System call failure when getting an environment variable."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_ENVIRONMENT_SET,
        "System call failure when setting an environment variable."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_ENVIRONMENT_UNSET,
        "System call failure when unsetting an environment variable."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_SYS_CALL_FAILURE,
        "System call failure."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_FILE_INVALID_PATH,
        "Invalid file path."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_MAX_FDS_EXCEEDED,
        "The maximum number of fds has been exceeded."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_NO_PERMISSION,
        "User does not have permission to perform the requested action."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_STREAM_UNSEEKABLE,
        "Stream does not support seek operations."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_C_STRING_BUFFER_NOT_NULL_TERMINATED,
        "A c-string like buffer was passed but a null terminator was not found within the bounds of the buffer."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_STRING_MATCH_NOT_FOUND,
      "The specified substring was not present in the input string."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_DIVIDE_BY_ZERO,
        "Attempt to divide a number by zero."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_FILE_HANDLE,
        "Invalid file handle."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_OPERATION_INTERUPTED,
        "The operation was interrupted."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_DIRECTORY_NOT_EMPTY,
        "An operation on a directory was attempted which is not allowed when the directory is not empty."
    ),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_PLATFORM_NOT_SUPPORTED,
        "Feature not supported on this platform."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_UTF8,
        "Invalid UTF-8."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_GET_HOME_DIRECTORY_FAILED,
        "Failed to get home directory."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_XML,
        "Invalid XML document."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_FILE_OPEN_FAILURE,
        "Failed opening file."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_FILE_READ_FAILURE,
        "Failed reading from file."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_FILE_WRITE_FAILURE,
        "Failed writing to file."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_INVALID_CBOR,
        "Malformed cbor data."),
    AWS_DEFINE_ERROR_INFO_COMMON(
        AWS_ERROR_CBOR_UNEXPECTED_TYPE,
        "Unexpected cbor type encountered."),
};
/* clang-format on */

static struct aws_error_info_list s_list = {
    .error_list = errors,
    .count = AWS_ARRAY_SIZE(errors),
};

static struct aws_log_subject_info s_common_log_subject_infos[] = {
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_COMMON_GENERAL,
        "aws-c-common",
        "Subject for aws-c-common logging that doesn't belong to any particular category"),
    DEFINE_LOG_SUBJECT_INFO(
        AWS_LS_COMMON_TASK_SCHEDULER,
        "task-scheduler",
        "Subject for task scheduler or task specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_THREAD, "thread", "Subject for logging thread related functions."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_MEMTRACE, "memtrace", "Output from the aws_mem_trace_dump function"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_XML_PARSER, "xml-parser", "Subject for xml parser specific logging."),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_IO, "common-io", "Common IO utilities"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_BUS, "bus", "Message bus"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_TEST, "test", "Unit/integration testing"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_JSON_PARSER, "json-parser", "Subject for json parser specific logging"),
    DEFINE_LOG_SUBJECT_INFO(AWS_LS_COMMON_CBOR, "cbor", "Subject for CBOR encode and decode"),
};

static struct aws_log_subject_info_list s_common_log_subject_list = {
    .subject_list = s_common_log_subject_infos,
    .count = AWS_ARRAY_SIZE(s_common_log_subject_infos),
};

static bool s_common_library_initialized = false;

void aws_common_library_init(struct aws_allocator *allocator) {
    (void)allocator;

    if (!s_common_library_initialized) {
        s_common_library_initialized = true;
        aws_register_error_info(&s_list);
        aws_register_log_subject_info_list(&s_common_log_subject_list);
        aws_thread_initialize_thread_management();
        aws_json_module_init(allocator);
        aws_cbor_module_init(allocator);

/* NUMA is funky and we can't rely on libnuma.so being available. We also don't want to take a hard dependency on it,
 * try and load it if we can. */
#ifdef AWS_OS_LINUX
        /* libnuma defines set_mempolicy() as a WEAK symbol. Loading into the global symbol table overwrites symbols and
           assumptions due to the way loaders and dlload are often implemented and those symbols are defined by things
           like libpthread.so on some unix distros. Sorry about the memory usage here, but it's our only safe choice.
           Also, please don't do numa configurations if memory is your economic bottleneck. */
        g_libnuma_handle = dlopen("libnuma.so", RTLD_LAZY | RTLD_LOCAL);

        /* turns out so versioning is really inconsistent these days */
        if (!g_libnuma_handle) {
            g_libnuma_handle = dlopen("libnuma.so.1", RTLD_LAZY | RTLD_LOCAL);
        }

        if (!g_libnuma_handle) {
            g_libnuma_handle = dlopen("libnuma.so.2", RTLD_LAZY | RTLD_LOCAL);
        }

        if (g_libnuma_handle) {
            AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: libnuma.so loaded");
            *(void **)(&g_set_mempolicy_ptr) = dlsym(g_libnuma_handle, "set_mempolicy");
            if (g_set_mempolicy_ptr) {
                AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: set_mempolicy() loaded");
            } else {
                AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: set_mempolicy() failed to load");
            }

            *(void **)(&g_numa_available_ptr) = dlsym(g_libnuma_handle, "numa_available");
            if (g_numa_available_ptr) {
                AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_available() loaded");
            } else {
                AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_available() failed to load");
            }
            if (g_numa_available_ptr() == -1) {
                AWS_LOGF_INFO(
                    AWS_LS_COMMON_GENERAL,
                    "static: numa_available() returns -1, numa functions are not available. Skip loading the other "
                    "numa functions.");
            } else {
                *(void **)(&g_numa_num_configured_nodes_ptr) = dlsym(g_libnuma_handle, "numa_num_configured_nodes");
                if (g_numa_num_configured_nodes_ptr) {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_num_configured_nodes() loaded");
                } else {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_num_configured_nodes() failed to load");
                }

                *(void **)(&g_numa_num_possible_cpus_ptr) = dlsym(g_libnuma_handle, "numa_num_possible_cpus");
                if (g_numa_num_possible_cpus_ptr) {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_num_possible_cpus() loaded");
                } else {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_num_possible_cpus() failed to load");
                }

                *(void **)(&g_numa_node_of_cpu_ptr) = dlsym(g_libnuma_handle, "numa_node_of_cpu");
                if (g_numa_node_of_cpu_ptr) {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_node_of_cpu() loaded");
                } else {
                    AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: numa_node_of_cpu() failed to load");
                }
            }

        } else {
            AWS_LOGF_INFO(AWS_LS_COMMON_GENERAL, "static: libnuma.so failed to load");
        }
#endif
    }
}

void aws_common_library_clean_up(void) {
    if (s_common_library_initialized) {
        s_common_library_initialized = false;
        aws_thread_join_all_managed();
        aws_unregister_error_info(&s_list);
        aws_unregister_log_subject_info_list(&s_common_log_subject_list);
        aws_json_module_cleanup();
        aws_cbor_module_cleanup();
#ifdef AWS_OS_LINUX
        if (g_libnuma_handle) {
            dlclose(g_libnuma_handle);
        }
#endif
    }
}

void aws_common_fatal_assert_library_initialized(void) {
    if (!s_common_library_initialized) {
        fprintf(
            stderr, "%s", "aws_common_library_init() must be called before using any functionality in aws-c-common.");

        AWS_FATAL_ASSERT(s_common_library_initialized);
    }
}

#ifdef _MSC_VER
#    pragma warning(pop)
#endif
