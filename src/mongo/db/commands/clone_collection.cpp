/**
*    Copyright (C) 2008 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/cloner.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/copydb.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/db.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/isself.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

using std::unique_ptr;
using std::string;
using std::stringstream;
using std::endl;

class CmdCloneCollection : public Command {
public:
    CmdCloneCollection() : Command("cloneCollection") {}

    virtual bool slaveOk() const {
        return false;
    }


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return parseNsFullyQualified(dbname, cmdObj);
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        std::string ns = parseNs(dbname, cmdObj);

        ActionSet actions;
        actions.addAction(ActionType::insert);
        actions.addAction(ActionType::createIndex);  // SERVER-11418
        if (shouldBypassDocumentValidationForCommand(cmdObj)) {
            actions.addAction(ActionType::bypassDocumentValidation);
        }

        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(ns)), actions)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    virtual void help(stringstream& help) const {
        help << "{ cloneCollection: <collection>, from: <host> [,query: <query_filter>] "
                "[,copyIndexes:<bool>] }"
                "\nCopies a collection from one server to another. Do not use on a single server "
                "as the destination "
                "is placed at the same db.collection (namespace) as the source.\n";
    }

    virtual bool run(OperationContext* txn,
                     const string& dbname,
                     BSONObj& cmdObj,
                     int,
                     string& errmsg,
                     BSONObjBuilder& result) {
        boost::optional<DisableDocumentValidation> maybeDisableValidation;
        if (shouldBypassDocumentValidationForCommand(cmdObj))
            maybeDisableValidation.emplace(txn);

        string fromhost = cmdObj.getStringField("from");
        if (fromhost.empty()) {
            errmsg = "missing 'from' parameter";
            return false;
        }

        {
            HostAndPort h(fromhost);
            if (repl::isSelf(h, txn->getServiceContext())) {
                errmsg = "can't cloneCollection from self";
                return false;
            }
        }

        string collection = parseNs(dbname, cmdObj);
        Status allowedWriteStatus = userAllowedWriteNS(dbname, collection);
        if (!allowedWriteStatus.isOK()) {
            return appendCommandStatus(result, allowedWriteStatus);
        }

        BSONObj query = cmdObj.getObjectField("query");
        if (query.isEmpty())
            query = BSONObj();

        BSONElement copyIndexesSpec = cmdObj.getField("copyindexes");
        bool copyIndexes = copyIndexesSpec.isBoolean() ? copyIndexesSpec.boolean() : true;

        log() << "cloneCollection.  db:" << dbname << " collection:" << collection
              << " from: " << fromhost << " query: " << query << " "
              << (copyIndexes ? "" : ", not copying indexes") << endl;

        Cloner cloner;
        unique_ptr<DBClientConnection> myconn;
        myconn.reset(new DBClientConnection());
        if (!myconn->connect(HostAndPort(fromhost), errmsg))
            return false;

        cloner.setConnection(myconn.release());

        return cloner.copyCollection(txn, collection, query, errmsg, copyIndexes);
    }

} cmdCloneCollection;

}  // namespace mongo
