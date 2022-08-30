/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <cstddef>

#include "mongo/db/query/ce/ce_test_utils.h"

#include "mongo/db/query/optimizer/cascades/ce_heuristic.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/cascades/logical_props_derivation.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

using namespace optimizer;
using namespace cascades;

CETester::CETester(std::string collName,
                   double collCard,
                   const optimizer::OptPhaseManager::PhaseSet& optPhases)
    : _collName(std::move(collName)), _collCard(collCard), _optPhases(optPhases) {}

optimizer::CEType CETester::getCE(const std::string& query) const {
    if constexpr (kCETestLogOnly) {
        std::cout << "Query: " << query << "\n";
    }

    // Construct ABT from pipeline and optimize.
    ABT abt = translatePipeline("[{$match: " + query + "}]", _collName);

    // Get cardinality estimate.
    return getCE(abt);
}

optimizer::CEType CETester::getCE(ABT& abt) const {
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainV2(abt) << std::endl;
    }

    // Optimize ABT.
    OptPhaseManager phaseManager = getPhaseManager();
    ASSERT_TRUE(phaseManager.optimize(abt));

    const auto& memo = phaseManager.getMemo();
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainMemo(memo) << std::endl;
    }

    auto cht = getCETransport();

    // If we are running no optimization phases, we are ensuring that we get the correct estimate on
    // the original ABT (usually testing the CE for FilterNodes). The memo won't have any groups for
    // us to estimate directly yet.
    if (_optPhases.empty()) {
        auto card = cht->deriveCE(memo, {}, abt.ref());
        return card;
    }

    CEType outCard = kInvalidCardinality;
    for (size_t i = 0; i < memo.getGroupCount(); i++) {
        const auto& group = memo.getGroup(i);

        // If the 'optPhases' either ends with the MemoSubstitutionPhase or the
        // MemoImplementationPhase, we should have exactly one logical node per group.
        ASSERT_EQUALS(group._logicalNodes.size(), 1);
        const auto& node = group._logicalNodes.at(0);

        // This gets the cardinality estimate actually produced during optimization.
        auto memoCE =
            properties::getPropertyConst<properties::CardinalityEstimate>(group._logicalProperties)
                .getEstimate();

        // Conversely, here we call deriveCE() on the ABT produced by the optimization phases, which
        // has all its delegators dereferenced.
        auto card = cht->deriveCE(memo, group._logicalProperties, node);

        if constexpr (!kCETestLogOnly) {
            // Ensure that the CE stored for the logical nodes of each group is what we would expect
            // when estimating that node directly. Note that this check will fail if we are testing
            // histogram estimation and only using the MemoSubstitutionPhase because the memo always
            // uses heuristic estimation in this case.
            ASSERT_APPROX_EQUAL(card, memoCE, kMaxCEError);
        }

        if (node.is<optimizer::RootNode>()) {
            // We want to return the cardinality for the entire ABT.
            outCard = memoCE;
        }
    }

    ASSERT_NOT_EQUALS(outCard, kInvalidCardinality);

    return outCard;
}

optimizer::OptPhaseManager CETester::getPhaseManager() const {
    ScanDefinition sd({}, {}, {DistributionType::Centralized}, true, _collCard);
    Metadata metadata({{_collName, sd}});
    return {_optPhases,
            _prefixId,
            true /*requireRID*/,
            metadata,
            getCETransport(),
            std::make_unique<DefaultCosting>(),
            DebugInfo::kDefaultForTests};
}

}  // namespace mongo::ce
