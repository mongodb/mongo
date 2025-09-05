/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/cell_interface.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

#include <benchmark/benchmark.h>
#include <boost/algorithm/string/join.hpp>

namespace mongo::sbe {

struct PathTestCase {
    value::CellBlock::Path path;
    BSONObj projectValue;
};

class ExtractFieldPathsFixture : public benchmark::Fixture {
public:
    void perfectTree(benchmark::State& state,
                     int branchingFactor,
                     int depth,
                     int numPathsToProject = -1);
};

using Get = value::CellBlock::Get;
using Traverse = value::CellBlock::Traverse;
using Id = value::CellBlock::Id;

void callback(value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder>* node,
              value::TypeTags eltTag,
              value::Value eltVal,
              const char* bson) {
    if (auto rec = node->projRecorder) {
        rec->recordValue(eltTag, eltVal);
    }
}

void ExtractFieldPathsFixture::perfectTree(benchmark::State& state,
                                           int branchingFactor,
                                           int depth,
                                           int numPathsToProject /*=-1*/) {
    BSONObjBuilder bob;
    int curIdx = 0;
    std::vector<value::CellBlock::Path> paths;
    value::CellBlock::Path curPath;
    std::function<void(BSONObjBuilder&, int)> rec = [&](BSONObjBuilder& curBob, int curDepth) {
        if (curDepth == depth) {
            curPath.push_back(Id{});
            BSONObj ret = curBob.done();
            paths.push_back(curPath);
            curPath.pop_back();
            return;
        }
        for (int i = 0; i < branchingFactor; i++) {
            std::stringstream curField;
            curField << "f" << (curIdx++);
            if (curDepth > 0) {
                curPath.push_back(Traverse{});
            }
            curPath.push_back(Get{curField.str()});
            BSONObjBuilder childBob(curBob.subobjStart(curField.str()));
            rec(childBob, curDepth + 1);
            if (curDepth > 0) {
                curPath.pop_back();
            }
            curPath.pop_back();
        }
    };
    BSONObjBuilder root;
    rec(root, 0);
    std::vector<PathTestCase> tests;
    int i = 0;
    for (const auto& path : paths) {
        // If numPathsToProject is specified, only project the first numPathsToProject paths.
        if (numPathsToProject != -1 && i == numPathsToProject) {
            break;
        }
        tests.push_back(PathTestCase{.path = path, .projectValue = fromjson("{result: {}}")});
        i++;
    }

    value::BsonWalkNode<value::ScalarProjectionPositionInfoRecorder> tree;
    // Construct extractor.
    std::vector<value::ScalarProjectionPositionInfoRecorder> recorders;
    recorders.reserve(tests.size());
    for (auto& tc : tests) {
        recorders.emplace_back();
        tree.add(tc.path, nullptr, &recorders.back());
    }
    auto [inputTag, inputVal] = stage_builder::makeValue(root.done());
    value::ValueGuard vg{inputTag, inputVal};  // Free input value's memory on exit.

    // Extract paths from input data in a single pass.
    for (auto _ : state) {
        walkField<value::ScalarProjectionPositionInfoRecorder>(
            &tree, inputTag, inputVal, nullptr /* bsonPtr */, callback);
    }
}

#define BENCHMARK_EXTRACT_FIELD_PATHS(Fixture)                                             \
    BENCHMARK_F(Fixture, perfectBinaryTreeDepth5AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 2, 5);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfectBinaryTreeDepth6AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 2, 6);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfectBinaryTreeDepth7AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 2, 7);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfectTernaryTreeDepth5AllLeaves)(benchmark::State & state) {    \
        perfectTree(state, 3, 5);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfectTernaryTreeDepth6AllLeaves)(benchmark::State & state) {    \
        perfectTree(state, 3, 6);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfectTernaryTreeDepth7AllLeaves)(benchmark::State & state) {    \
        perfectTree(state, 3, 7);                                                          \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect16naryTreeDepth1First8Leaves)(benchmark::State & state) {  \
        perfectTree(state, 16, 1, 8);                                                      \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect16naryTreeDepth1AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 16, 1);                                                         \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect32naryTreeDepth1First16Leaves)(benchmark::State & state) { \
        perfectTree(state, 32, 1, 16);                                                     \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect32naryTreeDepth1AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 32, 1);                                                         \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect64naryTreeDepth1AllLeaves)(benchmark::State & state) {     \
        perfectTree(state, 64, 1);                                                         \
    }                                                                                      \
    BENCHMARK_F(Fixture, perfect64naryTreeDepth1First32Leaves)(benchmark::State & state) { \
        perfectTree(state, 64, 1, 32);                                                     \
    }

BENCHMARK_EXTRACT_FIELD_PATHS(ExtractFieldPathsFixture);

#undef BENCHMARK_EXTRACT_FIELD_PATHS
}  // namespace mongo::sbe
