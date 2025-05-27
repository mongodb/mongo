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

#include "mongo/db/namespace_string.h"

#include "mongo/db/database_name.h"
#include "mongo/util/immutable/map.h"
#include "mongo/util/immutable/unordered_map.h"

#include <benchmark/benchmark.h>

namespace mongo {

namespace {

constexpr StringData kMapCollName = "CollName"_sd;

constexpr size_t kSmallSize = 8;  // Under the small-string optimization limit
constexpr size_t kLongSize = 32;  // Over the small string optimization limit

DatabaseName makeDB(StringData db) {
    return DatabaseName::createDatabaseName_forTest(boost::none, db);
}

NamespaceString makeNS(StringData db, StringData coll) {
    return NamespaceString::createNamespaceString_forTest(db, coll);
}

template <typename T>
auto discardValue(T&& x) {
    return std::forward<T>(x);
}

std::string makePaddedString(const int i, const size_t sz) {
    std::string r = std::to_string(i);
    return std::string(sz - r.size(), 'x') + r;
}

std::vector<std::pair<DatabaseName, std::shared_ptr<int>>> createDataVectorForMap() {
    std::vector<std::pair<DatabaseName, std::shared_ptr<int>>> data;
    for (int i = 0; i < 50; ++i) {
        data.emplace_back(makeDB(makePaddedString(i, kSmallSize)), std::make_shared<int>(i));
    }
    for (int i = 0; i < 50; ++i) {
        data.emplace_back(makeDB(makePaddedString(i, kLongSize)), std::make_shared<int>(i));
    }

    return data;
}
}  // namespace

void BM_NamespaceStringCreation(benchmark::State& state) {
    const StringData dbName =
        "ThisIsAVeryLongDatabaseNameThatIDontCareAboutAndPerhapsTheGetty"_sd.substr(0,
                                                                                    state.range(0));
    const StringData collName = "COL"_sd;
    for (auto _ : state) {
        benchmark::DoNotOptimize(makeNS(dbName, collName));
    }
}

void BM_CreateShortNsFromConstexpr(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(NamespaceString(NamespaceString::kRsOplogNamespace));
    }
}

void BM_CreateLongNsFromConstexpr(benchmark::State& state) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            NamespaceString(NamespaceString::kReshardingTxnClonerProgressNamespace));
    }
}

void BM_NamespaceStringShortDbName(benchmark::State& state) {
    const auto dbName = "short"_sd;
    NamespaceString ns = makeNS(dbName, kMapCollName);

    for (auto _ : state) {
        benchmark::DoNotOptimize(ns.dbName());
    }
}

void BM_NamespaceStringLongDbName(benchmark::State& state) {
    const std::string dbName(60, 'x');
    NamespaceString ns = makeNS(dbName, kMapCollName);

    for (auto _ : state) {
        benchmark::DoNotOptimize(ns.dbName());
    }
}

void searchInMap(benchmark::State& state, const NamespaceString& ns) {
    immutable::map<DatabaseName, std::shared_ptr<int>> dbMap;

    auto data = createDataVectorForMap();
    for (auto& pair : data) {
        dbMap = dbMap.insert(std::move(pair.first), std::move(pair.second));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(discardValue(dbMap.find(ns.dbName())));
    }
}

void BM_NamespaceStringShortDbNameMapLookupExist(benchmark::State& state) {
    searchInMap(state, makeNS(makePaddedString(12, kSmallSize), kMapCollName));
}

void BM_NamespaceStringLongDbNameMapLookupExist(benchmark::State& state) {
    searchInMap(state, makeNS(makePaddedString(34, kLongSize), kMapCollName));
}

void BM_NamespaceStringDbNameMapLookupDoesntExist(benchmark::State& state) {
    searchInMap(state, makeNS("DbNameMissing", kMapCollName));
}

void searchInUnorderedMap(benchmark::State& state, const NamespaceString& ns) {
    auto data = createDataVectorForMap();
    immutable::unordered_map<DatabaseName, std::shared_ptr<int>> dbMap{data.begin(), data.end()};

    for (auto _ : state) {
        benchmark::DoNotOptimize(discardValue(dbMap.find(ns.dbName())));
    }
}

void BM_NamespaceStringShortDbNameUnorderedMapLookupExist(benchmark::State& state) {
    searchInUnorderedMap(state, makeNS(makePaddedString(45, kSmallSize), kMapCollName));
}

void BM_NamespaceStringLongDbNameUnorderedMapLookupExist(benchmark::State& state) {
    searchInUnorderedMap(state, makeNS(makePaddedString(5, kLongSize), kMapCollName));
}

void BM_NamespaceStringDbNameUnorderedMapLookupDoesntExist(benchmark::State& state) {
    searchInUnorderedMap(state, makeNS("LongDatabaseNameThatIsNotInTheMap", kMapCollName));
}

void BM_NamespaceStringIsShardLocalCollection_ConfigDb_ShardLocal(benchmark::State& state) {
    const std::string dbName("config");
    const NamespaceString ns = makeNS(dbName, "foo");

    for (auto _ : state) {
        benchmark::DoNotOptimize(discardValue(ns.isShardLocalNamespace()));
    }
}

void BM_NamespaceStringIsShardLocalCollection_LocalDb(benchmark::State& state) {
    const std::string dbName("local");
    const NamespaceString ns = makeNS(dbName, kMapCollName);

    for (auto _ : state) {
        benchmark::DoNotOptimize(discardValue(ns.isShardLocalNamespace()));
    }
}

void BM_NamespaceStringIsShardLocalCollection_UserDb(benchmark::State& state) {
    const std::string dbName("test");
    const NamespaceString ns = makeNS(dbName, kMapCollName);

    for (auto _ : state) {
        benchmark::DoNotOptimize(discardValue(ns.isShardLocalNamespace()));
    }
}

BENCHMARK(BM_NamespaceStringCreation)->DenseRange(1, 63);

BENCHMARK(BM_CreateShortNsFromConstexpr);

BENCHMARK(BM_CreateLongNsFromConstexpr);

BENCHMARK(BM_NamespaceStringShortDbName);

BENCHMARK(BM_NamespaceStringLongDbName);

BENCHMARK(BM_NamespaceStringShortDbNameMapLookupExist);

BENCHMARK(BM_NamespaceStringLongDbNameMapLookupExist);

BENCHMARK(BM_NamespaceStringDbNameMapLookupDoesntExist);

BENCHMARK(BM_NamespaceStringShortDbNameUnorderedMapLookupExist);

BENCHMARK(BM_NamespaceStringLongDbNameUnorderedMapLookupExist);

BENCHMARK(BM_NamespaceStringDbNameUnorderedMapLookupDoesntExist);

BENCHMARK(BM_NamespaceStringIsShardLocalCollection_ConfigDb_ShardLocal);
BENCHMARK(BM_NamespaceStringIsShardLocalCollection_LocalDb);
BENCHMARK(BM_NamespaceStringIsShardLocalCollection_UserDb);

}  // namespace mongo
