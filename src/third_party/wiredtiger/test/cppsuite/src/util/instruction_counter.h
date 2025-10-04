/*-
 * Public Domain 2014-present MongoDB, Inc.
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

#pragma once

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "src/main/test.h"

namespace test_harness {

/*
 * Class that measures the number of instructions execute by a given function and adds the stats to
 * the statistics writer when destroyed.
 */
class instruction_counter {
public:
    instruction_counter(const std::string &id, const std::string &test_name);
    virtual ~instruction_counter();

    /* Appends the stat to the perf file. */
    void append_stats();

    /*
     * Counts hardware instructions used for a given operation and keeps track of how many
     * operations have been executed.
     */
    template <typename T>
    int
    track(T lambda)
    {
        int fd = syscall(SYS_perf_event_open, &_pe,
          0,  // pid: calling process/thread
          -1, // cpu: any CPU
          -1, // groupd_fd: group with only 1 member
          0); // flags
        testutil_assert(fd != -1);
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

        int ret = lambda();

        long long count;

        ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
        ssize_t bytes_read = read(fd, &count, sizeof(count));
        testutil_assert(bytes_read == sizeof(count));

        _instruction_count = count;

        if (fd > 0) {
            close(fd);
        }
        return ret;
    }

private:
    std::string _id;
    std::string _test_name;
    uint64_t _instruction_count;
    struct perf_event_attr _pe;
};
} // namespace test_harness
