// parallel_collection_scan.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/util/touch_pages.h"

namespace mongo {


    class ParallelCollectionScanCmd : public Command {
    public:

        struct ExtentInfo {
            ExtentInfo( DiskLoc dl, size_t s )
                : diskLoc(dl), size(s) {
            }
            DiskLoc diskLoc;
            size_t size;
        };

        // XXX: move this to the exec/ directory.
        class MultiIteratorStage : public PlanStage {
        public:
            MultiIteratorStage(WorkingSet* ws, Collection* collection)
                : _collection(collection),
                  _ws(ws) { }

            ~MultiIteratorStage() { }

            // takes ownership of it
            void addIterator(RecordIterator* it) {
                _iterators.push_back(it);
            }

            virtual StageState work(WorkingSetID* out) {
                if ( _collection == NULL )
                    return PlanStage::DEAD;

                DiskLoc next = _advance();
                if (next.isNull())
                    return PlanStage::IS_EOF;

                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->loc = next;
                member->obj = _collection->docFor(next);
                member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                return PlanStage::ADVANCED;
            }

            virtual bool isEOF() {
                return _collection == NULL || _iterators.empty();
            }

            void kill() {
                _collection = NULL;
                _iterators.clear();
            }

            virtual void saveState() {
                for (size_t i = 0; i < _iterators.size(); i++) {
                    _iterators[i]->saveState();
                }
            }

            virtual void restoreState(OperationContext* opCtx) {
                for (size_t i = 0; i < _iterators.size(); i++) {
                    if (!_iterators[i]->restoreState()) {
                        kill();
                    }
                }
            }

            virtual void invalidate(const DiskLoc& dl, InvalidationType type) {
                switch ( type ) {
                case INVALIDATION_DELETION:
                    for (size_t i = 0; i < _iterators.size(); i++) {
                        _iterators[i]->invalidate(dl);
                    }
                    break;
                case INVALIDATION_MUTATION:
                    // no-op
                    break;
                }
            }

            //
            // These should not be used.
            //

            virtual PlanStageStats* getStats() { return NULL; }
            virtual CommonStats* getCommonStats() { return NULL; }
            virtual SpecificStats* getSpecificStats() { return NULL; }

            virtual std::vector<PlanStage*> getChildren() const {
                vector<PlanStage*> empty;
                return empty;
            }

            virtual StageType stageType() const { return STAGE_MULTI_ITERATOR; }

        private:

            /**
             * @return if more data
             */
            DiskLoc _advance() {
                while (!_iterators.empty()) {
                    DiskLoc out = _iterators.back()->getNext();
                    if (!out.isNull())
                        return out;

                    _iterators.popAndDeleteBack();
                }

                return DiskLoc();
            }

            Collection* _collection;
            OwnedPointerVector<RecordIterator> _iterators;

            // Not owned by us.
            WorkingSet* _ws;
        };

        // ------------------------------------------------

        ParallelCollectionScanCmd() : Command( "parallelCollectionScan" ){}

        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return true; }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            Privilege p(parseResourcePattern(dbname, cmdObj), actions);
            if ( client->getAuthorizationSession()->isAuthorizedForPrivilege(p) )
                return Status::OK();
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int options,
                          string& errmsg, BSONObjBuilder& result,
                          bool fromRepl = false ) {

            NamespaceString ns( dbname, cmdObj[name].String() );

            Client::ReadContext ctx(txn, ns.ns());

            Database* db = ctx.ctx().db();
            Collection* collection = db->getCollection( txn, ns );

            if ( !collection )
                return appendCommandStatus( result,
                                            Status( ErrorCodes::NamespaceNotFound,
                                                    str::stream() <<
                                                    "ns does not exist: " << ns.ns() ) );

            size_t numCursors = static_cast<size_t>( cmdObj["numCursors"].numberInt() );

            if ( numCursors == 0 || numCursors > 10000 )
                return appendCommandStatus( result,
                                            Status( ErrorCodes::BadValue,
                                                    str::stream() <<
                                                    "numCursors has to be between 1 and 10000" <<
                                                    " was: " << numCursors ) );

            OwnedPointerVector<RecordIterator> iterators(collection->getManyIterators(txn));

            if (iterators.size() < numCursors) {
                numCursors = iterators.size();
            }

            OwnedPointerVector<PlanExecutor> execs;
            for ( size_t i = 0; i < numCursors; i++ ) {
                WorkingSet* ws = new WorkingSet();
                MultiIteratorStage* mis = new MultiIteratorStage(ws, collection);
                // Takes ownership of 'ws' and 'mis'.
                execs.push_back(new PlanExecutor(ws, mis, collection));
            }

            // transfer iterators to executors using a round-robin distribution.
            // TODO consider using a common work queue once invalidation issues go away.
            for (size_t i = 0; i < iterators.size(); i++) {
                PlanExecutor* theExec = execs[i % execs.size()];
                MultiIteratorStage* mis = static_cast<MultiIteratorStage*>(theExec->getRootStage());
                mis->addIterator(iterators.releaseAt(i));
            }

            {
                BSONArrayBuilder bucketsBuilder;
                for (size_t i = 0; i < execs.size(); i++) {
                    // transfer ownership of an executor to the ClientCursor (which manages its own
                    // lifetime).
                    ClientCursor* cc = new ClientCursor( collection, execs.releaseAt(i) );

                    // we are mimicking the aggregation cursor output here
                    // that is why there are ns, ok and empty firstBatch
                    BSONObjBuilder threadResult;
                    {
                        BSONObjBuilder cursor;
                        cursor.appendArray( "firstBatch", BSONObj() );
                        cursor.append( "ns", ns );
                        cursor.append( "id", cc->cursorid() );
                        threadResult.append( "cursor", cursor.obj() );
                    }
                    threadResult.appendBool( "ok", 1 );

                    bucketsBuilder.append( threadResult.obj() );
                }
                result.appendArray( "cursors", bucketsBuilder.obj() );
            }

            return true;

        }
    } parallelCollectionScanCmd;

}
