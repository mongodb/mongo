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

#include "mongo/db/exec/text_or.h"

#include <map>
#include <memory>
#include <vector>

#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/record_id.h"
#include "mongo/util/assert_util.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

using fts::FTSSpec;

const char* TextOrStage::kStageType = "TEXT_OR";

TextOrStage::TextOrStage(ExpressionContext* expCtx,
                         size_t keyPrefixSize,
                         WorkingSet* ws,
                         const MatchExpression* filter,
                         const CollectionPtr& collection)
    : RequiresCollectionStage(kStageType, expCtx, collection),
      _keyPrefixSize(keyPrefixSize),
      _ws(ws),
      _scoreIterator(_scores.end()),
      _filter(filter),
      _idRetrying(WorkingSet::INVALID_ID) {}

void TextOrStage::addChild(unique_ptr<PlanStage> child) {
    _children.push_back(std::move(child));
}

void TextOrStage::addChildren(Children childrenToAdd) {
    _children.insert(_children.end(),
                     std::make_move_iterator(childrenToAdd.begin()),
                     std::make_move_iterator(childrenToAdd.end()));
}

bool TextOrStage::isEOF() {
    return _internalState == State::kDone;
}

void TextOrStage::doSaveStateRequiresCollection() {
    if (_recordCursor) {
        _recordCursor->saveUnpositioned();
    }
}

void TextOrStage::doRestoreStateRequiresCollection() {
    if (_recordCursor) {
        invariant(_recordCursor->restore());
    }
}

void TextOrStage::doDetachFromOperationContext() {
    if (_recordCursor)
        _recordCursor->detachFromOperationContext();
}

void TextOrStage::doReattachToOperationContext() {
    if (_recordCursor)
        _recordCursor->reattachToOperationContext(opCtx());
}

std::unique_ptr<PlanStageStats> TextOrStage::getStats() {
    _commonStats.isEOF = isEOF();

    if (_filter) {
        BSONObjBuilder bob;
        _filter->serialize(&bob);
        _commonStats.filter = bob.obj();
    }

    unique_ptr<PlanStageStats> ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_TEXT_OR);
    ret->specific = std::make_unique<TextOrStats>(_specificStats);

    for (auto&& child : _children) {
        ret->children.emplace_back(child->getStats());
    }

    return ret;
}

const SpecificStats* TextOrStage::getSpecificStats() const {
    return &_specificStats;
}

PlanStage::StageState TextOrStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    PlanStage::StageState stageState = PlanStage::IS_EOF;

    switch (_internalState) {
        case State::kInit:
            stageState = initStage(out);
            break;
        case State::kReadingTerms:
            stageState = readFromChildren(out);
            break;
        case State::kReturningResults:
            stageState = returnResults(out);
            break;
        case State::kDone:
            // Should have been handled above.
            MONGO_UNREACHABLE;
            break;
    }

    return stageState;
}

PlanStage::StageState TextOrStage::initStage(WorkingSetID* out) {
    *out = WorkingSet::INVALID_ID;

    return handlePlanStageYield(expCtx(),
                                "TextOrStage initStage",
                                collection()->ns().ns(),
                                [&] {
                                    _recordCursor = collection()->getCursor(opCtx());
                                    _internalState = State::kReadingTerms;
                                    return PlanStage::NEED_TIME;
                                },
                                [&] {
                                    // yieldHandler
                                    invariant(_internalState == State::kInit);
                                    _recordCursor.reset();
                                });
}

PlanStage::StageState TextOrStage::readFromChildren(WorkingSetID* out) {
    // Check to see if there were any children added in the first place.
    if (_children.size() == 0) {
        _internalState = State::kDone;
        return PlanStage::IS_EOF;
    }
    invariant(_currentChild < _children.size());

    // Either retry the last WSM we worked on or get a new one from our current child.
    WorkingSetID id;
    StageState childState;
    if (_idRetrying == WorkingSet::INVALID_ID) {
        childState = _children[_currentChild]->work(&id);
    } else {
        childState = ADVANCED;
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    if (PlanStage::ADVANCED == childState) {
        return addTerm(id, out);
    } else if (PlanStage::IS_EOF == childState) {
        // Done with this child.
        ++_currentChild;

        if (_currentChild < _children.size()) {
            // We have another child to read from.
            return PlanStage::NEED_TIME;
        }

        // If we're here we are done reading results.  Move to the next state.
        _scoreIterator = _scores.begin();
        _internalState = State::kReturningResults;

        return PlanStage::NEED_TIME;
    } else {
        // Propagate WSID from below.
        *out = id;
        return childState;
    }
}

PlanStage::StageState TextOrStage::returnResults(WorkingSetID* out) {
    if (_scoreIterator == _scores.end()) {
        _internalState = State::kDone;
        return PlanStage::IS_EOF;
    }

    // Retrieve the record that contains the text score.
    TextRecordData textRecordData = _scoreIterator->second;
    ++_scoreIterator;

    // Ignore non-matched documents.
    if (textRecordData.score < 0) {
        invariant(textRecordData.wsid == WorkingSet::INVALID_ID);
        return PlanStage::NEED_TIME;
    }

    WorkingSetMember* wsm = _ws->get(textRecordData.wsid);

    // Populate the working set member with the text score metadata and return it.
    wsm->metadata().setTextScore(textRecordData.score);
    *out = textRecordData.wsid;
    return PlanStage::ADVANCED;
}

PlanStage::StageState TextOrStage::addTerm(WorkingSetID wsid, WorkingSetID* out) {
    WorkingSetMember* wsm = _ws->get(wsid);
    invariant(wsm->getState() == WorkingSetMember::RID_AND_IDX);
    invariant(1 == wsm->keyData.size());
    const IndexKeyDatum newKeyData = wsm->keyData.back();  // copy to keep it around.
    TextRecordData* textRecordData = &_scores[wsm->recordId];

    if (textRecordData->score < 0) {
        // We have already rejected this document for not matching the filter.
        invariant(WorkingSet::INVALID_ID == textRecordData->wsid);
        _ws->free(wsid);
        return NEED_TIME;
    }

    if (WorkingSet::INVALID_ID == textRecordData->wsid) {
        // We haven't seen this RecordId before.
        invariant(textRecordData->score == 0);

        if (!Filter::passes(newKeyData.keyData, newKeyData.indexKeyPattern, _filter)) {
            _ws->free(wsid);
            textRecordData->score = -1;
            return NEED_TIME;
        }

        // Our parent expects RID_AND_OBJ members, so we fetch the document here if we haven't
        // already.
        const auto ret =
            handlePlanStageYield(expCtx(),
                                 "TextOrStage addTerm",
                                 collection()->ns().ns(),
                                 [&] {
                                     if (!WorkingSetCommon::fetch(opCtx(),
                                                                  _ws,
                                                                  wsid,
                                                                  _recordCursor.get(),
                                                                  collection(),
                                                                  collection()->ns())) {
                                         _ws->free(wsid);
                                         textRecordData->score = -1;
                                         return NEED_TIME;
                                     }
                                     ++_specificStats.fetches;
                                     return PlanStage::ADVANCED;
                                 },
                                 [&] {
                                     // yieldHandler
                                     wsm->makeObjOwnedIfNeeded();
                                     _idRetrying = wsid;
                                     *out = WorkingSet::INVALID_ID;
                                 });

        if (ret != PlanStage::ADVANCED) {
            return ret;
        }

        textRecordData->wsid = wsid;

        // Ensure that the BSONObj underlying the WorkingSetMember is owned in case we yield.
        wsm->makeObjOwnedIfNeeded();
    } else {
        // We already have a working set member for this RecordId. Free the new WSM and retrieve the
        // old one. Note that since we don't keep all index keys, we could get a score that doesn't
        // match the document, but this has always been a problem.
        // TODO something to improve the situation.
        invariant(wsid != textRecordData->wsid);
        _ws->free(wsid);
        wsm = _ws->get(textRecordData->wsid);
    }

    // Locate score within possibly compound key: {prefix,term,score,suffix}.
    BSONObjIterator keyIt(newKeyData.keyData);
    for (unsigned i = 0; i < _keyPrefixSize; i++) {
        keyIt.next();
    }

    keyIt.next();  // Skip past 'term'.

    BSONElement scoreElement = keyIt.next();
    double documentTermScore = scoreElement.number();

    // Aggregate relevance score, term keys.
    textRecordData->score += documentTermScore;
    return NEED_TIME;
}

}  // namespace mongo
