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
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/multi_iterator.h"
#include "mongo/util/touch_pages.h"

namespace mongo {

    using std::auto_ptr;
    using std::string;

    class ParallelCollectionScanCmd : public Command {
    public:

        struct ExtentInfo {
            ExtentInfo( RecordId dl, size_t s )
                : diskLoc(dl), size(s) {
            }
            RecordId diskLoc;
            size_t size;
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
            if ( AuthorizationSession::get(client)->isAuthorizedForPrivilege(p) )
                return Status::OK();
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int options,
                          string& errmsg, BSONObjBuilder& result,
                          bool fromRepl = false ) {

            NamespaceString ns( dbname, cmdObj[name].String() );

            AutoGetCollectionForRead ctx(txn, ns.ns());

            Collection* collection = ctx.getCollection();
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
                MultiIteratorStage* mis = new MultiIteratorStage(txn, ws, collection);

                PlanExecutor* rawExec;
                // Takes ownership of 'ws' and 'mis'.
                Status execStatus = PlanExecutor::make(txn, ws, mis, collection,
                                                       PlanExecutor::YIELD_AUTO, &rawExec);
                invariant(execStatus.isOK());
                auto_ptr<PlanExecutor> curExec(rawExec);

                // The PlanExecutor was registered on construction due to the YIELD_AUTO policy.
                // We have to deregister it, as it will be registered with ClientCursor.
                curExec->deregisterExec();

                // Need to save state while yielding locks between now and getMore().
                curExec->saveState();

                execs.push_back(curExec.release());
            }

            // transfer iterators to executors using a round-robin distribution.
            // TODO consider using a common work queue once invalidation issues go away.
            for (size_t i = 0; i < iterators.size(); i++) {
                PlanExecutor* theExec = execs[i % execs.size()];
                MultiIteratorStage* mis = static_cast<MultiIteratorStage*>(theExec->getRootStage());

                // This wasn't called above as they weren't assigned yet
                iterators[i]->saveState();

                mis->addIterator(iterators.releaseAt(i));
            }

            {
                BSONArrayBuilder bucketsBuilder;
                for (size_t i = 0; i < execs.size(); i++) {
                    // transfer ownership of an executor to the ClientCursor (which manages its own
                    // lifetime).
                    ClientCursor* cc = new ClientCursor( collection->getCursorManager(),
                                                         execs.releaseAt(i),
                                                         ns.ns() );

                    BSONObjBuilder threadResult;
                    appendCursorResponseObject( cc->cursorid(),
                                                ns.ns(),
                                                BSONArray(),
                                                &threadResult );
                    threadResult.appendBool( "ok", 1 );

                    bucketsBuilder.append( threadResult.obj() );
                }
                result.appendArray( "cursors", bucketsBuilder.obj() );
            }

            return true;

        }
    } parallelCollectionScanCmd;

}
