/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/exec/idhack.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* IDHackStage::kStageType = "IDHACK";

IDHackStage::IDHackStage(OperationContext* opCtx,
                         const Collection* collection,
                         CanonicalQuery* query,
                         WorkingSet* ws,
                         const IndexDescriptor* descriptor)
    : PlanStage(kStageType, opCtx),
      _collection(collection),
      _workingSet(ws),
      _key(query->getQueryObj()["_id"].wrap()),
      _done(false),
      _idBeingPagedIn(WorkingSet::INVALID_ID) {
    const IndexCatalog* catalog = _collection->getIndexCatalog();
    _specificStats.indexName = descriptor->indexName();
    _accessMethod = catalog->getIndex(descriptor);

    if (NULL != query->getProj()) {
        _addKeyMetadata = query->getProj()->wantIndexKey();
    } else {
        _addKeyMetadata = false;
    }
}

IDHackStage::IDHackStage(OperationContext* opCtx,
                         Collection* collection,
                         const BSONObj& key,
                         WorkingSet* ws,
                         const IndexDescriptor* descriptor)
    : PlanStage(kStageType, opCtx),
      _collection(collection),
      _workingSet(ws),
      _key(key),
      _done(false),
      _addKeyMetadata(false),
      _idBeingPagedIn(WorkingSet::INVALID_ID) {
    const IndexCatalog* catalog = _collection->getIndexCatalog();
    _specificStats.indexName = descriptor->indexName();
    _accessMethod = catalog->getIndex(descriptor);
}

IDHackStage::~IDHackStage() {}

bool IDHackStage::isEOF() {
    if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
        // We asked the parent for a page-in, but still haven't had a chance to return the
        // paged in document
        return false;
    }

    return _done;
}

PlanStage::StageState IDHackStage::doWork(WorkingSetID* out) {
    if (_done) {
        return PlanStage::IS_EOF;
    }

    if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
        invariant(_recordCursor);
        WorkingSetID id = _idBeingPagedIn;
        _idBeingPagedIn = WorkingSet::INVALID_ID;

        invariant(WorkingSetCommon::fetchIfUnfetched(getOpCtx(), _workingSet, id, _recordCursor));

        WorkingSetMember* member = _workingSet->get(id);
        return advance(id, member, out);
    }

    WorkingSetID id = WorkingSet::INVALID_ID;
    try {
        // Look up the key by going directly to the index.
        RecordId recordId = _accessMethod->findSingle(getOpCtx(), _key);

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
            _recordCursor = _collection->getCursor(getOpCtx());

        // We may need to request a yield while we fetch the document.
        if (auto fetcher = _recordCursor->fetcherForId(recordId)) {
            // There's something to fetch. Hand the fetcher off to the WSM, and pass up a
            // fetch request.
            _idBeingPagedIn = id;
            member->setFetcher(fetcher.release());
            *out = id;
            return NEED_YIELD;
        }

        // The doc was already in memory, so we go ahead and return it.
        if (!WorkingSetCommon::fetch(getOpCtx(), _workingSet, id, _recordCursor)) {
            // _id is immutable so the index would return the only record that could
            // possibly match the query.
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
        BSONObjBuilder bob;
        BSONObj ownedKeyObj = member->obj.value()["_id"].wrap().getOwned();
        bob.appendKeys(_key, ownedKeyObj);
        member->addComputed(new IndexKeyComputedData(bob.obj()));
    }

    _done = true;
    *out = id;
    return PlanStage::ADVANCED;
}

void IDHackStage::doSaveState() {
    if (_recordCursor)
        _recordCursor->saveUnpositioned();
}

void IDHackStage::doRestoreState() {
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

void IDHackStage::doInvalidate(OperationContext* opCtx, const RecordId& dl, InvalidationType type) {
    // Since updates can't mutate the '_id' field, we can ignore mutation invalidations.
    if (INVALIDATION_MUTATION == type) {
        return;
    }

    // It's possible that the RecordId getting invalidated is the one we're about to
    // fetch. In this case we do a "forced fetch" and put the WSM in owned object state.
    if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
        WorkingSetMember* member = _workingSet->get(_idBeingPagedIn);
        if (member->hasRecordId() && (member->recordId == dl)) {
            // Fetch it now and kill the RecordId.
            WorkingSetCommon::fetchAndInvalidateRecordId(opCtx, member, _collection);
        }
    }
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
