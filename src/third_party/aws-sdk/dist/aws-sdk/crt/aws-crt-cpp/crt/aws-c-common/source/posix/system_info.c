/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/system_info.h>

#include <aws/common/byte_buf.h>
#include <aws/common/logging.h>
#include <aws/common/platform.h>
#include <aws/common/private/dlloads.h>

#if defined(__FreeBSD__) || defined(__NetBSD__)
#    define __BSD_VISIBLE 1
#endif

#if defined(__linux__)
#    include <sys/sysinfo.h>
#endif

#if defined(__linux__) || defined(__unix__)
#    include <sys/types.h>
#endif

#include <unistd.h>

#if defined(HAVE_SYSCONF)
size_t aws_system_info_processor_count(void) {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (AWS_LIKELY(nprocs >= 0)) {
        return (size_t)nprocs;
    }

    AWS_FATAL_POSTCONDITION(nprocs >= 0);
    return 0;
}
#else
size_t aws_system_info_processor_count(void) {
#    if defined(AWS_NUM_CPU_CORES)
    AWS_FATAL_PRECONDITION(AWS_NUM_CPU_CORES > 0);
    return AWS_NUM_CPU_CORES;
#    else
    return 1;
#    endif
}
#endif

#include <ctype.h>
#include <fcntl.h>

uint16_t aws_get_cpu_group_count(void) {
    if (g_numa_num_configured_nodes_ptr) {
        return aws_max_u16(1, (uint16_t)g_numa_num_configured_nodes_ptr());
    }

    return 1U;
}

size_t aws_get_cpu_count_for_group(uint16_t group_idx) {
    if (g_numa_node_of_cpu_ptr) {
        size_t total_cpus = aws_system_info_processor_count();

        uint16_t cpu_count = 0;
        for (size_t i = 0; i < total_cpus; ++i) {
            if (group_idx == g_numa_node_of_cpu_ptr((int)i)) {
                cpu_count++;
            }
        }
        return cpu_count;
    }

    return aws_system_info_processor_count();
}

void aws_get_cpu_ids_for_group(uint16_t group_idx, struct aws_cpu_info *cpu_ids_array, size_t cpu_ids_array_length) {
    AWS_PRECONDITION(cpu_ids_array);

    if (!cpu_ids_array_length) {
        return;
    }

    /* go ahead and initialize everything. */
    for (size_t i = 0; i < cpu_ids_array_length; ++i) {
        cpu_ids_array[i].cpu_id = -1;
        cpu_ids_array[i].suspected_hyper_thread = false;
    }

    if (g_numa_node_of_cpu_ptr) {
        size_t total_cpus = aws_system_info_processor_count();
        size_t current_array_idx = 0;
        for (size_t i = 0; i < total_cpus && current_array_idx < cpu_ids_array_length; ++i) {
            if ((int)group_idx == g_numa_node_of_cpu_ptr((int)i)) {
                cpu_ids_array[current_array_idx].cpu_id = (int32_t)i;

                /* looking for an index jump is a more reliable way to find these. If they're in the group and then
                 * the index jumps, say from 17 to 36, we're most-likely in hyper-thread land. Also, inside a node,
                 * once we find the first hyper-thread, the remaining cores are also likely hyper threads. */
                if (current_array_idx > 0 && (cpu_ids_array[current_array_idx - 1].suspected_hyper_thread ||
                                              cpu_ids_array[current_array_idx - 1].cpu_id < ((int)i - 1))) {
                    cpu_ids_array[current_array_idx].suspected_hyper_thread = true;
                }
                current_array_idx += 1;
            }
        }

        return;
    }

    /* a crude hint, but hyper-threads are numbered as the second half of the cpu id listing. The assumption if you
     * hit here is that this is just listing all cpus on the system. */
    size_t hyper_thread_hint = cpu_ids_array_length / 2 - 1;

    for (size_t i = 0; i < cpu_ids_array_length; ++i) {
        cpu_ids_array[i].cpu_id = (int32_t)i;
        cpu_ids_array[i].suspected_hyper_thread = i > hyper_thread_hint;
    }
}

bool aws_is_debugger_present(void) {
    /* Open the status file */
    const int status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1) {
        return false;
    }

    /* Read its contents */
    char buf[4096];
    const ssize_t num_read = read(status_fd, buf, sizeof(buf) - 1);
    close(status_fd);
    if (num_read <= 0) {
        return false;
    }
    buf[num_read] = '\0';

    /* Search for the TracerPid field, which will indicate the debugger process */
    const char tracerPidString[] = "TracerPid:";
    const char *tracer_pid = strstr(buf, tracerPidString);
    if (!tracer_pid) {
        return false;
    }

    /* If it's not 0, then there's a debugger */
    for (const char *cur = tracer_pid + sizeof(tracerPidString) - 1; cur <= buf + num_read; ++cur) {
        if (!aws_isspace(*cur)) {
            return aws_isdigit(*cur) && *cur != '0';
        }
    }

    return false;
}

#include <signal.h>

#ifndef __has_builtin
#    define __has_builtin(x) 0
#endif

void aws_debug_break(void) {
#ifdef DEBUG_BUILD
    if (aws_is_debugger_present()) {
#    if __has_builtin(__builtin_debugtrap)
        __builtin_debugtrap();
#    else
        raise(SIGTRAP);
#    endif
    }
#endif /* DEBUG_BUILD */
}

#if defined(AWS_HAVE_EXECINFO)
#    include <execinfo.h>
#    include <limits.h>

#    define AWS_BACKTRACE_DEPTH 128

struct aws_stack_frame_info {
    char exe[PATH_MAX];
    char addr[32];
    char base[32]; /* base addr for dylib/exe */
    char function[128];
};

/* Ensure only safe characters in a path buffer in case someone tries to
   rename the exe and trigger shell execution via the sub commands used to
   resolve symbols */
char *s_whitelist_chars(char *path) {
    char *cur = path;
    while (*cur) {
        bool whitelisted = aws_isalnum(*cur) || aws_isspace(*cur) || *cur == '/' || *cur == '_' || *cur == '.' ||
                           (cur > path && *cur == '-');
        if (!whitelisted) {
            *cur = '_';
        }
        ++cur;
    }
    return path;
}

#    if defined(__APPLE__)
#        include <ctype.h>
#        include <dlfcn.h>
#        include <mach-o/dyld.h>
static char s_exe_path[PATH_MAX];
static const char *s_get_executable_path(void) {
    static const char *s_exe = NULL;
    if (AWS_LIKELY(s_exe)) {
        return s_exe;
    }
    uint32_t len = sizeof(s_exe_path);
    if (!_NSGetExecutablePath(s_exe_path, &len)) {
        s_exe = s_exe_path;
    }
    return s_exe;
}
int s_parse_symbol(const char *symbol, void *addr, struct aws_stack_frame_info *frame) {
    /* symbols look like: <frame_idx>   <exe-or-shared-lib>         <addr> <function> + <offset>
     */
    const char *current_exe = s_get_executable_path();
    /* parse exe/shared lib */
    const char *exe_start = strstr(symbol, " ");
    while (aws_isspace(*exe_start)) {
        ++exe_start;
    }
    const char *exe_end = strstr(exe_start, " ");
    strncpy(frame->exe, exe_start, exe_end - exe_start);
    /* executables get basename'd, so restore the path */
    if (strstr(current_exe, frame->exe)) {
        strncpy(frame->exe, current_exe, strlen(current_exe));
    }
    s_whitelist_chars(frame->exe);

    /* parse addr */
    const char *addr_start = strstr(exe_end, "0x");
    const char *addr_end = strstr(addr_start, " ");
    strncpy(frame->addr, addr_start, addr_end - addr_start);

    /* parse function */
    const char *function_start = strstr(addr_end, " ") + 1;
    const char *function_end = strstr(function_start, " ");
    /* truncate function name if needed */
    size_t function_len = function_end - function_start;
    if (function_len >= (sizeof(frame->function) - 1)) {
        function_len = sizeof(frame->function) - 1;
    }
    strncpy(frame->function, function_start, function_len);

    /* find base addr for library/exe */
    Dl_info addr_info;
    dladdr(addr, &addr_info);
    snprintf(frame->base, sizeof(frame->base), "0x%p", addr_info.dli_fbase);

    return AWS_OP_SUCCESS;
}

void s_resolve_cmd(char *cmd, size_t len, struct aws_stack_frame_info *frame) {
    snprintf(cmd, len, "atos -o %s -l %s %s", frame->exe, frame->base, frame->addr);
}
#    else
int s_parse_symbol(const char *symbol, void *addr, struct aws_stack_frame_info *frame) {
    /* symbols look like: <exe-or-shared-lib>(<function>+<addr>) [0x<addr>]
     *                or: <exe-or-shared-lib> [0x<addr>]
     *                or: [0x<addr>]
     */
    (void)addr;
    const char *open_paren = strstr(symbol, "(");
    const char *close_paren = strstr(symbol, ")");
    const char *exe_end = open_paren;
    /* there may not be a function in parens, or parens at all */
    if (open_paren == NULL || close_paren == NULL) {
        exe_end = strstr(symbol, "[");
        if (!exe_end) {
            return AWS_OP_ERR;
        }
        /* if exe_end == symbol, there's no exe */
        if (exe_end != symbol) {
            exe_end -= 1;
        }
    }

    ptrdiff_t exe_len = exe_end - symbol;
    if (exe_len > 0) {
        strncpy(frame->exe, symbol, exe_len);
    }
    s_whitelist_chars(frame->exe);

    long function_len = (open_paren && close_paren) ? close_paren - open_paren - 1 : 0;
    if (function_len > 0) { /* dynamic symbol was found */
        /* there might be (<function>+<addr>) or just (<function>) */
        const char *function_start = open_paren + 1;
        const char *plus = strstr(function_start, "+");
        const char *function_end = (plus) ? plus : close_paren;
        if (function_end > function_start) {
            function_len = function_end - function_start;
            strncpy(frame->function, function_start, function_len);
        } else if (plus) {
            long addr_len = close_paren - plus - 1;
            strncpy(frame->addr, plus + 1, addr_len);
        }
    }
    if (frame->addr[0] == 0) {
        /* use the address in []'s, since it's all we have */
        const char *addr_start = strstr(exe_end, "[") + 1;
        char *addr_end = strstr(addr_start, "]");
        if (!addr_end) {
            return AWS_OP_ERR;
        }
        strncpy(frame->addr, addr_start, addr_end - addr_start);
    }

    return AWS_OP_SUCCESS;
}
void s_resolve_cmd(char *cmd, size_t len, struct aws_stack_frame_info *frame) {
    snprintf(cmd, len, "addr2line -afips -e %s %s", frame->exe, frame->addr);
}
#    endif

size_t aws_backtrace(void **stack_frames, size_t num_frames) {
    return backtrace(stack_frames, (int)aws_min_size(num_frames, INT_MAX));
}

char **aws_backtrace_symbols(void *const *stack_frames, size_t stack_depth) {
    return backtrace_symbols(stack_frames, (int)aws_min_size(stack_depth, INT_MAX));
}

char **aws_backtrace_addr2line(void *const *stack_frames, size_t stack_depth) {
    char **symbols = aws_backtrace_symbols(stack_frames, stack_depth);
    AWS_FATAL_ASSERT(symbols);
    struct aws_byte_buf lines;
    aws_byte_buf_init(&lines, aws_default_allocator(), stack_depth * 256);

    /* insert pointers for each stack entry */
    memset(lines.buffer, 0, stack_depth * sizeof(void *));
    lines.len += stack_depth * sizeof(void *);

    /* symbols look like: <exe-or-shared-lib>(<function>+<addr>) [0x<addr>]
     *                or: <exe-or-shared-lib> [0x<addr>]
     * start at 1 to skip the current frame (this function) */
    for (size_t frame_idx = 0; frame_idx < stack_depth; ++frame_idx) {
        struct aws_stack_frame_info frame;
        AWS_ZERO_STRUCT(frame);
        const char *symbol = symbols[frame_idx];
        if (s_parse_symbol(symbol, stack_frames[frame_idx], &frame)) {
            goto parse_failed;
        }

        /* TODO: Emulate libunwind */
        char cmd[sizeof(struct aws_stack_frame_info)] = {0};
        s_resolve_cmd(cmd, sizeof(cmd), &frame);
        FILE *out = popen(cmd, "r");
        if (!out) {
            goto parse_failed;
        }
        char output[1024];
        if (fgets(output, sizeof(output), out)) {
            /* if addr2line or atos don't know what to do with an address, they just echo it */
            /* if there are spaces in the output, then they resolved something */
            if (strstr(output, " ")) {
                symbol = output;
            }
        }
        pclose(out);

    parse_failed:
        /* record the pointer to where the symbol will be */
        *((char **)&lines.buffer[frame_idx * sizeof(void *)]) = (char *)lines.buffer + lines.len;
        struct aws_byte_cursor line_cursor = aws_byte_cursor_from_c_str(symbol);
        line_cursor.len += 1; /* strings must be null terminated, make sure we copy the null */
        aws_byte_buf_append_dynamic(&lines, &line_cursor);
    }
    free(symbols);
    return (char **)lines.buffer; /* caller is responsible for freeing */
}

void aws_backtrace_print(FILE *fp, void *call_site_data) {
    siginfo_t *siginfo = call_site_data;
    if (siginfo) {
        fprintf(fp, "Signal received: %d, errno: %d\n", siginfo->si_signo, siginfo->si_errno);
        if (siginfo->si_signo == SIGSEGV) {
            fprintf(fp, "  SIGSEGV @ 0x%p\n", siginfo->si_addr);
        }
    }

    void *stack_frames[AWS_BACKTRACE_DEPTH];
    size_t stack_depth = aws_backtrace(stack_frames, AWS_BACKTRACE_DEPTH);
    char **symbols = aws_backtrace_symbols(stack_frames, stack_depth);
    if (symbols == NULL) {
        fprintf(fp, "Unable to decode backtrace via backtrace_symbols\n");
        return;
    }

    fprintf(fp, "################################################################################\n");
    fprintf(fp, "Stack trace:\n");
    fprintf(fp, "################################################################################\n");
    for (size_t frame_idx = 1; frame_idx < stack_depth; ++frame_idx) {
        const char *symbol = symbols[frame_idx];
        fprintf(fp, "%s\n", symbol);
    }
    fflush(fp);

    free(symbols);
}

void aws_backtrace_log(int log_level) {
    void *stack_frames[AWS_BACKTRACE_DEPTH];
    size_t num_frames = aws_backtrace(stack_frames, AWS_BACKTRACE_DEPTH);
    if (!num_frames) {
        AWS_LOGF(log_level, AWS_LS_COMMON_GENERAL, "Unable to capture backtrace");
        return;
    }
    char **symbols = aws_backtrace_symbols(stack_frames, num_frames);
    for (size_t line = 0; line < num_frames; ++line) {
        const char *symbol = symbols[line];
        AWS_LOGF(log_level, AWS_LS_COMMON_GENERAL, "%s", symbol);
    }
    free(symbols);
}

#else
void aws_backtrace_print(FILE *fp, void *call_site_data) {
    (void)call_site_data;
    fprintf(fp, "No call stack information available\n");
}

size_t aws_backtrace(void **stack_frames, size_t num_frames) {
    (void)stack_frames;
    (void)num_frames;
    return 0;
}

char **aws_backtrace_symbols(void *const *stack_frames, size_t stack_depth) {
    (void)stack_frames;
    (void)stack_depth;
    return NULL;
}

char **aws_backtrace_addr2line(void *const *stack_frames, size_t stack_depth) {
    (void)stack_frames;
    (void)stack_depth;
    return NULL;
}

void aws_backtrace_log(int log_level) {
    AWS_LOGF(log_level, AWS_LS_COMMON_GENERAL, "aws_backtrace_log: no execinfo compatible backtrace API available");
}
#endif /* AWS_HAVE_EXECINFO */

#if defined(AWS_OS_APPLE)
enum aws_platform_os aws_get_platform_build_os(void) {
    return AWS_PLATFORM_OS_MAC;
}
#else
enum aws_platform_os aws_get_platform_build_os(void) {
    return AWS_PLATFORM_OS_UNIX;
}
#endif /* AWS_OS_APPLE */
