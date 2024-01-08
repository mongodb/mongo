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

#include "mongo/db/query/optimizer/cascades/interfaces.h"
#include "mongo/db/query/optimizer/cascades/memo.h"
#include "mongo/db/query/optimizer/containers.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/node.h"  // IWYU pragma: keep
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/partial_schema_requirements.h"
#include "mongo/db/query/optimizer/reference_tracker.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/syntax/path.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/const_fold_interface.h"
#include "mongo/db/query/optimizer/utils/unit_test_abt_literals.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/optimizer/utils/utils.h"
#include "mongo/util/debugger.h"

#if defined(__clang__)
#define clang_optnone __attribute__((optnone))
#else
#define clang_optnone
#endif
#pragma GCC push_options
#pragma GCC optimize("O0")

using namespace mongo::optimizer;
using namespace mongo::optimizer::properties;
using namespace mongo::optimizer::unit_test_abt_literals;

int clang_optnone main(int argc, char** argv) {
    ABT testABT = NodeBuilder{}
                      .root("root")
                      .filter(_evalf(_composem(_get("a", _cmp("Eq", "1"_cint64)),
                                               _get("b", _cmp("Eq", "1"_cint64))),
                                     "root"_var))
                      .finish(_scan("root", "coll"));

    auto prefixId = PrefixId::createForTests();
    auto phaseManager = makePhaseManager(
        {OptPhase::MemoSubstitutionPhase, OptPhase::MemoExplorationPhase},
        prefixId,
        {{{"coll",
           createScanDef(
               {},
               {{"index1",
                 IndexDefinition{// collation
                                 {{makeIndexPath(FieldPathType{"a"}, false /*isMultiKey*/),
                                   CollationOp::Ascending}},
                                 false /*isMultiKey*/}}})}}},
        boost::none /*costModel*/,
        {true /*debugMode*/, 2 /*debugLevel*/, DebugInfo::kIterationLimitForTests});

    ABT optimized = testABT;
    phaseManager.optimize(optimized);

    // Verify output as a sanity check, the real test is in the gdb test file.
    ASSERT_EXPLAIN_V2_AUTO(
        "Root [{root}]\n"
        "RIDIntersect [root]\n"
        "|   Sargable [Seek]\n"
        "|   |   |   requirements: \n"
        "|   |   |       {{{root, 'PathGet [b] PathIdentity []', {{{=Const [1]}}}}}}\n"
        "|   |   scanParams: \n"
        "|   |       {'b': evalTemp_4}\n"
        "|   |           residualReqs: \n"
        "|   |               {{{evalTemp_4, 'PathIdentity []', {{{=Const [1]}}}, entryIndex: 0}}}\n"
        "|   Scan [coll, {root}]\n"
        "Sargable [Index]\n"
        "|   |   requirements: \n"
        "|   |       {{{root, 'PathGet [a] PathIdentity []', {{{=Const [1]}}}}}}\n"
        "|   candidateIndexes: \n"
        "|       candidateId: 1, index1, {}, {SimpleEquality}, {{{=Const [1]}}}\n"
        "Scan [coll, {root}]\n",
        optimized);

    // Retrieve the SargableNodes, which allows for printing candidate indexes as well as
    // requirements.
    [[maybe_unused]] const SargableNode& indexSargable = *optimized.cast<RootNode>()
                                                              ->getChild()
                                                              .cast<RIDIntersectNode>()
                                                              ->getLeftChild()
                                                              .cast<SargableNode>();
    [[maybe_unused]] const SargableNode& residualSargable = *optimized.cast<RootNode>()
                                                                 ->getChild()
                                                                 .cast<RIDIntersectNode>()
                                                                 ->getRightChild()
                                                                 .cast<SargableNode>();

    auto testInterval = _disj(_interval(_incl("1"_cint32), _incl("3"_cint32)),
                              _interval(_incl("4"_cint32), _incl("5"_cint32)));

    [[maybe_unused]] mongo::optimizer::FieldProjectionMap emptyProjectionMap;
    [[maybe_unused]] mongo::optimizer::FieldProjectionMap testProjectionMap;
    testProjectionMap._rootProjection = "test";
    testProjectionMap._fieldProjections.emplace("a", "b");
    testProjectionMap._fieldProjections.emplace("c", "d");

    mongo::breakpoint();

    return 0;
}

#pragma GCC pop_options
