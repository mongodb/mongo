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

#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/unittest/unittest.h"

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/core.h>

namespace mongo {
namespace {

class BM_WriteConflict : public benchmark::Fixture {
protected:
    struct ThreadLocalState {
        std::unique_ptr<Client, ServiceContext::ClientDeleter> client;
        std::unique_ptr<OperationContext, ServiceContext::OperationContextDeleter> opCtx;
        int committed = 0;
        int attempts = 0;
        int threadIndex;
    };

    auto threadLocalSetUp(int threadIndex) {
        ThreadLocalState tls;
        tls.threadIndex = threadIndex;
        tls.client = harness->getServiceContext()->getService()->makeClient(
            fmt::format("thread {}", threadIndex));
        tls.opCtx = harness->newOperationContext(tls.client.get());
        return tls;
    }

    void logStats(benchmark::State& state, const ThreadLocalState& tls) {
        state.counters["Committed"] =
            benchmark::Counter(tls.committed, benchmark::Counter::kIsRate);
        state.counters["Attempted"] = benchmark::Counter(tls.attempts, benchmark::Counter::kIsRate);
    }

    void attemptWrite(ThreadLocalState& state) {
        ++state.attempts;
        Lock::GlobalLock lk(state.opCtx.get(), MODE_IX);
        WriteUnitOfWork wuow(state.opCtx.get());

        RecordData unused;
        ASSERT(rs->findRecord(state.opCtx.get(),
                              *shard_role_details::getRecoveryUnit(state.opCtx.get()),
                              RecordId(1),
                              &unused));

        auto data = BSON("tid" << state.threadIndex << "inc" << state.committed);
        ASSERT_OK(rs->updateRecord(state.opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(state.opCtx.get()),
                                   RecordId(1),
                                   data.objdata(),
                                   data.objsize()));

        wuow.commit();
        ++state.committed;
    }

    template <typename Setup, typename ConflictHandler>
    void test(benchmark::State& state,
              Setup&& perIterationSetup,
              ConflictHandler&& onWriteConflict) {
        auto tls = threadLocalSetUp(state.thread_index);
        for (auto _ : state) {
            perIterationSetup();
            while (true) {
                try {
                    attemptWrite(tls);
                    break;
                } catch (const WriteConflictException&) {
                    onWriteConflict(tls);
                }
            }
        }
        logStats(state, tls);
    }

private:
    void SetUp(benchmark::State& state) override {
        if (state.thread_index != 0) {
            _threads.wait(0);
            return;
        }

        {
            auto& lv2Manager = logv2::LogManager::global();
            logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
            lv2Config.makeDisabled();
            uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
        }

        harness = newRecordStoreHarnessHelper();
        rs = harness->newRecordStore("ns");

        auto data = BSON("init" << true);
        auto client = harness->serviceContext()->getService()->makeClient("init");
        auto opCtx = harness->newOperationContext(client.get());
        Lock::GlobalLock lk(opCtx.get(), MODE_IS);
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(rs->insertRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   RecordId(1),
                                   data.objdata(),
                                   data.objsize(),
                                   Timestamp()));
        uow.commit();

        _threads.store(state.threads);
        _threads.notifyAll();
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index != 0) {
            if (_threads.subtractAndFetch(1) == 1) {
                _threads.notifyAll();
            }
            return;
        }

        for (int count = _threads.load(); count > 1; count = _threads.load()) {
            _threads.wait(count);
        }

        rs.reset();
        harness.reset();
        _threads.store(0);
    }

    WaitableAtomic<int> _threads{0};
    std::unique_ptr<RecordStoreHarnessHelper> harness;
    std::unique_ptr<RecordStore> rs;
};

BENCHMARK_DEFINE_F(BM_WriteConflict, WriteConflictRetry)(benchmark::State& state) {
    auto tls = threadLocalSetUp(state.thread_index);
    for (auto _ : state) {
        writeConflictRetry(
            tls.opCtx.get(), "test", NamespaceString(), [&]() { attemptWrite(tls); });
    }
    logStats(state, tls);
}
BENCHMARK_REGISTER_F(BM_WriteConflict, WriteConflictRetry)
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(64)
    ->Threads(128);

BENCHMARK_DEFINE_F(BM_WriteConflict, NoBackoff)(benchmark::State& state) {
    test(
        state,
        [] {
            // No setup required
        },
        [](auto&) {
            // Do nothing on conflicts
        });
};
BENCHMARK_REGISTER_F(BM_WriteConflict, NoBackoff)
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(64)
    ->Threads(128);

BENCHMARK_DEFINE_F(BM_WriteConflict, OldBackoff)(benchmark::State& state) {
    int attempt = 0;
    test(
        state,
        [&] { attempt = 0; },
        [&](auto&) {
            if (attempt < 4) {
                // no-op
            } else if (attempt < 10) {
                sleepmillis(1);
            } else if (attempt < 100) {
                sleepmillis(5);
            } else if (attempt < 200) {
                sleepmillis(10);
            } else {
                sleepmillis(100);
            }
        });
};
BENCHMARK_REGISTER_F(BM_WriteConflict, OldBackoff)
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(64)
    ->Threads(128);

BENCHMARK_DEFINE_F(BM_WriteConflict, ExponentialBackoff)(benchmark::State& state) {
    int conflicts = 0;
    test(
        state,
        [&] { conflicts = 0; },
        [&](auto& tls) {
            ++conflicts;
            static constexpr double kBase = 10;
            static constexpr double kMaxMicros = 500'000;
            double micros = std::min(kMaxMicros, std::pow(kBase, conflicts));
            auto& prng = tls.opCtx->getClient()->getPrng();
            micros -= ((micros / 2) * prng.nextCanonicalDouble());
            sleepmicros(static_cast<long long>(micros));
        });
};
BENCHMARK_REGISTER_F(BM_WriteConflict, ExponentialBackoff)
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(64)
    ->Threads(128);

BENCHMARK_DEFINE_F(BM_WriteConflict, TimeBasedBackoff)(benchmark::State& state) {
    int conflicts = 0;
    double elapsed = 0;
    uint64_t startTime = 0;
    test(
        state,
        [&] {
            conflicts = 0;
            elapsed = 0;
            startTime = 0;
        },
        [&](auto&) {
            if (startTime != 0) {
                ++conflicts;
                elapsed += curTimeMicros64() - startTime;
                sleepmicros(elapsed / conflicts / 10 * std::pow(1.5, conflicts));
            }
            startTime = curTimeMicros64();
        });
};
BENCHMARK_REGISTER_F(BM_WriteConflict, TimeBasedBackoff)
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(64)
    ->Threads(128);

}  // namespace
}  // namespace mongo
