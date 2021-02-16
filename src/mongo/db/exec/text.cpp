/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/text.h"

#include <memory>
#include <vector>

#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/text_match.h"
#include "mongo/db/exec/text_or.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/internal_plans.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;


using fts::FTSIndexFormat;
using fts::MAX_WEIGHT;

const char* TextStage::kStageType = "TEXT";

TextStage::TextStage(ExpressionContext* expCtx,
                     const CollectionPtr& collection,
                     const TextStageParams& params,
                     WorkingSet* ws,
                     const MatchExpression* filter)
    : PlanStage(kStageType, expCtx), _params(params) {
    _children.emplace_back(
        buildTextTree(expCtx->opCtx, collection, ws, filter, params.wantTextScore));
    _specificStats.indexPrefix = _params.indexPrefix;
    _specificStats.indexName = _params.index->indexName();
    _specificStats.parsedTextQuery = _params.query.toBSON();
    _specificStats.textIndexVersion = _params.index->infoObj()["textIndexVersion"].numberInt();
}

bool TextStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState TextStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    return child()->work(out);
}

unique_ptr<PlanStageStats> TextStage::getStats() {
    _commonStats.isEOF = isEOF();

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_TEXT);
    ret->specific = std::make_unique<TextStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* TextStage::getSpecificStats() const {
    return &_specificStats;
}

unique_ptr<PlanStage> TextStage::buildTextTree(OperationContext* opCtx,
                                               const CollectionPtr& collection,
                                               WorkingSet* ws,
                                               const MatchExpression* filter,
                                               bool wantTextScore) const {
    // If the query requires the "textScore" field or involves multiple search terms, a TEXT_OR or
    // OR stage is needed. Otherwise, we can use a single index scan directly.
    const bool needOrStage = wantTextScore || _params.query.getTermsForBounds().size() > 1;
    const MatchExpression* emptyFilter = nullptr;
    // Get all the index scans for each term in our query.
    std::vector<std::unique_ptr<PlanStage>> indexScanList;
    for (const auto& term : _params.query.getTermsForBounds()) {
        IndexScanParams ixparams(opCtx, _params.index);
        ixparams.bounds.startKey = FTSIndexFormat::getIndexKey(
            MAX_WEIGHT, term, _params.indexPrefix, _params.spec.getTextIndexVersion());
        ixparams.bounds.endKey = FTSIndexFormat::getIndexKey(
            0, term, _params.indexPrefix, _params.spec.getTextIndexVersion());
        ixparams.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
        ixparams.bounds.isSimpleRange = true;
        ixparams.direction = -1;
        ixparams.shouldDedup = _params.index->getEntry()->isMultikey();

        // If we will be adding a TEXT_OR or OR stage, then it is responsible for applying the
        // filter. Otherwise, the index scan applies the filter.
        indexScanList.push_back(std::make_unique<IndexScan>(
            expCtx(), collection, ixparams, ws, needOrStage ? emptyFilter : filter));
    }

    // Build the union of the index scans as a TEXT_OR or an OR stage, depending on whether the
    // projection requires the "textScore" $meta field.
    std::unique_ptr<PlanStage> textMatchStage;
    if (wantTextScore) {
        // We use a TEXT_OR stage to get the union of the results from the index scans and then
        // compute their text scores. This is a blocking operation.
        auto textScorer =
            std::make_unique<TextOrStage>(expCtx(), _params.spec, ws, filter, collection);

        textScorer->addChildren(std::move(indexScanList));

        textMatchStage = std::make_unique<TextMatchStage>(
            expCtx(), std::move(textScorer), _params.query, _params.spec, ws);
    } else {
        // Because we don't need the text score, we can use a non-blocking OR stage to get the union
        // of the index scans or use the index scan directly if there is only one.
        std::unique_ptr<mongo::PlanStage> textSearcher;
        if (indexScanList.size() == 1) {
            tassert(5397400,
                    "If there is only one index scan and we do not need textScore, needOrStage "
                    "should be false",
                    !needOrStage);
            textSearcher = std::move(indexScanList[0]);
        } else {
            auto orTextSearcher = std::make_unique<OrStage>(expCtx(), ws, true, filter);
            orTextSearcher->addChildren(std::move(indexScanList));
            textSearcher = std::move(orTextSearcher);
        }

        // Unlike the TEXT_OR stage, the OR stage does not fetch the documents that it outputs. We
        // add our own FETCH stage to satisfy the requirement of the TEXT_MATCH stage that its
        // WorkingSetMember inputs have fetched data.
        auto fetchStage = std::make_unique<FetchStage>(
            expCtx(), ws, std::move(textSearcher), emptyFilter, collection);

        textMatchStage = std::make_unique<TextMatchStage>(
            expCtx(), std::move(fetchStage), _params.query, _params.spec, ws);
    }

    return textMatchStage;
}

}  // namespace mongo
