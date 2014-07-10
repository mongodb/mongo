/**
 *    Copyright 2013-2014 MongoDB Inc.
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
#include "mongo/db/exec/projection.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    IDHackRunner::IDHackRunner(OperationContext* txn,
                               const Collection* collection,
                               CanonicalQuery* query)
        : _txn(txn),
          _collection(collection),
          _key(query->getQueryObj()["_id"].wrap()),
          _query(query),
          _killed(false),
          _done(false),
          _nscanned(0),
          _nscannedObjects(0) { }

    IDHackRunner::IDHackRunner(OperationContext* txn, Collection* collection, const BSONObj& key)
        : _txn(txn),
          _collection(collection),
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
        DiskLoc loc = accessMethod->findSingle(_txn,  _key );

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
        else {
            // If we're sharded, get the config metadata for this collection.  This will be used
            // later to see if we own the document to be returned.
            //
            // Query execution machinery should generally delegate to ShardFilterStage in order to
            // accomplish this task.  It is only safe to rely on the state of the config metadata
            // here because it is not possible for the config metadata to change during the lifetime
            // of the IDHackRunner (since the IDHackRunner returns only a single document, the
            // config metadata must be the same as it was when the query started).
            CollectionMetadataPtr collectionMetadata;
            if (shardingState.needCollectionMetadata(_collection->ns().ns())) {
                collectionMetadata = shardingState.getCollectionMetadata(_collection->ns().ns());
            }

            // If we're not sharded, consider a covered projection (we can't if we're sharded, since
            // we require a fetch in order to apply the sharding filter).
            if (!collectionMetadata && hasCoveredProjection()) {
                // Covered query on _id field only.  Set object to search key.  Search key is
                // retrieved from the canonical query at construction and always contains the _id
                // field name.  It is possible to construct the ID hack runner with just the
                // collection and the key object (which could be {"": my_obj_id}) but _query would
                // be null in that case and the query would never be seen as covered.
                *objOut = _key.getOwned();
            }
            // Otherwise, fetch the document.
            else {
                *objOut = _collection->docFor(loc);
                _nscannedObjects++;

                // If we're sharded, make sure the key belongs to us.
                if (collectionMetadata) {
                    KeyPattern kp(collectionMetadata->getKeyPattern());
                    if (!collectionMetadata->keyBelongsToMe(kp.extractSingleKey(*objOut))) {
                        // We have something with a matching _id but it doesn't belong to me.
                        _done = true;
                        return Runner::RUNNER_EOF;
                    }
                }

                // Apply the projection if one was requested.
                if (_query && _query->getProj()) {
                    *objOut = applyProjection(*objOut);
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

    BSONObj IDHackRunner::applyProjection(const BSONObj& docObj) const {
        invariant(_query && _query->getProj());

        // We have a non-covered projection (covered projections should be handled earlier,
        // in getNext(..). For simple inclusion projections we use a fast path similar to that
        // implemented in the ProjectionStage. For non-simple inclusion projections we fallback
        // to ProjectionExec.
        const BSONObj& projObj = _query->getParsed().getProj();

        if (_query->getProj()->wantIndexKey()) {
            // $returnKey is specified. This overrides everything else.
            BSONObjBuilder bob;
            const BSONObj& queryObj = _query->getParsed().getFilter();
            bob.append(queryObj["_id"]);
            return bob.obj();
        }
        else if (_query->getProj()->requiresDocument()) {
            // Not a simple projection, so fallback on the regular projection path.
            BSONObj projectedObj;
            ProjectionExec projExec(projObj,
                                    _query->root(),
                                    WhereCallbackReal(_collection->ns().db()));
            projExec.transform(docObj, &projectedObj);
            return projectedObj;
        }
        else {
            // This is a simple inclusion projection. Start by getting the set
            // of fields to include.
            unordered_set<StringData, StringData::Hasher> includedFields;
            ProjectionStage::getSimpleInclusionFields(projObj, &includedFields);

            // Apply the simple inclusion projection.
            BSONObjBuilder bob;
            ProjectionStage::transformSimpleInclusion(docObj, includedFields, bob);

            return bob.obj();
        }
    }

    bool IDHackRunner::isEOF() {
        return _killed || _done;
    }

    void IDHackRunner::saveState() { }

    bool IDHackRunner::restoreState(OperationContext* opCtx) { return true; }

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
            // ID hack queries are only considered covered if they have the projection {_id: 1}.
            (*explain)->setIndexOnly(hasCoveredProjection());
        }
        else if (NULL != planInfo) {
            *planInfo = new PlanInfo();
            (*planInfo)->planSummary = "IDHACK";
        }

        return Status::OK();
    }

    // static
    bool IDHackRunner::hasCoveredProjection() const {
        // Some update operations use the IDHackRunner without creating a
        // canonical query. In this case, _query will be NULL. Just return
        // false, as we won't have to do any projection handling for updates.
        if (NULL == _query.get()) {
            return false;
        }

        const ParsedProjection* proj = _query->getProj();
        if (!proj) {
            return false;
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
