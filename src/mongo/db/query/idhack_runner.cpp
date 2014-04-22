/**
 *    Copyright 2013 MongoDB Inc.
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

#include "mongo/db/query/idhack_runner.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/record.h"
#include "mongo/s/d_logic.h"

namespace {

    using namespace mongo;

    /**
     * Does the query contain a projection on {_id: 1}?
     */
    bool hasIDProjection(const CanonicalQuery* query) {
        // We don't know the answer if the query is NULL.
        if (!query) {
            return false;
        }
        // No projection means not covered.
        if (!query->getProj()) {
            return false;
        }
        // Since the only supported projection is {_id: 1},
        // a valid ParsedProjection is enough to indicate that
        // we have a covered query.
        return true;
     }

} // namespace

namespace mongo {

    IDHackRunner::IDHackRunner(const Collection* collection, CanonicalQuery* query)
        : _collection(collection),
          _key(query->getQueryObj()["_id"].wrap()),
          _query(query),
          _killed(false),
          _done(false),
          _nscanned(0),
          _nscannedObjects(0) { }

    IDHackRunner::IDHackRunner(Collection* collection, const BSONObj& key)
        : _collection(collection),
          _key(key),
          _query(NULL),
          _killed(false),
          _done(false) { }

    IDHackRunner::~IDHackRunner() { }

    Runner::RunnerState IDHackRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return Runner::RUNNER_DEAD; }
        if (_done) { return Runner::RUNNER_EOF; }

        // Use the index catalog to get the id index.
        const IndexCatalog* catalog = _collection->getIndexCatalog();

        // Find the index we use.
        IndexDescriptor* idDesc = catalog->findIdIndex();
        if (NULL == idDesc) {
            _done = true;
            return Runner::RUNNER_EOF;
        }

        // This may not be valid always.  See SERVER-12397.
        const BtreeBasedAccessMethod* accessMethod =
            static_cast<const BtreeBasedAccessMethod*>(catalog->getIndex(idDesc));

        // Look up the key by going directly to the Btree.
        DiskLoc loc = accessMethod->findSingle( _key );

        // Key not found.
        if (loc.isNull()) {
            _done = true;
            return Runner::RUNNER_EOF;
        }

        _nscanned++;

        // Set out parameters and note that we're done w/lookup.
        if (NULL == objOut) {
            // No object requested - nothing to do.
        }
        else if (hasIDProjection(_query.get())) {
            // Covered query on _id field only.
            // Set object to search key.
            // Search key is retrieved from the canonical query at
            // construction and always contains the _id field name.
            // It is possible to construct the ID hack runner with just the collection
            // and the key object (which could be {"": my_obj_id}) but _query would be null
            // in that case and the query would never be seen as covered.
            *objOut = _key.getOwned();
        }
        else {
            invariant(!hasIDProjection(_query.get()));

            // Fetch object from storage.
            Record* record = _collection->getRecordStore()->recordFor( loc );

            _nscannedObjects++;

            // If the record isn't in memory...
            if (!Record::likelyInPhysicalMemory(record->dataNoThrowing())) {
                // And we're allowed to yield ourselves...
                if (Runner::YIELD_AUTO == _policy) {
                    // Note what we're yielding to fetch so that we don't crash if the loc is
                    // deleted during a yield.
                    _locFetching = loc;
                    // Yield.  TODO: Do we want to bother yielding if micros < 0?
                    int micros = ClientCursor::suggestYieldMicros();
                    ClientCursor::staticYield(micros, "", record);
                    // This can happen when we're yielded for various reasons (e.g. db/idx dropped).
                    if (_killed) {
                        _done = true;
                        return Runner::RUNNER_DEAD;
                    }
                }
            }

            // Either the data was in memory or we paged it in.
            *objOut = loc.obj();

            // If we're sharded make sure the key belongs to us.  We need the object to do this.
            if (shardingState.needCollectionMetadata(_collection->ns().ns())) {
                CollectionMetadataPtr m = shardingState.getCollectionMetadata(_collection->ns().ns());
                if (m) {
                    KeyPattern kp(m->getKeyPattern());
                    if (!m->keyBelongsToMe( kp.extractSingleKey(*objOut))) {
                        // We have something with a matching _id but it doesn't belong to me.
                        _done = true;
                        return Runner::RUNNER_EOF;
                    }
                }
            }
        }

        // Return the DiskLoc if the caller wants it.
        if (NULL != dlOut) {
            *dlOut = loc;
        }

        _done = true;
        return Runner::RUNNER_ADVANCED;
    }

    bool IDHackRunner::isEOF() {
        return _killed || _done;
    }

    void IDHackRunner::saveState() { }

    bool IDHackRunner::restoreState() { return true; }

    void IDHackRunner::setYieldPolicy(Runner::YieldPolicy policy) {
        if (_done || _killed) { return; }
        _policy = policy;
    }

    // Nothing to do here, holding no state.
    void IDHackRunner::invalidate(const DiskLoc& dl, InvalidationType type) {
        if (_done || _killed) { return; }
        if (_locFetching == dl && (type == INVALIDATION_DELETION)) {
            _locFetching = DiskLoc();
            _killed = true;
        }
    }

    const std::string& IDHackRunner::ns() {
        return _collection->ns().ns();
    }

    void IDHackRunner::kill() {
        _killed = true;
        _collection = NULL;
    }

    Status IDHackRunner::getInfo(TypeExplain** explain,
                                 PlanInfo** planInfo) const {
        // The explain plan simply indicates that the plan is idhack.
        if (NULL != explain) {
            *explain = new TypeExplain();
            // Explain format does not match 2.4 and is intended
            // to indicate clearly that the ID hack has been applied.
            (*explain)->setCursor("IDCursor");
            (*explain)->setIDHack(true);
            (*explain)->setN(_nscanned);
            (*explain)->setNScanned(_nscanned);
            (*explain)->setNScannedObjects(_nscannedObjects);
            BSONElement keyElt = _key.firstElement();
            BSONObj indexBounds = BSON("_id" << BSON_ARRAY( BSON_ARRAY( keyElt << keyElt ) ) );
            (*explain)->setIndexBounds(indexBounds);
            // Covered projection is only one supported.
            (*explain)->setIndexOnly(hasIDProjection(_query.get()));
        }
        else if (NULL != planInfo) {
            *planInfo = new PlanInfo();
            (*planInfo)->planSummary = "IDHACK";
        }

        return Status::OK();
    }

    // static
    bool IDHackRunner::supportsQuery(const CanonicalQuery& query) {
        return !query.getParsed().showDiskLoc()
            && query.getParsed().getHint().isEmpty()
            && 0 == query.getParsed().getSkip()
            && canUseProjection(query)
            && CanonicalQuery::isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().hasOption(QueryOption_CursorTailable);
    }

    // static
    bool IDHackRunner::canUseProjection(const CanonicalQuery& query) {
        const ParsedProjection* proj = query.getProj();

        // No projection is OK - ID Hack will fetch entire document.
        if (!proj) {
            return true;
        }

        // If there is a projection, it has to be a covered projection on
        // the _id field only.
        if (proj->requiresDocument()) {
            return false;
        }
        const std::vector<std::string>& requiredFields = proj->getRequiredFields();
        if (1U != requiredFields.size()) {
            return false;
        }
        if ("_id" != requiredFields[0]) {
            return false;
        }

        // Can use this projection with ID Hack.
        return true;
    }

} // namespace mongo
