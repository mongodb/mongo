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

const char* TextMatchStage::kStageType = "TEXT_MATCH";

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
