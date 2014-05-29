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

        class MultiIteratorRunner : public Runner {
        public:
            MultiIteratorRunner( const StringData& ns, Collection* collection )
                : _ns( ns.toString() ),
                  _collection( collection ) {
            }
            ~MultiIteratorRunner() {
            }

            // takes ownership of it
            void addIterator(RecordIterator* it) {
                _iterators.push_back(it);
            }

            virtual RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
                if ( _collection == NULL )
                    return RUNNER_DEAD;

                DiskLoc next = _advance();
                if (next.isNull())
                    return RUNNER_EOF;

                if ( objOut )
                    *objOut = _collection->docFor( next );
                if ( dlOut )
                    *dlOut = next;
                return RUNNER_ADVANCED;
            }

            virtual bool isEOF() {
                return _collection == NULL || _iterators.empty();
            }
            virtual void kill() {
                _collection = NULL;
                _iterators.clear();
            }
            virtual void saveState() {
                for (size_t i = 0; i < _iterators.size(); i++) {
                    _iterators[i]->prepareToYield();
                }
            }
            virtual bool restoreState(OperationContext* opCtx) {
                for (size_t i = 0; i < _iterators.size(); i++) {
                    if (!_iterators[i]->recoverFromYield()) {
                        kill();
                        return false;
                    }
                }
                return true;
            }

            virtual const string& ns() { return _ns; }
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
            virtual const Collection* collection() {
                return _collection;
            }
            virtual Status getInfo(TypeExplain** explain, PlanInfo** planInfo) const {
                return Status( ErrorCodes::InternalError, "no" );
            }
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

            string _ns;
            Collection* _collection;
            OwnedPointerVector<RecordIterator> _iterators;
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

            OwnedPointerVector<RecordIterator> iterators(collection->getManyIterators());

            if (iterators.size() < numCursors) {
                numCursors = iterators.size();
            }

            OwnedPointerVector<MultiIteratorRunner> runners;
            for ( size_t i = 0; i < numCursors; i++ ) {
                runners.push_back(new MultiIteratorRunner(ns.ns(), collection));
            }

            // transfer iterators to runners using a round-robin distribution.
            // TODO consider using a common work queue once invalidation issues go away.
            for (size_t i = 0; i < iterators.size(); i++) {
                runners[i % runners.size()]->addIterator(iterators.releaseAt(i));
            }

            {
                BSONArrayBuilder bucketsBuilder;
                for (size_t i = 0; i < runners.size(); i++) {
                    // transfer ownership of a runner to the ClientCursor (which manages its own
                    // lifetime).
                    ClientCursor* cc = new ClientCursor( collection, runners.releaseAt(i) );

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
