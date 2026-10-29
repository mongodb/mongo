// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/profile_filter.h"

#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"
#include "mongo/util/processinfo.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

DatabaseProfileSettings settings;

static void BM_ProfileFilter_Get_Set_Default(benchmark::State& state) {
    if (state.thread_index == 0) {
        settings.setAllDatabaseProfileFiltersAndDefault(
            std::shared_ptr<ProfileFilterImpl>(nullptr));
    }

    int i = 0;
    for (auto keepRunning : state) {
        if (state.thread_index == i++ % 20) {
            settings.setAllDatabaseProfileFiltersAndDefault(
                std::shared_ptr<ProfileFilterImpl>(nullptr));
        }
        benchmark::DoNotOptimize(settings.getDefaultFilter());
        benchmark::ClobberMemory();
    }
}

static void BM_ProfileFilter_GetSettingsDefault(benchmark::State& state) {
    const DatabaseName testDB = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(settings.getDatabaseProfileSettings(testDB));
        benchmark::ClobberMemory();
    }
}

static void BM_ProfileFilter_GetSettingsNonDefault(benchmark::State& state) {
    const DatabaseName testDB = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    if (state.thread_index == 0) {
        settings.setDatabaseProfileSettings(testDB, {1, nullptr, Milliseconds(5000)});
    }

    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(settings.getDatabaseProfileSettings(testDB));
        benchmark::ClobberMemory();
    }
}

static void BM_ProfileFilter_SetClear(benchmark::State& state) {
    const DatabaseName testDB = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    if (state.thread_index == 0) {
    }

    for (auto keepRunning : state) {
        settings.setDatabaseProfileSettings(testDB, {1, nullptr, Milliseconds(5000)});
        settings.clearDatabaseProfileSettings(testDB);
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_ProfileFilter_Get_Set_Default)
    ->Threads(1)
    ->Threads(ProcessInfo::getNumAvailableCores())
    ->Threads(2 * ProcessInfo::getNumAvailableCores());
BENCHMARK(BM_ProfileFilter_GetSettingsDefault)
    ->Threads(1)
    ->Threads(ProcessInfo::getNumAvailableCores())
    ->Threads(2 * ProcessInfo::getNumAvailableCores());
BENCHMARK(BM_ProfileFilter_GetSettingsNonDefault)
    ->Threads(1)
    ->Threads(ProcessInfo::getNumAvailableCores())
    ->Threads(2 * ProcessInfo::getNumAvailableCores());
BENCHMARK(BM_ProfileFilter_SetClear)
    ->Threads(1)
    ->Threads(ProcessInfo::getNumAvailableCores())
    ->Threads(2 * ProcessInfo::getNumAvailableCores());

}  // namespace
}  // namespace mongo
