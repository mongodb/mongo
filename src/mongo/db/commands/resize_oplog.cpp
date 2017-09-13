/**
*    Copyright (C) 2017 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::stringstream;

class CmdReplSetResizeOplog : public BasicCommand {
public:
    CmdReplSetResizeOplog() : BasicCommand("replSetResizeOplog") {}

    virtual bool slaveOk() const final {
        return true;
    }

    bool adminOnly() const final {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "resize oplog size in MB";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::replSetResizeOplog)) {
            return Status::OK();
        }
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss("local", "oplog.rs");
        Lock::GlobalWrite global(opCtx);
        Database* database = dbHolder().get(opCtx, nss.db());
        if (!database) {
            return appendCommandStatus(
                result, Status(ErrorCodes::NamespaceNotFound, "database local does not exist"));
        }
        Collection* coll = database->getCollection(opCtx, nss);
        if (!coll) {
            return appendCommandStatus(
                result, Status(ErrorCodes::NamespaceNotFound, "oplog does not exist"));
        }
        if (!coll->isCapped()) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::IllegalOperation, "oplog isn't capped"));
        }
        if (!jsobj["size"].isNumber()) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::InvalidOptions, "invalid size field, size should be a number"));
        }

        long long sizeMb = jsobj["size"].numberLong();
        long long size = sizeMb * 1024 * 1024;
        if (sizeMb < 990L) {
            return appendCommandStatus(
                result, Status(ErrorCodes::InvalidOptions, "oplog size should be 990MB at least"));
        }
        WriteUnitOfWork wunit(opCtx);
        Status status = coll->getRecordStore()->updateCappedSize(opCtx, size);
        if (!status.isOK()) {
            return appendCommandStatus(result, status);
        }
        CollectionCatalogEntry* entry = coll->getCatalogEntry();
        entry->updateCappedSize(opCtx, size);
        wunit.commit();
        LOG(0) << "replSetResizeOplog success, currentSize:" << size;
        return appendCommandStatus(result, Status::OK());
    }
} cmdReplSetResizeOplog;
}
