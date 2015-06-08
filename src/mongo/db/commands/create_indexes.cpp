// create_indexes.cpp

/**
*    Copyright (C) 2013 MongoDB Inc.
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

#include <string>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/s/d_state.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

    using std::string;

    /**
     * { createIndexes : "bar", indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ] }
     */
    class CmdCreateIndex : public Command {
    public:
        CmdCreateIndex() : Command( "createIndexes" ){}

        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool slaveOk() const { return false; } // TODO: this could be made true...

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            ActionSet actions;
            actions.addAction(ActionType::createIndex);
            Privilege p(parseResourcePattern(dbname, cmdObj), actions);
            if (AuthorizationSession::get(client)->isAuthorizedForPrivilege(p))
                return Status::OK();
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }


        BSONObj _addNsToSpec( const NamespaceString& ns, const BSONObj& obj ) {
            BSONObjBuilder b;
            b.append( "ns", ns );
            b.appendElements( obj );
            return b.obj();
        }

        virtual bool run(OperationContext* txn,  const string& dbname, BSONObj& cmdObj, int options,
                          string& errmsg, BSONObjBuilder& result) {
            // ---  parse

            NamespaceString ns( dbname, cmdObj[name].String() );
            Status status = userAllowedWriteNS( ns );
            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            if ( cmdObj["indexes"].type() != Array ) {
                errmsg = "indexes has to be an array";
                result.append( "cmdObj", cmdObj );
                return false;
            }

            std::vector<BSONObj> specs;
            {
                BSONObjIterator i( cmdObj["indexes"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() != Object ) {
                        errmsg = "everything in indexes has to be an Object";
                        result.append( "cmdObj", cmdObj );
                        return false;
                    }
                    specs.push_back( e.Obj() );
                }
            }

            if ( specs.size() == 0 ) {
                errmsg = "no indexes to add";
                return false;
            }

            // check specs
            for ( size_t i = 0; i < specs.size(); i++ ) {
                BSONObj spec = specs[i];
                if ( spec["ns"].eoo() ) {
                    spec = _addNsToSpec( ns, spec );
                    specs[i] = spec;
                }

                if ( spec["ns"].type() != String ) {
                    errmsg = "spec has no ns";
                    result.append( "spec", spec );
                    return false;
                }
                if ( ns != spec["ns"].String() ) {
                    errmsg = "namespace mismatch";
                    result.append( "spec", spec );
                    return false;
                }
            }

            // now we know we have to create index(es)
            // Note: createIndexes command does not currently respect shard versioning.
            ScopedTransaction transaction(txn, MODE_IX);
            Lock::DBLock dbLock(txn->lockState(), ns.db(), MODE_X);
            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
                return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                    << "Not primary while creating indexes in " << ns.ns()));
            }

            Database* db = dbHolder().get(txn, ns.db());
            if (!db) {
                db = dbHolder().openDb(txn, ns.db());
            }

            Collection* collection = db->getCollection( ns.ns() );
            result.appendBool( "createdCollectionAutomatically", collection == NULL );
            if ( !collection ) {
                MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                    WriteUnitOfWork wunit(txn);
                    collection = db->createCollection(txn, ns.ns(), CollectionOptions());
                    invariant( collection );
                    wunit.commit();
                } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());
            }

            const int numIndexesBefore = collection->getIndexCatalog()->numIndexesTotal(txn);
            result.append("numIndexesBefore", numIndexesBefore);

            MultiIndexBlock indexer(txn, collection);
            indexer.allowBackgroundBuilding();
            indexer.allowInterruption();

            const size_t origSpecsSize = specs.size();
            indexer.removeExistingIndexes(&specs);

            if (specs.size() == 0) {
                result.append("numIndexesAfter", numIndexesBefore);
                result.append( "note", "all indexes already exist" );
                return true;
            }

            if (specs.size() != origSpecsSize) {
                result.append( "note", "index already exists" );
            }

            for ( size_t i = 0; i < specs.size(); i++ ) {
                const BSONObj& spec = specs[i];
                if ( spec["unique"].trueValue() ) {
                    status = checkUniqueIndexConstraints(txn, ns.ns(), spec["key"].Obj());

                    if ( !status.isOK() ) {
                        appendCommandStatus( result, status );
                        return false;
                    }
                }
            }

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                uassertStatusOK(indexer.init(specs));
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());

            // If we're a background index, replace exclusive db lock with an intent lock, so that
            // other readers and writers can proceed during this phase.  
            if (indexer.getBuildInBackground()) {
                txn->recoveryUnit()->abandonSnapshot();
                dbLock.relockWithMode(MODE_IX);
                if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
                    return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                        << "Not primary while creating background indexes in " << ns.ns()));
                }
            }

            try {
                Lock::CollectionLock colLock(txn->lockState(), ns.ns(), MODE_IX);
                uassertStatusOK(indexer.insertAllDocumentsInCollection());
            }
            catch (const DBException& e) {
                invariant(e.getCode() != ErrorCodes::WriteConflict);
                // Must have exclusive DB lock before we clean up the index build via the
                // destructor of 'indexer'.
                if (indexer.getBuildInBackground()) {
                    try {
                        // This function cannot throw today, but we will preemptively prepare for
                        // that day, to avoid data corruption due to lack of index cleanup.
                        txn->recoveryUnit()->abandonSnapshot();
                        dbLock.relockWithMode(MODE_X);
                        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
                            return appendCommandStatus(
                                result,
                                Status(ErrorCodes::NotMaster, str::stream()
                                    << "Not primary while creating background indexes in "
                                    << ns.ns() << ": cleaning up index build failure due to "
                                    << e.toString()));
                        }
                    }
                    catch (...) {
                        std::terminate();
                    }
                }
                throw;
            }
            // Need to return db lock back to exclusive, to complete the index build.
            if (indexer.getBuildInBackground()) {
                txn->recoveryUnit()->abandonSnapshot();
                dbLock.relockWithMode(MODE_X);
                uassert(ErrorCodes::NotMaster,
                        str::stream() << "Not primary while completing index build in " << dbname,
                        repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns));

                Database* db = dbHolder().get(txn, ns.db());
                uassert(28551, "database dropped during index build", db);
                uassert(28552, "collection dropped during index build",
                        db->getCollection(ns.ns()));
            }

            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                WriteUnitOfWork wunit(txn);

                indexer.commit();

                for ( size_t i = 0; i < specs.size(); i++ ) {
                    std::string systemIndexes = ns.getSystemIndexesCollection();
                    getGlobalServiceContext()->getOpObserver()->onCreateIndex(txn,
                                                                           systemIndexes,
                                                                           specs[i]);
                }

                wunit.commit();
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createIndexes", ns.ns());

            result.append( "numIndexesAfter", collection->getIndexCatalog()->numIndexesTotal(txn) );

            return true;
        }

    private:
        static Status checkUniqueIndexConstraints(OperationContext* txn,
                                                  StringData ns,
                                                  const BSONObj& newIdxKey) {

            invariant(txn->lockState()->isCollectionLockedForMode(ns, MODE_X));

            if ( shardingState.enabled() ) {
                CollectionMetadataPtr metadata(
                        shardingState.getCollectionMetadata( ns.toString() ));

                if ( metadata ) {
                    ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
                    if (!shardKeyPattern.isUniqueIndexCompatible(newIdxKey)) {
                        return Status(ErrorCodes::CannotCreateIndex,
                            str::stream() << "cannot create unique index over " << newIdxKey
                                          << " with shard key pattern "
                                          << shardKeyPattern.toBSON());
                    }
                }
            }

            return Status::OK();
        }

    } cmdCreateIndex;

}
