// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/text_match.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/storage/snapshot.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

using std::unique_ptr;


TextMatchStage::TextMatchStage(ExpressionContext* expCtx,
                               unique_ptr<PlanStage> child,
                               const TextMatchParams& params,
                               WorkingSet* ws)
    : PlanStage(kStageType, expCtx), _ftsMatcher(params.query, params.spec), _ws(ws) {
    _specificStats.indexPrefix = params.indexPrefix;
    _specificStats.indexName = params.index->indexName();
    _specificStats.parsedTextQuery = params.query.toBSON();
    _specificStats.textIndexVersion = params.index->infoObj()["textIndexVersion"].numberInt();
    _children.emplace_back(std::move(child));
}

TextMatchStage::~TextMatchStage() {}

bool TextMatchStage::isEOF() const {
    return child()->isEOF();
}

std::unique_ptr<PlanStageStats> TextMatchStage::getStats() {
    _commonStats.isEOF = isEOF();

    unique_ptr<PlanStageStats> ret =
        std::make_unique<PlanStageStats>(_commonStats, STAGE_TEXT_MATCH);
    ret->specific = std::make_unique<TextMatchStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());

    return ret;
}

const SpecificStats* TextMatchStage::getSpecificStats() const {
    return &_specificStats;
}

PlanStage::StageState TextMatchStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    // Retrieve fetched document from child.
    StageState stageState = child()->work(out);

    if (stageState == PlanStage::ADVANCED) {
        // We just successfully retrieved a fetched doc.
        WorkingSetMember* wsm = _ws->get(*out);

        // Filter for phrases and negated terms.
        if (!_ftsMatcher.matches(wsm->doc.value().toBson())) {
            _ws->free(*out);
            *out = WorkingSet::INVALID_ID;
            ++_specificStats.docsRejected;
            stageState = PlanStage::NEED_TIME;
        }
    }

    return stageState;
}

}  // namespace mongo
