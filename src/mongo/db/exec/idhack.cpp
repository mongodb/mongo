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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/idhack.h"

#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* IDHackStage::kStageType = "IDHACK";

IDHackStage::IDHackStage(OperationContext* opCtx,
                         CanonicalQuery* query,
                         WorkingSet* ws,
                         const IndexDescriptor* descriptor)
    : RequiresIndexStage(kStageType, opCtx, descriptor),
      _workingSet(ws),
      _key(query->getQueryObj()["_id"].wrap()) {
    _specificStats.indexName = descriptor->indexName();
    if (NULL != query->getProj()) {
        _addKeyMetadata = query->getProj()->wantIndexKey();
    }
}

IDHackStage::IDHackStage(OperationContext* opCtx,
                         const BSONObj& key,
                         WorkingSet* ws,
                         const IndexDescriptor* descriptor)
    : RequiresIndexStage(kStageType, opCtx, descriptor), _workingSet(ws), _key(key) {
    _specificStats.indexName = descriptor->indexName();
}

IDHackStage::~IDHackStage() {}

bool IDHackStage::isEOF() {
    return _done;
}

PlanStage::StageState IDHackStage::doWork(WorkingSetID* out) {
    if (_done) {
        return PlanStage::IS_EOF;
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    try {
        // Look up the key by going directly to the index.
        RecordId recordId = indexAccessMethod()->findSingle(getOpCtx(), _key);

        // Key not found.
        if (recordId.isNull()) {
            _done = true;
            return PlanStage::IS_EOF;
        }

        ++_specificStats.keysExamined;
        ++_specificStats.docsExamined;

        // Create a new WSM for the result document.
        id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->recordId = recordId;
        _workingSet->transitionToRecordIdAndIdx(id);

        if (!_recordCursor)
            _recordCursor = collection()->getCursor(getOpCtx());

        // Find the document associated with 'id' in the collection's record store.
        if (!WorkingSetCommon::fetch(getOpCtx(), _workingSet, id, _recordCursor)) {
            // We didn't find a document with RecordId 'id'.
            _workingSet->free(id);
            _commonStats.isEOF = true;
            _done = true;
            return IS_EOF;
        }

        return advance(id, member, out);
    } catch (const WriteConflictException&) {
        // Restart at the beginning on retry.
        _recordCursor.reset();
        if (id != WorkingSet::INVALID_ID)
            _workingSet->free(id);

        *out = WorkingSet::INVALID_ID;
        return NEED_YIELD;
    }
}

PlanStage::StageState IDHackStage::advance(WorkingSetID id,
                                           WorkingSetMember* member,
                                           WorkingSetID* out) {
    invariant(member->hasObj());

    if (_addKeyMetadata) {
        BSONObj ownedKeyObj = member->obj.value()["_id"].wrap().getOwned();
        member->addComputed(
            new IndexKeyComputedData(IndexKeyComputedData::rehydrateKey(_key, ownedKeyObj)));
    }

    _done = true;
    *out = id;
    return PlanStage::ADVANCED;
}

void IDHackStage::doSaveStateRequiresIndex() {
    if (_recordCursor)
        _recordCursor->saveUnpositioned();
}

void IDHackStage::doRestoreStateRequiresIndex() {
    if (_recordCursor)
        _recordCursor->restore();
}

void IDHackStage::doDetachFromOperationContext() {
    if (_recordCursor)
        _recordCursor->detachFromOperationContext();
}

void IDHackStage::doReattachToOperationContext() {
    if (_recordCursor)
        _recordCursor->reattachToOperationContext(getOpCtx());
}

// static
bool IDHackStage::supportsQuery(Collection* collection, const CanonicalQuery& query) {
    return !query.getQueryRequest().showRecordId() && query.getQueryRequest().getHint().isEmpty() &&
        !query.getQueryRequest().getSkip() &&
        CanonicalQuery::isSimpleIdQuery(query.getQueryRequest().getFilter()) &&
        !query.getQueryRequest().isTailable() &&
        CollatorInterface::collatorsMatch(query.getCollator(), collection->getDefaultCollator());
}

unique_ptr<PlanStageStats> IDHackStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_IDHACK);
    ret->specific = make_unique<IDHackStats>(_specificStats);
    return ret;
}

const SpecificStats* IDHackStage::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
