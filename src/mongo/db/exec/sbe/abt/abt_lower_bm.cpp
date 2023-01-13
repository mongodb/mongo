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

#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"

namespace mongo::optimizer {
namespace {
class ABTLoweringFixture : public benchmark::Fixture {
    ProjectionName scanLabel = ProjectionName{"scan0"_sd};
    NodeToGroupPropsMap _nodeMap;
    // This can be modified by tests that need other labels.
    FieldProjectionMap _fieldProjMap{{}, {scanLabel}, {}};
    int lastNodeGenerated = 0;

protected:
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
                    {false}};
        properties::setPropertyOverwrite(n._physicalProps, properties::ProjectionRequirement({}));
        return n;
    }
    ABT&& _node(ABT&& tree) {
        _nodeMap.insert({tree.cast<Node>(), makeNodeProp()});
        return std::move(tree);
    }
    void benchmarkABTLowering(benchmark::State& state, const ABT& n) {
        for (auto keepRunning : state) {
            auto m = Metadata(
                {{"collName",
                  createScanDef(
                      {{"type", "mongod"}, {"database", "test"}, {"uuid", UUID::gen().toString()}},
                      {{"idx", makeIndexDefinition("a", CollationOp::Ascending)}})}});

            auto env = VariableEnvironment::build(n);
            SlotVarMap map;
            boost::optional<sbe::value::SlotId> ridSlot;
            sbe::value::SlotIdGenerator ids;

            benchmark::DoNotOptimize(
                SBENodeLowering{env, map, ridSlot, ids, m, _nodeMap, false}.optimize(n));
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_F(ABTLoweringFixture, BM_LowerPhysicalScan)(benchmark::State& state) {
    benchmarkABTLowering(
        state,
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"root0"}}, {}}, "collName", false)));
}

}  // namespace
}  // namespace mongo::optimizer
