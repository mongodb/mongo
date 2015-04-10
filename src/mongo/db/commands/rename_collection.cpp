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
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    using std::min;
    using std::string;
    using std::stringstream;

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

        std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                             Database* db,
                                             const NamespaceString& source,
                                             const NamespaceString& target) {

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
            invariant(!fromRepl == txn->writesAreReplicated());
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

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

            return appendCommandStatus(result,
                                       renameCollection(txn,
                                                        NamespaceString(source),
                                                        NamespaceString(target),
                                                        cmdObj["dropTarget"].trueValue(),
                                                        cmdObj["stayTemp"].trueValue()));
        }
    } cmdrenamecollection;

} // namespace mongo
