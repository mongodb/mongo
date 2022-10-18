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

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/cascades/cost_derivation.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::ce {

using namespace optimizer;
using namespace cascades;

CETester::CETester(std::string collName,
                   double collCard,
                   const optimizer::OptPhaseManager::PhaseSet& optPhases)
    : _optPhases(optPhases), _hints(), _metadata({}), _collName(collName) {
    addCollection(collName, collCard);
}

template <class T>
optimizer::CEType CETester::getMatchCE(const std::string& predicate) const {
    return getCE<T>("[{$match: " + predicate + "}]");
}

template <class T>
optimizer::CEType CETester::getCE(const std::string& pipeline) const {
    if constexpr (kCETestLogOnly) {
        std::cout << "\n\nQuery: " << pipeline << "\n";
    }

    // Construct ABT from pipeline and optimize.
    ABT abt = translatePipeline(pipeline, _collName);

    // Get cardinality estimate.
    return getCE<T>(abt);
}

template optimizer::CEType CETester::getCE<optimizer::RootNode>(const std::string& query) const;
template optimizer::CEType CETester::getCE<optimizer::SargableNode>(const std::string& query) const;


template <class T>
optimizer::CEType CETester::getCE(ABT& abt) const {
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainV2(abt) << std::endl;
    }

    OptPhaseManager phaseManager{_optPhases,
                                 _prefixId,
                                 false /*requireRID*/,
                                 _metadata,
                                 getCETransport(),
                                 std::make_unique<DefaultCosting>(),
                                 defaultConvertPathToInterval,
                                 ConstEval::constFold,
                                 DebugInfo::kDefaultForTests,
                                 _hints};
    phaseManager.optimize(abt);

    const auto& memo = phaseManager.getMemo();
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainMemo(memo) << std::endl;
    }

    auto cht = getCETransport();

    // If we are running no optimization phases, we are ensuring that we get the correct estimate on
    // the original ABT (usually testing the CE for FilterNodes). The memo won't have any groups for
    // us to estimate directly yet.
    if (_optPhases.empty()) {
        auto card = cht->deriveCE(_metadata, memo, {}, abt.ref());

        if constexpr (kCETestLogOnly) {
            std::cout << "CE: " << card << std::endl;
        }

        return card;
    }

    CEType outCard = kInvalidCardinality;
    for (size_t groupId = 0; groupId < memo.getGroupCount(); groupId++) {
        // Note that we always verify CE for MemoLogicalDelegatorNodes when calling getCE().

        // If the 'optPhases' either ends with the MemoSubstitutionPhase or the
        // MemoImplementationPhase, we should have exactly one logical node per group. However, if
        // we have indexes, or a $group, we may have multiple logical nodes. In this case, we still
        // want to pick the first node.
        const auto& node = memo.getLogicalNodes(groupId).front();

        // This gets the cardinality estimate actually produced during optimization.
        const auto& logicalProps = memo.getLogicalProps(groupId);
        auto memoCE = properties::getPropertyConst<properties::CardinalityEstimate>(logicalProps)
                          .getEstimate();

        // Conversely, here we call deriveCE() on the ABT produced by the optimization phases, which
        // has all its delegators dereferenced.
        auto card = cht->deriveCE(_metadata, memo, logicalProps, node.ref());

        if constexpr (!kCETestLogOnly) {
            // Ensure that the CE stored for the logical nodes of each group is what we would expect
            // when estimating that node directly. Note that this check will fail if we are testing
            // histogram estimation and only using the MemoSubstitutionPhase because the memo always
            // uses heuristic estimation in this case.
            ASSERT_APPROX_EQUAL(card, memoCE, kMaxCEError);
        } else {
            if (std::abs(memoCE - card) > kMaxCEError) {
                std::cout << "ERROR: CE Group(" << groupId << ") " << card << " vs. " << memoCE
                          << std::endl;
                std::cout << ExplainGenerator::explainV2(node) << std::endl;
            }
        }

        if (node.is<T>()) {
            // We want to return the cardinality for the entire ABT.
            outCard = memoCE;
        }
    }

    ASSERT_NOT_EQUALS(outCard, kInvalidCardinality);

    if constexpr (kCETestLogOnly) {
        std::cout << "CE: " << outCard << std::endl;
    }

    return outCard;
}

template optimizer::CEType CETester::getCE<optimizer::RootNode>(ABT& abt) const;
template optimizer::CEType CETester::getCE<optimizer::SargableNode>(ABT& abt) const;
template optimizer::CEType CETester::getMatchCE<optimizer::RootNode>(
    const std::string& matchPredicate) const;
template optimizer::CEType CETester::getMatchCE<optimizer::SargableNode>(
    const std::string& matchPredicate) const;

ScanDefinition& CETester::getCollScanDefinition() {
    auto it = _metadata._scanDefs.find(_collName);
    invariant(it != _metadata._scanDefs.end());
    return it->second;
}


void CETester::setCollCard(double card) {
    auto& scanDef = getCollScanDefinition();
    addCollection(_collName, card, scanDef.getIndexDefs());
}

void CETester::setIndexes(opt::unordered_map<std::string, IndexDefinition> indexes) {
    auto& scanDef = getCollScanDefinition();
    addCollection(_collName, scanDef.getCE(), indexes);
}

void CETester::addCollection(std::string collName,
                             double numRecords,
                             opt::unordered_map<std::string, IndexDefinition> indexes) {
    _metadata._scanDefs.insert_or_assign(collName,
                                         createScanDef({},
                                                       indexes,
                                                       ConstEval::constFold,
                                                       {DistributionType::Centralized},
                                                       true /*exists*/,
                                                       numRecords));
}

ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    value::Array bounds;
    std::vector<Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    for (size_t i = 0; i < data.size(); i++) {
        const auto& item = data.at(i);
        const auto [tag, val] = stage_builder::makeValue(item._v);
        bounds.push_back(tag, val);

        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }

    return {std::move(bounds), std::move(buckets)};
}

double estimateIntValCard(const ScalarHistogram& hist, const int v, const EstimationType type) {
    const auto [tag, val] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
    return estimate(hist, tag, val, type).card;
};

}  // namespace mongo::ce
