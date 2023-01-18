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

#include "mongo/db/query/ce/test_utils.h"

#include "mongo/db/pipeline/abt/utils.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata_factory.h"
#include "mongo/db/query/optimizer/opt_phase_manager.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/unit_test_pipeline_utils.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::optimizer::ce {
namespace value = sbe::value;

CETester::CETester(std::string collName,
                   CEType collCard,
                   const OptPhaseManager::PhaseSet& optPhases)
    : _optPhases(optPhases),
      _prefixId(PrefixId::createForTests()),
      _hints(),
      _metadata({}),
      _collName(collName) {
    addCollection(collName, collCard);
}

CEType CETester::getMatchCE(const std::string& queryPredicate,
                            std::function<bool(const ABT&)> nodePredicate) const {
    return getCE("[{$match: " + queryPredicate + "}]", nodePredicate);
}

CEType CETester::getCE(const std::string& pipeline,
                       std::function<bool(const ABT&)> nodePredicate) const {
    if constexpr (kCETestLogOnly) {
        std::cout << "\n\nQuery: " << pipeline << "\n";
    }

    // Construct ABT from pipeline and optimize.
    ABT abt =
        translatePipeline(_metadata, pipeline, _prefixId.getNextId("scan"), _collName, _prefixId);

    // Get cardinality estimate.
    return getCE(abt, nodePredicate);
}

CEType CETester::getCE(ABT& abt, std::function<bool(const ABT&)> nodePredicate) const {
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainV2(abt) << std::endl;
    }

    OptPhaseManager phaseManager{_optPhases,
                                 _prefixId,
                                 false /*requireRID*/,
                                 _metadata,
                                 getEstimator(),
                                 makeHeuristicCE(),
                                 makeCostEstimator(),
                                 defaultConvertPathToInterval,
                                 ConstEval::constFold,
                                 DebugInfo::kDefaultForTests,
                                 _hints};
    optimize(phaseManager, abt);

    const auto& memo = phaseManager.getMemo();
    if constexpr (kCETestLogOnly) {
        std::cout << ExplainGenerator::explainMemo(memo) << std::endl;
    }

    auto cht = getEstimator(true /* forValidation */);

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
        // We only want to return the cardinality for the memo group matching the 'nodePredicate'.
        if (const auto& node = memo.getLogicalNodes(groupId).front(); nodePredicate(node)) {
            const auto& logicalProps = memo.getLogicalProps(groupId);
            outCard = properties::getPropertyConst<properties::CardinalityEstimate>(logicalProps)
                          .getEstimate();
        }
    }

    ASSERT_NOT_EQUALS(outCard, kInvalidCardinality);

    if constexpr (kCETestLogOnly) {
        std::cout << "CE: " << outCard << std::endl;
    }

    return outCard;
}

void CETester::optimize(OptPhaseManager& phaseManager, ABT& abt) const {
    phaseManager.optimize(abt);
}

ScanDefinition& CETester::getCollScanDefinition() {
    auto it = _metadata._scanDefs.find(_collName);
    invariant(it != _metadata._scanDefs.end());
    return it->second;
}


void CETester::setCollCard(CEType card) {
    auto& scanDef = getCollScanDefinition();
    addCollection(_collName, card, scanDef.getIndexDefs());
}

void CETester::setIndexes(opt::unordered_map<std::string, IndexDefinition> indexes) {
    auto& scanDef = getCollScanDefinition();
    addCollection(_collName, scanDef.getCE(), indexes);
}

void CETester::addCollection(std::string collName,
                             CEType numRecords,
                             opt::unordered_map<std::string, IndexDefinition> indexes) {
    _metadata._scanDefs.insert_or_assign(collName,
                                         createScanDef({},
                                                       indexes,
                                                       ConstEval::constFold,
                                                       {DistributionType::Centralized},
                                                       true /*exists*/,
                                                       numRecords));
}

stats::ScalarHistogram createHistogram(const std::vector<BucketData>& data) {
    value::Array bounds;
    std::vector<stats::Bucket> buckets;

    double cumulativeFreq = 0.0;
    double cumulativeNDV = 0.0;

    // Create a value vector & sort it.
    std::vector<stats::SBEValue> values;
    for (size_t i = 0; i < data.size(); i++) {
        const auto& item = data[i];
        const auto [tag, val] = stage_builder::makeValue(item._v);
        values.emplace_back(tag, val);
    }
    sortValueVector(values);

    for (size_t i = 0; i < values.size(); i++) {
        const auto& val = values[i];
        const auto [tag, value] = copyValue(val.getTag(), val.getValue());
        bounds.push_back(tag, value);

        const auto& item = data[i];
        cumulativeFreq += item._equalFreq + item._rangeFreq;
        cumulativeNDV += item._ndv + 1.0;
        buckets.emplace_back(
            item._equalFreq, item._rangeFreq, cumulativeFreq, item._ndv, cumulativeNDV);
    }
    return stats::ScalarHistogram::make(std::move(bounds), std::move(buckets));
}

double estimateIntValCard(const stats::ScalarHistogram& hist,
                          const int v,
                          const EstimationType type) {
    const auto [tag, val] =
        std::make_pair(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(v));
    return estimate(hist, tag, val, type).card;
};

}  // namespace mongo::optimizer::ce
