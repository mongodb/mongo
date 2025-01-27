/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif  // __linux__

#include <benchmark/benchmark.h>

#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic.h"

namespace mongo {

/**
 * An RAII type that tracks the number of instructions and CPU cycles.
 */
class BenchmarkProfiler {
public:
#ifdef __linux__
    ~BenchmarkProfiler() {
        if (_perfFD >= 0) {
            close(_perfFD);
        }
    }
#endif  // __linux__

    struct Profile {
        uint64_t instructions;
        uint64_t cycles;
    };

    Profile capture() const {
        Profile p;
        p.cycles = _getCycle() - _start;
        p.instructions = _instructions();
        return p;
    }

private:
    /**
     * Returns the cycle/timestamp counter of the processor using platform-specific instructions.
     *
     * We use `rdtsc` (Read Time-Stamp Counter) on x86, followed by a `lfence` to properly order the
     * execution of `rdtsc` with the instructions that follow. The instruction reads the processor’s
     * time-stamp counter into two 32-bit registers (i.e. EDX and EAX).
     *
     * On AArch64, we query the contents of the `cntvct` register after issuing an `isb` to enforce
     * ordering for the counter read, as suggested by AArch64's Generic Timer documentation:
     * https://developer.arm.com/documentation/102379/latest/
     *
     * The inline assembly instructions can be replaced with intrinsics, if desired.
     */
    uint64_t _getCycle() const {
#if defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        __asm__ __volatile__("lfence" ::: "memory");
        return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
        asm volatile("isb" : : : "memory");
        uint64_t tsc;
        asm volatile("mrs %0, cntvct_el0" : "=r"(tsc));
        return tsc;
#else
        return 0;
#endif
    }

    int _openPerfEvent() const {
#ifdef __linux__
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.type = PERF_TYPE_HARDWARE;
        pe.size = sizeof(pe);
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        pe.disabled = 1;
        pe.exclude_kernel = 1;
        pe.exclude_hv = 1;
        int fd = syscall(SYS_perf_event_open,
                         &pe,
                         0,   // pid: calling process/thread
                         -1,  // cpu: any CPU
                         -1,  // groupd_fd: group with only 1 member
                         0);  // flags
        ioctl(fd, PERF_EVENT_IOC_RESET, 0);
        ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        return fd;
#else
        return -1;
#endif  // __linux__
    }

    uint64_t _instructions() const {
#ifdef __linux__
        if (_perfFD >= 0) {
            long long count;
            invariant(read(_perfFD, &count, sizeof(count)) == sizeof(count));
            return count;
        }
#endif  // __linux__
        return 0;
    }

    const int _perfFD = _openPerfEvent();
    const uint64_t _start = _getCycle();
};

class BenchmarkWithProfiler : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        // Do not emit logs that impact performance measurements. The following assumes that this
        // benchmark will have its own process-global state (i.e. has its own target).
        auto& settings = logv2::LogManager::global().getGlobalSettings();
        for (size_t i = 0; i < logv2::LogComponent::kNumLogComponents; ++i) {
            settings.setMinimumLoggedSeverity(
                logv2::LogComponent{static_cast<logv2::LogComponent::Value>(i)},
                logv2::LogSeverity::Error());
        }
    }

    void TearDown(benchmark::State& state) override {}

    template <typename BenchmarkFunc>
    void runBenchmarkWithProfiler(const BenchmarkFunc& benchmarkFunc, benchmark::State& state) {
        BenchmarkProfiler bp;
        for (auto _ : state) {
            benchmarkFunc();
        }
        auto profile = bp.capture();
        state.counters["cycles_per_iteration"] =
            benchmark::Counter(profile.cycles, benchmark::Counter::kAvgIterations);
        state.counters["instructions_per_iteration"] =
            benchmark::Counter(profile.instructions, benchmark::Counter::kAvgIterations);
    }

private:
};

}  // namespace mongo
