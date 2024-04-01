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

#include <benchmark/benchmark.h>

#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime_list.h"
#include "mongo/logv2/log_domain_global.h"


namespace mongo {
namespace repl {
namespace {

void BM_InsertHeadEmptyList(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        for (auto i = 0; i < 16; i++) {
            list.insert(list.begin(), Timestamp(i, i + 1));
        }
    }
}

void BM_InsertHeadReuseList(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        // Prepare a list with enough free slots
        state.PauseTiming();
        for (auto i = 0; i < 16; i++) {
            list.insert(list.begin(), Timestamp(i, i + 1));
        }
        list.clear_forTest();
        state.ResumeTiming();

        for (auto i = 0; i < 16; i++) {
            list.insert(list.begin(), Timestamp(i, i + 1));
        }
    }
}

void BM_InsertTail(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        for (auto i = 0; i < 16; i++) {
            list.insert(list.end(), Timestamp(1, 2));
        }
    }
}

void BM_EraseHead(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        // Exclude preparation from measurement
        state.PauseTiming();
        for (auto i = 0; i < 16; i++) {
            list.insert(list.begin(), Timestamp(i, i + 1));
        }
        state.ResumeTiming();

        while (!list.empty()) {
            list.erase(list.begin());
        }
    }
}

void BM_EraseTail(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        // Exclude preparation from measurement
        state.PauseTiming();
        for (auto i = 0; i < 16; i++) {
            list.insert(list.begin(), Timestamp(i, i + 1));
        }
        state.ResumeTiming();

        while (!list.empty()) {
            list.erase(std::prev(list.end()));
        }
    }
}

void BM_EraseMiddle(benchmark::State& state, OpTimeList& list) {
    for (auto _ : state) {
        // Exclude preparation from measurement
        state.PauseTiming();
        auto itMid = list.end();
        for (auto i = 0; i < 16; i += 2) {
            list.insert(itMid, Timestamp(i, i + 1));
            itMid = list.insert(itMid, Timestamp(i + 1, i + 2));
        }
        auto itPrev = std::prev(itMid);
        auto itNext = std::next(itMid);
        state.ResumeTiming();

        // Erase the first half except the first
        while (itPrev != list.begin()) {
            auto it = itPrev;
            itPrev = std::prev(it);
            list.erase(it);
        }

        // Erase the second half except the last
        while (itNext != list.end()) {
            auto it = itNext;
            itNext = std::next(it);
            list.erase(it);
        }
    }
}

void BM_ListInsertHeadEmptyList(benchmark::State& state) {
    OpTimeList list;
    BM_InsertHeadEmptyList(state, list);
}

BENCHMARK(BM_ListInsertHeadEmptyList)->MinTime(10.0);

void BM_ListInsertHeadReuseList(benchmark::State& state) {
    OpTimeList list;
    BM_InsertHeadReuseList(state, list);
}

BENCHMARK(BM_ListInsertHeadReuseList)->MinTime(10.0);

void BM_ListInsertTail(benchmark::State& state) {
    OpTimeList list;
    BM_InsertTail(state, list);
}

BENCHMARK(BM_ListInsertTail)->MinTime(10.0);

void BM_ListEraseHead(benchmark::State& state) {
    OpTimeList list;
    BM_EraseHead(state, list);
}
BENCHMARK(BM_ListEraseHead)->MinTime(10.0);

void BM_ListEraseTail(benchmark::State& state) {
    OpTimeList list;
    BM_EraseTail(state, list);
}
BENCHMARK(BM_ListEraseTail)->MinTime(10.0);

void BM_ListEraseMiddle(benchmark::State& state) {
    OpTimeList list;
    BM_EraseMiddle(state, list);
}
BENCHMARK(BM_ListEraseMiddle)->MinTime(10.0);
}  // namespace
}  // namespace repl
}  // namespace mongo
