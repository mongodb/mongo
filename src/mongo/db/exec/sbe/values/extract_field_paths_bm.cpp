// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/db/exec/sbe/values/object_walk_node.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"

#include <benchmark/benchmark.h>


namespace mongo::sbe {

struct PathTestCase {
    value::Path path;
    BSONObj projectValue;
};

class ExtractFieldPathsFixture : public benchmark::Fixture {
public:
    void perfectTree(benchmark::State& state,
                     int branchingFactor,
                     int depth,
                     int numPathsToProject = -1);
};

using Get = value::Get;
using Traverse = value::Traverse;
using Id = value::Id;

void callback(value::ObjectWalkNode<value::ScalarProjectionPositionInfoRecorder>* node,
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
    std::vector<value::Path> paths;
    value::Path curPath;
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

    value::ObjectWalkNode<value::ScalarProjectionPositionInfoRecorder> tree;
    // Construct extractor.
    std::vector<value::ScalarProjectionPositionInfoRecorder> recorders;
    recorders.reserve(tests.size());
    for (auto& tc : tests) {
        recorders.emplace_back();
        tree.add(tc.path, nullptr, &recorders.back());
    }
    value::TagValueOwned input =
        value::TagValueOwned::fromRaw(stage_builder::makeValue(root.done()));

    // Extract paths from input data in a single pass.
    for (auto _ : state) {
        walkField<value::ScalarProjectionPositionInfoRecorder>(&tree,
                                                               input.tag(),
                                                               input.value(),
                                                               nullptr /* bsonPtr */,
                                                               callback,
                                                               true /*traverseArrays*/);
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
