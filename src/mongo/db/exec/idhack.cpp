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
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/storage/record_fetcher.h"
#include "mongo/s/d_state.h"

namespace mongo {

    using std::auto_ptr;
    using std::vector;

    // static
    const char* IDHackStage::kStageType = "IDHACK";

    IDHackStage::IDHackStage(OperationContext* txn, const Collection* collection,
                             CanonicalQuery* query, WorkingSet* ws)
        : _txn(txn),
          _collection(collection),
          _workingSet(ws),
          _key(query->getQueryObj()["_id"].wrap()),
          _done(false),
          _idBeingPagedIn(WorkingSet::INVALID_ID),
          _commonStats(kStageType) {
        if (NULL != query->getProj()) {
            _addKeyMetadata = query->getProj()->wantIndexKey();
        }
        else {
            _addKeyMetadata = false;
        }
    }

    IDHackStage::IDHackStage(OperationContext* txn, Collection* collection,
                             const BSONObj& key, WorkingSet* ws)
        : _txn(txn),
          _collection(collection),
          _workingSet(ws),
          _key(key),
          _done(false),
          _addKeyMetadata(false),
          _idBeingPagedIn(WorkingSet::INVALID_ID),
          _commonStats(kStageType) { }

    IDHackStage::~IDHackStage() { }

    bool IDHackStage::isEOF() {
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            // We asked the parent for a page-in, but still haven't had a chance to return the
            // paged in document
            return false;
        }

        return  _done;
    }

    PlanStage::StageState IDHackStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (_done) { return PlanStage::IS_EOF; }

        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            WorkingSetID id = _idBeingPagedIn;
            _idBeingPagedIn = WorkingSet::INVALID_ID;
            WorkingSetMember* member = _workingSet->get(id);

            WorkingSetCommon::completeFetch(_txn, member, _collection);

            return advance(id, member, out);
        }

        // Use the index catalog to get the id index.
        const IndexCatalog* catalog = _collection->getIndexCatalog();

        // Find the index we use.
        IndexDescriptor* idDesc = catalog->findIdIndex(_txn);
        if (NULL == idDesc) {
            _done = true;
            return PlanStage::IS_EOF;
        }

        // This may not be valid always.  See SERVER-12397.
        const BtreeBasedAccessMethod* accessMethod =
            static_cast<const BtreeBasedAccessMethod*>(catalog->getIndex(idDesc));

        // Look up the key by going directly to the Btree.
        RecordId loc = accessMethod->findSingle(_txn, _key);

        // Key not found.
        if (loc.isNull()) {
            _done = true;
            return PlanStage::IS_EOF;
        }

        ++_specificStats.keysExamined;
        ++_specificStats.docsExamined;

        // Create a new WSM for the result document.
        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = loc;
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

        // We may need to request a yield while we fetch the document.
        std::auto_ptr<RecordFetcher> fetcher(_collection->documentNeedsFetch(_txn, loc));
        if (NULL != fetcher.get()) {
            // There's something to fetch. Hand the fetcher off to the WSM, and pass up a
            // fetch request.
            _idBeingPagedIn = id;
            member->setFetcher(fetcher.release());
            *out = id;
            _commonStats.needFetch++;
            return NEED_FETCH;
        }

        // The doc was already in memory, so we go ahead and return it.
        member->obj = _collection->docFor(_txn, member->loc);
        return advance(id, member, out);
    }

    PlanStage::StageState IDHackStage::advance(WorkingSetID id,
                                               WorkingSetMember* member,
                                               WorkingSetID* out) {
        invariant(member->hasObj());

        if (_addKeyMetadata) {
            BSONObjBuilder bob;
            BSONObj ownedKeyObj = member->obj["_id"].wrap().getOwned();
            bob.appendKeys(_key, ownedKeyObj);
            member->addComputed(new IndexKeyComputedData(bob.obj()));
        }

        _done = true;
        ++_commonStats.advanced;
        *out = id;
        return PlanStage::ADVANCED;
    }

    void IDHackStage::saveState() {
        _txn = NULL;
        ++_commonStats.yields;
    }

    void IDHackStage::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;
    }

    void IDHackStage::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        // Since updates can't mutate the '_id' field, we can ignore mutation invalidations.
        if (INVALIDATION_MUTATION == type) {
            return;
        }

        // It's possible that the loc getting invalidated is the one we're about to
        // fetch. In this case we do a "forced fetch" and put the WSM in owned object state.
        if (WorkingSet::INVALID_ID != _idBeingPagedIn) {
            WorkingSetMember* member = _workingSet->get(_idBeingPagedIn);
            if (member->hasLoc() && (member->loc == dl)) {
                // Fetch it now and kill the diskloc.
                WorkingSetCommon::fetchAndInvalidateLoc(_txn, member, _collection);
            }
        }
    }

    // static
    bool IDHackStage::supportsQuery(const CanonicalQuery& query) {
        return !query.getParsed().showDiskLoc()
            && query.getParsed().getHint().isEmpty()
            && 0 == query.getParsed().getSkip()
            && CanonicalQuery::isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().getOptions().tailable;
    }

    vector<PlanStage*> IDHackStage::getChildren() const {
        vector<PlanStage*> empty;
        return empty;
    }

    PlanStageStats* IDHackStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_IDHACK));
        ret->specific.reset(new IDHackStats(_specificStats));
        return ret.release();
    }

    const CommonStats* IDHackStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* IDHackStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
