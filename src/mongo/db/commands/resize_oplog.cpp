/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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
namespace {

class CmdReplSetResizeOplog : public BasicCommand {
public:
    CmdReplSetResizeOplog() : BasicCommand("replSetResizeOplog") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const final {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "resize oplog size in MB";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::replSetResizeOplog)) {
            return Status::OK();
        }
        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        const NamespaceString nss("local", "oplog.rs");
        Lock::GlobalWrite global(opCtx);
        auto databaseHolder = DatabaseHolder::get(opCtx);
        auto database = databaseHolder->getDb(opCtx, nss.db());
        if (!database) {
            uasserted(ErrorCodes::NamespaceNotFound, "database local does not exist");
        }
        Collection* coll = database->getCollection(opCtx, nss);
        if (!coll) {
            uasserted(ErrorCodes::NamespaceNotFound, "oplog does not exist");
        }
        if (!coll->isCapped()) {
            uasserted(ErrorCodes::IllegalOperation, "oplog isn't capped");
        }
        if (!jsobj["size"].isNumber()) {
            uasserted(ErrorCodes::InvalidOptions, "invalid size field, size should be a number");
        }

        long long sizeMb = jsobj["size"].numberLong();
        if (sizeMb < 990L) {
            uasserted(ErrorCodes::InvalidOptions, "oplog size should be 990MB at least");
        }

        const long long kMB = 1024 * 1024;
        const long long kPB = kMB * 1024 * 1024 * 1024;
        if (sizeMb > kPB / kMB) {
            uasserted(ErrorCodes::InvalidOptions, "oplog size in MB cannot exceed maximum of 1PB");
        }
        long long size = sizeMb * kMB;

        WriteUnitOfWork wunit(opCtx);
        Status status = coll->getRecordStore()->updateCappedSize(opCtx, size);
        uassertStatusOK(status);
        CollectionCatalogEntry* entry = coll->getCatalogEntry();
        entry->updateCappedSize(opCtx, size);
        wunit.commit();
        LOG(0) << "replSetResizeOplog success, currentSize:" << size;
        return true;
    }

} cmdReplSetResizeOplog;

}  // namespace
}  // namespace mongo
