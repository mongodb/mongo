/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif  // __linux__

#include <benchmark/benchmark.h>
#include <memory>
#include <string>

#include "mongo/base/init.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/basic.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

/**
 * An RAII type that tracks the number of instructions and CPU cycles.
 */
class BenchmarkProfiler {
public:
    BenchmarkProfiler() : _perfFD(_openPerfEvent()), _start(_getCycle()) {}

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
    inline uint64_t _getCycle() const {
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

    const int _perfFD;
    const uint64_t _start;
};

class ServiceEntryPointCommonBenchmarkFixture : public benchmark::Fixture {
public:
    ServiceEntryPointCommonBenchmarkFixture() {
        // Do not emit logs that impact performance measurements. The following assumes that this
        // benchmark will have its own process-global state (i.e. has its own target).
        auto& settings = logv2::LogManager::global().getGlobalSettings();
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kASIO, logv2::LogSeverity::Error());
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kSharding,
                                          logv2::LogSeverity::Error());
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kNetwork,
                                          logv2::LogSeverity::Error());
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kControl,
                                          logv2::LogSeverity::Error());
    }

    void SetUp(::benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (_configuredThreads++)
            return;
        setGlobalServiceContext(ServiceContext::make());

        // Minimal set up necessary for ServiceEntryPoint.
        auto service = getGlobalServiceContext();

        ReadWriteConcernDefaults::create(service, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        auto replCoordMock = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        // Transition to primary so that the server can accept writes.
        invariant(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoordMock));
        service->getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointMongod>());

        _instructions.store(0);
        _cycles.store(0);
        _iterations.store(0);
    }

    void TearDown(::benchmark::State& state) override {
        stdx::lock_guard lk(_setupMutex);
        if (--_configuredThreads)
            return;
        setGlobalServiceContext({});

        const auto iterations = _iterations.load();
        const auto instructions = _instructions.load();
        const auto cycles = _cycles.load();
        state.counters["instructions"] = instructions;
        state.counters["instructions_per_iteration"] =
            static_cast<int>(static_cast<double>(instructions) / iterations);
        state.counters["cycles"] = cycles;
        state.counters["cycles_per_iteration"] = static_cast<double>(cycles) / iterations;
    }

    void doRequest(ServiceEntryPoint* sep, Client* client, Message& msg) {
        auto newOpCtx = client->makeOperationContext();
        iassert(sep->handleRequest(newOpCtx.get(), msg).getNoThrow());
    }

    void runBenchmark(benchmark::State& state, BSONObj obj) {
        auto strand = ClientStrand::make(getGlobalServiceContext()->getService()->makeClient(
            fmt::format("conn{}", _nextClientId.fetchAndAdd(1)), nullptr));
        OpMsgRequest request;
        request.body = obj;
        auto msg = request.serialize();
        strand->run([&] {
            auto client = strand->getClientPointer();
            auto sep = client->getService()->getServiceEntryPoint();
            BenchmarkProfiler bp;
            for (auto _ : state) {
                doRequest(sep, client, msg);
            }
            auto profile = bp.capture();
            _instructions.fetchAndAdd(profile.instructions);
            _cycles.fetchAndAdd(profile.cycles);
            _iterations.fetchAndAdd(state.iterations());
        });

        state.SetItemsProcessed(int64_t(state.iterations()));
    }

private:
    AtomicWord<uint64_t> _nextClientId{0};

    AtomicWord<uint64_t> _instructions;
    AtomicWord<uint64_t> _cycles;
    AtomicWord<uint64_t> _iterations;

    ReadWriteConcernDefaultsLookupMock _lookupMock;
    Mutex _setupMutex;
    size_t _configuredThreads = 0;
};

BENCHMARK_DEFINE_F(ServiceEntryPointCommonBenchmarkFixture, BM_SEP_PING)(benchmark::State& state) {
    runBenchmark(state,
                 BSON("ping" << 1 << "$db"
                             << "admin"));
}

/**
 * ASAN can't handle the # of threads the benchmark creates.
 * With sanitizers, run this in a diminished "correctness check" mode.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kMaxThreads = 1;
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif

BENCHMARK_REGISTER_F(ServiceEntryPointCommonBenchmarkFixture, BM_SEP_PING)
    ->ThreadRange(1, kMaxThreads);

/**
 * Required initializers, but this is a benchmark so nothing needs to be done.
 */
MONGO_INITIALIZER_GENERAL(ForkServer, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {}
MONGO_INITIALIZER(ServerLogRedirection)(mongo::InitializerContext*) {}


}  // namespace
}  // namespace mongo
