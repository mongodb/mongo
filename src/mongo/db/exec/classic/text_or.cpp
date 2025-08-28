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

#include "mongo/db/exec/classic/text_or.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/filter.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/classic/working_set_common.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/query/util/spill_util.h"
#include "mongo/db/record_id.h"
#include "mongo/db/sorter/sorter_template_defs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/assert_util.h"

#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>

namespace mongo {

namespace {

int64_t getMemoryLimit() {
    return feature_flags::gFeatureFlagExtendedAutoSpilling.isEnabled()
        ? loadMemoryLimit(StageMemoryLimit::TextOrStageMaxMemoryBytes)
        : std::numeric_limits<int64_t>::max();
}

}  // namespace

const char* TextOrStage::kStageType = "TEXT_OR";

TextOrStage::TextOrStage(ExpressionContext* expCtx,
                         size_t keyPrefixSize,
                         WorkingSet* ws,
                         const MatchExpression* filter,
                         VariantCollectionPtrOrAcquisition collection)
    : RequiresCollectionStage(kStageType, expCtx, collection),
      _keyPrefixSize(keyPrefixSize),
      _ws(ws),
      _memoryTracker(OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(
          *expCtx, getMemoryLimit())),
      _filter(filter),
      _idRetrying(WorkingSet::INVALID_ID) {}

void TextOrStage::addChild(std::unique_ptr<PlanStage> child) {
    _children.push_back(std::move(child));
}

void TextOrStage::addChildren(Children childrenToAdd) {
    _children.insert(_children.end(),
                     std::make_move_iterator(childrenToAdd.begin()),
                     std::make_move_iterator(childrenToAdd.end()));
}

bool TextOrStage::isEOF() const {
    return _internalState == State::kDone;
}

void TextOrStage::doSaveStateRequiresCollection() {
    if (_recordCursor) {
        _recordCursor->saveUnpositioned();
    }
}

void TextOrStage::doRestoreStateRequiresCollection() {
    if (_recordCursor) {
        invariant(_recordCursor->restore(*shard_role_details::getRecoveryUnit(opCtx())));
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
        _commonStats.filter = _filter->serialize();
    }

    auto ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_TEXT_OR);
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

    return handlePlanStageYield(
        expCtx(),
        "TextOrStage initStage",
        [&] {
            _recordCursor = collectionPtr()->getCursor(opCtx());
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
        if (_sorter) {
            if (!_scores.empty()) {
                doForceSpill();
            }
            _sorterIterator = _sorter->done();
        }
        _internalState = State::kReturningResults;

        return PlanStage::NEED_TIME;
    } else {
        // Propagate WSID from below.
        *out = id;
        return childState;
    }
}

PlanStage::StageState TextOrStage::returnResults(WorkingSetID* out) {
    _specificStats.peakTrackedMemBytes = _memoryTracker.peakTrackedMemoryBytes();

    if (_sorter) {
        return returnResultsSpilled(out);
    } else {
        return returnResultsInMemory(out);
    }
}

PlanStage::StageState TextOrStage::returnResultsInMemory(WorkingSetID* out) {
    if (_scores.empty()) {
        _internalState = State::kDone;
        return PlanStage::IS_EOF;
    }

    // Retrieve the record that contains the text score.
    auto it = _scores.begin();
    TextRecordData textRecordData = it->second;
    _memoryTracker.add(-1 * (it->first.memUsage() + sizeof(TextRecordData)));
    _scores.erase(it);

    // Ignore non-matched documents.
    if (textRecordData.score == kRejectedDocumentScore) {
        invariant(textRecordData.wsid == WorkingSet::INVALID_ID);
        return PlanStage::NEED_TIME;
    }

    WorkingSetMember* wsm = _ws->get(textRecordData.wsid);

    // Populate the working set member with the text score metadata and return it.
    wsm->metadata().setTextScore(textRecordData.score);
    *out = textRecordData.wsid;
    return PlanStage::ADVANCED;
}

PlanStage::StageState TextOrStage::returnResultsSpilled(WorkingSetID* out) {
    if (!_sorterIterator->more()) {
        _internalState = State::kDone;
        return PlanStage::IS_EOF;
    }

    auto [recordId, textRecordData] = _sorterIterator->next();
    double score = textRecordData.score;
    bool skip = score == kRejectedDocumentScore;

    while (_sorterIterator->more() && _sorterIterator->current() == recordId) {
        double currentScore = _sorterIterator->next().second.score;
        score += currentScore;
        skip |= currentScore == kRejectedDocumentScore;
    }

    if (skip) {
        return PlanStage::NEED_TIME;
    }

    WorkingSetMember wsm = textRecordData.document.extract();
    wsm.metadata().setTextScore(score);
    *out = _ws->emplace(std::move(wsm));
    return PlanStage::ADVANCED;
}

PlanStage::StageState TextOrStage::addTerm(WorkingSetID wsid, WorkingSetID* out) {
    WorkingSetMember* wsm = _ws->get(wsid);
    invariant(wsm->getState() == WorkingSetMember::RID_AND_IDX);
    invariant(1 == wsm->keyData.size());
    const IndexKeyDatum newKeyData = wsm->keyData.back();  // copy to keep it around.

    auto [it, inserted] = _scores.try_emplace(wsm->recordId, TextRecordData{});
    if (inserted) {
        _memoryTracker.add(it->first.memUsage() + sizeof(TextRecordData));
    }

    TextRecordData* textRecordData = &it->second;

    if (textRecordData->score == kRejectedDocumentScore) {
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
            textRecordData->score = kRejectedDocumentScore;
            return NEED_TIME;
        }

        // Our parent expects RID_AND_OBJ members, so we fetch the document here if we haven't
        // already.
        const auto ret = handlePlanStageYield(
            expCtx(),
            "TextOrStage addTerm",
            [&] {
                if (!WorkingSetCommon::fetch(opCtx(),
                                             _ws,
                                             wsid,
                                             _recordCursor.get(),
                                             collectionPtr(),
                                             collectionPtr()->ns())) {
                    _ws->free(wsid);
                    textRecordData->score = kRejectedDocumentScore;
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
        _memoryTracker.add(wsm->getMemUsage());
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

    if (!_memoryTracker.withinMemoryLimit()) {
        doForceSpill();
    }

    return NEED_TIME;
}

void TextOrStage::doForceSpill() {
    if (_scores.empty()) {
        return;
    }

    if (!_sorter) {
        initSorter();
    }

    uassertStatusOK(ensureSufficientDiskSpaceForSpilling(
        expCtx()->getTempDir(), internalQuerySpillingMinAvailableDiskSpaceBytes.loadRelaxed()));

    size_t recordsToSpill = _scores.size();
    auto prevMemUsage = _sorter->stats().memUsage();
    for (auto it = _scores.begin(); it != _scores.end();) {
        const auto& [recordId, textRecordData] = (*it);
        SortableWorkingSetMember wsm = textRecordData.wsid != WorkingSet::INVALID_ID
            ? _ws->extract(textRecordData.wsid)
            : WorkingSetMember{};
        TextRecordDataForSorter dataForSorter = {
            std::move(wsm),
            textRecordData.score,
        };
        _sorter->add(recordId, dataForSorter);

        // To provide as accurate memory accounting as possible, we update the memory tracker after
        // we add each document to the sorter, instead of updating it after the sorter is completely
        // populated.
        auto currMemUsage = _sorter->stats().memUsage();
        _memoryTracker.add(currMemUsage - prevMemUsage);
        prevMemUsage = currMemUsage;
        _scores.erase(it++);
    }
    _sorter->spill();

    auto spilledDataStorageIncrease =
        _specificStats.spillingStats.updateSpillingStats(1 /*spills*/,
                                                         _memoryTracker.inUseTrackedMemoryBytes(),
                                                         recordsToSpill,
                                                         _sorterStats->bytesSpilled());
    textOrCounters.incrementPerSpilling(1 /*spills*/,
                                        _memoryTracker.inUseTrackedMemoryBytes(),
                                        recordsToSpill,
                                        spilledDataStorageIncrease);
    _memoryTracker.set(0);

    if (_internalState == State::kReturningResults) {
        _sorterIterator = _sorter->done();
    }
}

void TextOrStage::initSorter() {
    uassert(ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed,
            "Exceeded memory limit for TEXT_OR, but didn't allow external sort."
            " Pass allowDiskUse:true to opt in.",
            expCtx()->getAllowDiskUse());

    // We disable automatic spilling inside Sorter because we manually spill after adding the whole
    // batch to the _sorter.
    static constexpr size_t kMaxMemoryUsageForSorter = std::numeric_limits<size_t>::max();

    _sorterStats = std::make_unique<SorterFileStats>(nullptr /*sorterTracker*/);
    _sorter = Sorter<RecordId, TextRecordDataForSorter>::make(
        SortOptions{}
            .FileStats(_sorterStats.get())
            .MaxMemoryUsageBytes(kMaxMemoryUsageForSorter)
            .TempDir(expCtx()->getTempDir()),
        [](const RecordId& lhs, const RecordId& rhs) { return lhs.compare(rhs); });
}

}  // namespace mongo
