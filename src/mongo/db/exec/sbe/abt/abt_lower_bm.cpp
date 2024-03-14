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

#include <benchmark/benchmark.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/exec/sbe/abt/abt_lower_defs.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/optimizer/algebra/polyvalue.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/index_bounds.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo::optimizer {
namespace {
class ABTNodeLoweringFixture : public benchmark::Fixture {
protected:
    ProjectionName scanLabel = ProjectionName{"scan0"_sd};
    NodeToGroupPropsMap _nodeMap;
    // This can be modified by tests that need other labels.
    FieldProjectionMap _fieldProjMap{{}, {scanLabel}, {}};
    int lastNodeGenerated = 0;
    auto getNextNodeID() {
        return lastNodeGenerated++;
    }

    auto makeNodeProp() {
        NodeProps n{getNextNodeID(),
                    {},
                    {},
                    {},
                    boost::none,
                    CostType::fromDouble(0),
                    CostType::fromDouble(0),
                    CEType{0.0}};
        properties::setPropertyOverwrite(n._physicalProps, properties::ProjectionRequirement({}));
        return n;
    }
    ABT&& _node(ABT&& tree) {
        _nodeMap.insert({tree.cast<Node>(), makeNodeProp()});
        return std::move(tree);
    }
    void benchmarkLowering(benchmark::State& state, const ABT& n) {
        for (auto keepRunning : state) {
            auto m = Metadata(
                {{"collName",
                  createScanDef(
                      {{"type", "mongod"}, {"database", "test"}, {"uuid", UUID::gen().toString()}},
                      {{"idx", makeIndexDefinition("a", CollationOp::Ascending)}})}});

            auto env = VariableEnvironment::build(n);
            SlotVarMap map;
            sbe::RuntimeEnvironment runtimeEnv;
            boost::optional<sbe::value::SlotId> ridSlot;
            sbe::value::SlotIdGenerator ids;
            sbe::InputParamToSlotMap inputParamToSlotMap;

            benchmark::DoNotOptimize(
                SBENodeLowering{env, runtimeEnv, ids, inputParamToSlotMap, m, _nodeMap}.optimize(
                    n, map, ridSlot));
            benchmark::ClobberMemory();
        }
    }

    ABT scanNode(std::string scanProj = "scan0") {
        return make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{scanProj}}, {}}, "collName", false);
    }

    void runPathLowering(VariableEnvironment& env, PrefixId& prefixId, ABT& tree) {
        // Run rewriters while things change
        bool changed = false;
        do {
            changed = false;
            if (PathLowering{prefixId}.optimize(tree)) {
                changed = true;
            }
            if (ConstEval{env}.optimize(tree)) {
                changed = true;
            }
        } while (changed);
    }

    void runPathLowering(ABT& tree) {
        auto env = VariableEnvironment::build(tree);
        auto prefixId = PrefixId::createForTests();
        runPathLowering(env, prefixId, tree);
    }

    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList,
                       ABT source,
                       std::string sourceBinding) {
        for (auto [fieldName, bindingName] : bindingList) {
            auto field =
                make<EvalPath>(make<PathGet>(FieldNameType(fieldName), make<PathIdentity>()),
                               make<Variable>(ProjectionName(sourceBinding)));
            runPathLowering(field);
            ABT evalNode = make<EvaluationNode>(
                ProjectionName(bindingName), std::move(field), std::move(source));
            source = std::move(_node(std::move(evalNode)));
        }
        return source;
    }

    // Create bindings (as above) and also create a scan node source.
    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList) {
        return createBindings(std::move(bindingList), _node(scanNode("scan0")), "scan0");
    }
};

BENCHMARK_F(ABTNodeLoweringFixture, BM_LowerPhysicalScan)(benchmark::State& state) {
    benchmarkLowering(
        state,
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"root0"}}, {}}, "collName", false)));
}

BENCHMARK_F(ABTNodeLoweringFixture, BM_LowerIndexScanAndSeek)(benchmark::State& state) {
    auto indexScan = _node(
        make<IndexScanNode>(FieldProjectionMap{{ProjectionName{"rid"}}, {}, {}},
                            "collName",
                            "idx",
                            CompoundIntervalRequirement{{false, makeSeq(Constant::fromDouble(23))},
                                                        {true, makeSeq(Constant::fromDouble(35))}},
                            false));

    auto seek = _node(make<LimitSkipNode>(
        properties::LimitSkipRequirement(1, 0),
        _node(make<SeekNode>(ProjectionName{"rid"}, _fieldProjMap, "collName"))));

    benchmarkLowering(state,
                      _node(make<NestedLoopJoinNode>(JoinType::Inner,
                                                     ProjectionNameSet{"rid"},
                                                     Constant::boolean(true),
                                                     std::move(indexScan),
                                                     std::move(seek))));
}

BENCHMARK_F(ABTNodeLoweringFixture, BM_LowerGroupByPlan)(benchmark::State& state) {
    benchmarkLowering(
        state,
        _node(make<RootNode>(
            ProjectionNameOrderPreservingSet({"key1", "outFunc1"}),
            _node(make<GroupByNode>(
                ProjectionNameVector{"key1"},
                ProjectionNameVector{"outFunc1"},
                makeSeq(make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput1")))),
                GroupNodeType::Complete,
                createBindings({{"a", "key1"}, {"c", "aggInput1"}}))))));
}

BENCHMARK_DEFINE_F(ABTNodeLoweringFixture, BM_LowerNestedLoopJoins)(benchmark::State& state) {
    const int64_t nNLJs = state.range(0);
    ABT n = scanNode();
    for (int i = 0; i < nNLJs; i++) {
        ABT leftChild = _node(scanNode(str::stream() << "scan" << std::to_string(i + 1)));
        n = make<NestedLoopJoinNode>(JoinType::Inner,
                                     ProjectionNameSet{},
                                     Constant::boolean(true),
                                     std::move(leftChild),
                                     std::move(_node(std::move(n))));
    }
    benchmarkLowering(state, _node(std::move(n)));
}

BENCHMARK_REGISTER_F(ABTNodeLoweringFixture, BM_LowerNestedLoopJoins)
    ->Arg(1)
    ->Arg(20)
    ->Arg(40)
    ->Arg(100);

BENCHMARK_DEFINE_F(ABTNodeLoweringFixture, BM_LowerEvalNodes)(benchmark::State& state) {
    const int64_t nEvals = state.range(0);
    ABT n = scanNode();
    for (int i = 0; i < nEvals; i++) {
        n = make<EvaluationNode>(
            ProjectionName(std::string(str::stream() << "proj" << std::to_string(i))),
            Constant::int32(1337),
            _node(std::move(n)));
    }
    benchmarkLowering(state, _node(std::move(n)));
}

BENCHMARK_REGISTER_F(ABTNodeLoweringFixture, BM_LowerEvalNodes)->Arg(1)->Arg(20)->Arg(40)->Arg(100);

BENCHMARK_DEFINE_F(ABTNodeLoweringFixture, BM_LowerEvalNodesUnderNLJs)(benchmark::State& state) {
    const int64_t nEvals = state.range(0);
    const int64_t nNLJs = state.range(1);

    ABT n = scanNode();
    for (int i = 0; i < nNLJs; i++) {
        ABT leftChild = scanNode(str::stream() << "scan" << std::to_string(i + 1));
        for (int j = 0; j < nEvals; j++) {
            leftChild = make<EvaluationNode>(
                ProjectionName(std::string(str::stream() << "proj" << std::to_string(i) << "-"
                                                         << std::to_string(j))),
                Constant::int32(1337),
                _node(std::move(leftChild)));
        }
        n = make<NestedLoopJoinNode>(JoinType::Inner,
                                     ProjectionNameSet{},
                                     Constant::boolean(true),
                                     std::move(_node(std::move(leftChild))),
                                     std::move(_node(std::move(n))));
    }

    benchmarkLowering(state, _node(std::move(n)));
}

BENCHMARK_REGISTER_F(ABTNodeLoweringFixture, BM_LowerEvalNodesUnderNLJs)
    ->Args({1, 1})
    ->Args({2, 1})
    ->Args({1, 2})
    ->Args({2, 2})
    ->Args({4, 2})
    ->Args({2, 4})
    ->Args({4, 4})
    ->Args({5, 5});

void BM_LowerABTLetExpr(benchmark::State& state) {
    auto nLets = state.range(0);
    ABT n = Constant::boolean(true);
    for (int i = 0; i < nLets; i++) {
        n = make<Let>(ProjectionName{std::string(str::stream() << "var" << std::to_string(i))},
                      Constant::int32(i),
                      std::move(n));
    }
    for (auto keepRunning : state) {
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        sbe::RuntimeEnvironment runtimeEnv;
        sbe::value::SlotIdGenerator ids;
        sbe::InputParamToSlotMap inputParamToSlotMap;
        benchmark::DoNotOptimize(
            SBEExpressionLowering{env, map, runtimeEnv, ids, inputParamToSlotMap}.optimize(n));
        benchmark::ClobberMemory();
    }
}

BENCHMARK(BM_LowerABTLetExpr)->Arg(1)->Arg(10)->Arg(20)->Arg(40)->Arg(100);
}  // namespace
}  // namespace mongo::optimizer
