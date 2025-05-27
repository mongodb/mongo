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
        settings.setDatabaseProfileSettings(testDB,
                                            {1, std::shared_ptr<ProfileFilterImpl>(nullptr)});
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
        settings.setDatabaseProfileSettings(testDB,
                                            {1, std::shared_ptr<ProfileFilterImpl>(nullptr)});
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
