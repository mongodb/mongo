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

#include "mongo/db/structure/btree/btree.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/exec/projection_exec.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/type_explain.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    IDHackRunner::IDHackRunner(Collection* collection, CanonicalQuery* query)
        : _collection(collection),
          _query(query),
          _killed(false),
          _done(false) { }

    IDHackRunner::~IDHackRunner() { }

    Runner::RunnerState IDHackRunner::getNext(BSONObj* objOut, DiskLoc* dlOut) {
        if (_killed) { return Runner::RUNNER_DEAD; }
        if (_done) { return Runner::RUNNER_EOF; }

        // Use the index catalog to get the id index.
        IndexCatalog* catalog = _collection->getIndexCatalog();

        // Find the index we use.
        const IndexDescriptor* idDesc = catalog->findIdIndex();
        if (NULL == idDesc) {
            _done = true;
            return Runner::RUNNER_EOF;
        }

        // XXX: This may not be valid always.  See SERVER-12397.
        BtreeBasedAccessMethod* accessMethod =
            static_cast<BtreeBasedAccessMethod*>(catalog->getIndex(idDesc));

        BSONObj key = _query->getQueryObj()["_id"].wrap();

        // Look up the key by going directly to the Btree.
        DiskLoc loc = accessMethod->findSingle( key );

        _done = true;

        // Key not found.
        if (loc.isNull()) {
            return Runner::RUNNER_EOF;
        }

        // Set out parameters and note that we're done w/lookup.
        if (NULL != objOut) {
            Record* record = loc.rec();

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
                        return Runner::RUNNER_DEAD;
                    }
                }
            }

            // Either the data was in memory or we paged it in.
            *objOut = loc.obj();

            // If we're sharded make sure the key belongs to us.  We need the object to do this.
            if (shardingState.needCollectionMetadata(_query->ns())) {
                CollectionMetadataPtr m = shardingState.getCollectionMetadata(_query->ns());
                if (m) {
                    KeyPattern kp(m->getKeyPattern());
                    if (!m->keyBelongsToMe( kp.extractSingleKey(*objOut))) {
                        // We have something with a matching _id but it doesn't belong to me.
                        return Runner::RUNNER_EOF;
                    }
                }
            }

            // If there is a projection...
            if (NULL != _query->getProj()) {
                // Create something to execute it.
                auto_ptr<ProjectionExec> projExec(new ProjectionExec(_query->getParsed().getProj(),
                                                                     _query->root()));
                projExec->transform(*objOut, objOut);
            }
        }

        // Return the DiskLoc if the caller wants it.
        if (NULL != dlOut) {
            *dlOut = loc;
        }

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
        return _query->getParsed().ns();
    }

    void IDHackRunner::kill() {
        _killed = true;
    }

    Status IDHackRunner::getExplainPlan(TypeExplain** explain) const {
        // The explain plan simply indicates that the plan is idhack.
        *explain = new TypeExplain();
        (*explain)->setIDHack(true);
        return Status::OK();
    }

} // namespace mongo
