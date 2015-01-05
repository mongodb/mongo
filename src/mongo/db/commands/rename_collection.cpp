// rename_collection.cpp

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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    class CmdRenameCollection : public Command {
    public:
        CmdRenameCollection() : Command( "renameCollection" ) {}
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
        }
        virtual void help( stringstream &help ) const {
            help << " example: { renameCollection: foo.a, to: bar.b }";
        }

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = source;
            std::vector<BSONObj> prelim = 
                IndexBuilder::killMatchingIndexBuilds(db->getCollection(source), criteria);

            std::vector<BSONObj> indexes;

            for (int i = 0; i < static_cast<int>(prelim.size()); i++) {
                // Change the ns
                BSONObj stripped = prelim[i].removeField("ns");
                BSONObjBuilder builder;
                builder.appendElements(stripped);
                builder.append("ns", target);
                indexes.push_back(builder.obj());
            }

            return indexes;
        }

        static void dropCollection(OperationContext* txn, Database* db, StringData collName) {
            WriteUnitOfWork wunit(txn);
            if (db->dropCollection(txn, collName).isOK()) {
                // ignoring failure case
                wunit.commit();
            }
        }

        virtual bool run(OperationContext* txn,
                         const string& dbname,
                         BSONObj& cmdObj,
                         int,
                         string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl) {
            ScopedTransaction transaction(txn, MODE_X);
            Lock::GlobalWrite globalWriteLock(txn->lockState());
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

            // We stay in source context the whole time. This is mostly to set the CurOp namespace.
            Client::Context ctx(txn, source);

            if ( !NamespaceString::validCollectionComponent(target.c_str()) ) {
                errmsg = "invalid collection name: " + target;
                return false;
            }
            if ( source.empty() || target.empty() ) {
                errmsg = "invalid command syntax";
                return false;
            }

            if ((repl::getGlobalReplicationCoordinator()->getReplicationMode() != 
                 repl::ReplicationCoordinator::modeNone)) {
                if (NamespaceString(source).isOplog()) {
                    errmsg = "can't rename live oplog while replicating";
                    return false;
                }
                if (NamespaceString(target).isOplog()) {
                    errmsg = "can't rename to live oplog while replicating";
                    return false;
                }
            }

            if (NamespaceString::oplog(source) != NamespaceString::oplog(target)) {
                errmsg =
                    "If either the source or target of a rename is an oplog name, both must be";
                return false;
            }

            if (!fromRepl) { // If it got through on the master, need to allow it here too
                Status sourceStatus = userAllowedWriteNS(source);
                if (!sourceStatus.isOK()) {
                    errmsg = "error with source namespace: " + sourceStatus.reason();
                    return false;
                }

                Status targetStatus = userAllowedWriteNS(target);
                if (!targetStatus.isOK()) {
                    errmsg = "error with target namespace: " + targetStatus.reason();
                    return false;
                }
            }

            if (NamespaceString(source).coll() == "system.indexes"
                || NamespaceString(target).coll() == "system.indexes") {
                errmsg = "renaming system.indexes is not allowed";
                return false;
            }

            Database* const sourceDB = dbHolder().get(txn, nsToDatabase(source));
            Collection* const sourceColl = sourceDB ? sourceDB->getCollection(source)
                                                    : NULL;
            if (!sourceColl) {
                errmsg = "source namespace does not exist";
                return false;
            }

            {
                // Ensure that collection name does not exceed maximum length.
                // Ensure that index names do not push the length over the max.
                // Iterator includes unfinished indexes.
                IndexCatalog::IndexIterator sourceIndIt =
                    sourceColl->getIndexCatalog()->getIndexIterator( txn, true );
                int longestIndexNameLength = 0;
                while ( sourceIndIt.more() ) {
                    int thisLength = sourceIndIt.next()->indexName().length();
                    if ( thisLength > longestIndexNameLength )
                        longestIndexNameLength = thisLength;
                }

                unsigned int longestAllowed =
                    min(int(NamespaceString::MaxNsCollectionLen),
                        int(NamespaceString::MaxNsLen) - 2/*strlen(".$")*/ - longestIndexNameLength);
                if (target.size() > longestAllowed) {
                    StringBuilder sb;
                    sb << "collection name length of " << target.size()
                       << " exceeds maximum length of " << longestAllowed
                       << ", allowing for index names";
                    errmsg = sb.str();
                    return false;
                }
            }

            const std::vector<BSONObj> indexesInProg = stopIndexBuilds(txn, sourceDB, cmdObj);
            // Dismissed on success
            ScopeGuard indexBuildRestorer = MakeGuard(IndexBuilder::restoreIndexes,
                                                      txn,
                                                      indexesInProg);

            Database* const targetDB = dbHolder().openDb(txn, nsToDatabase(target));

            {
                WriteUnitOfWork wunit(txn);

                // Check if the target namespace exists and if dropTarget is true.
                // If target exists and dropTarget is not true, return false.
                if (targetDB->getCollection(target)) {
                    if (!cmdObj["dropTarget"].trueValue()) {
                        errmsg = "target namespace exists";
                        return false;
                    }

                    Status s = targetDB->dropCollection(txn, target);
                    if ( !s.isOK() ) {
                        errmsg = s.toString();
                        return false;
                    }
                }

                // If we are renaming in the same database, just
                // rename the namespace and we're done.
                if (sourceDB == targetDB) {
                    Status s = targetDB->renameCollection(txn,
                                                          source,
                                                          target,
                                                          cmdObj["stayTemp"].trueValue() );
                    if (!s.isOK()) {
                        return appendCommandStatus(result, s);
                    }

                    if (!fromRepl) {
                        repl::logOp(txn, "c", (dbname + ".$cmd").c_str(), cmdObj);
                    }

                    wunit.commit();
                    indexBuildRestorer.Dismiss();
                    return true;
                }

                wunit.commit();
            }

            // If we get here, we are renaming across databases, so we must copy all the data and
            // indexes, then remove the source collection.

            // Create the target collection. It will be removed if we fail to copy the collection.
            // TODO use a temp collection and unset the temp flag on success.
            Collection* targetColl = NULL;
            {
                CollectionOptions options;
                options.setNoIdIndex();

                if (sourceColl->isCapped()) {
                    const CollectionOptions sourceOpts =
                        sourceColl->getCatalogEntry()->getCollectionOptions(txn);

                    options.capped = true;
                    options.cappedSize = sourceOpts.cappedSize;
                    options.cappedMaxDocs = sourceOpts.cappedMaxDocs;
                }

                WriteUnitOfWork wunit(txn);

                // No logOp necessary because the entire renameCollection command is one logOp.
                targetColl = targetDB->createCollection(txn, target, options);
                if (!targetColl) {
                    errmsg = "Failed to create target collection.";
                    return false;
                }

                wunit.commit();
            }

            // Dismissed on success
            ScopeGuard targetCollectionDropper = MakeGuard(dropCollection, txn, targetDB, target);

            MultiIndexBlock indexer(txn, targetColl);
            indexer.allowInterruption();

            // Copy the index descriptions from the source collection, adjusting the ns field.
            {
                std::vector<BSONObj> indexesToCopy;
                IndexCatalog::IndexIterator sourceIndIt =
                    sourceColl->getIndexCatalog()->getIndexIterator( txn, true );
                while (sourceIndIt.more()) {
                    const BSONObj currIndex = sourceIndIt.next()->infoObj();

                    // Process the source index.
                    BSONObjBuilder newIndex;
                    newIndex.append("ns", target);
                    newIndex.appendElementsUnique(currIndex);
                    indexesToCopy.push_back(newIndex.obj());
                }
                indexer.init(indexesToCopy);
            }

            {
                // Copy over all the data from source collection to target collection.
                boost::scoped_ptr<RecordIterator> sourceIt(sourceColl->getIterator(txn));
                while (!sourceIt->isEOF()) {
                    txn->checkForInterrupt();

                    const BSONObj obj = sourceColl->docFor(txn, sourceIt->getNext());

                    WriteUnitOfWork wunit(txn);
                    // No logOp necessary because the entire renameCollection command is one logOp.
                    Status status = targetColl->insertDocument(txn, obj, &indexer, true).getStatus();
                    if (!status.isOK())
                        return appendCommandStatus(result, status);
                    wunit.commit();
                }
            }

            Status status = indexer.doneInserting();
            if (!status.isOK())
                return appendCommandStatus(result, status);

            {
                // Getting here means we successfully built the target copy. We now remove the
                // source collection and finalize the rename.
                WriteUnitOfWork wunit(txn);

                Status status = sourceDB->dropCollection(txn, source);
                if (!status.isOK())
                    return appendCommandStatus(result, status);

                indexer.commit();

                if (!fromRepl) {
                    repl::logOp(txn, "c", (dbname + ".$cmd").c_str(), cmdObj);
                }

                wunit.commit();
            }

            indexBuildRestorer.Dismiss();
            targetCollectionDropper.Dismiss();
            return true;
        }
    } cmdrenamecollection;

}
