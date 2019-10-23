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
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/util/log.h"

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
        AutoGetCollection autoColl(opCtx, NamespaceString::kRsOplogNamespace, MODE_X);
        Database* database = autoColl.getDb();
        uassert(ErrorCodes::NamespaceNotFound, "database local does not exist", database);
        Collection* coll = autoColl.getCollection();
        uassert(ErrorCodes::NamespaceNotFound, "oplog does not exist", coll);
        uassert(ErrorCodes::IllegalOperation, "oplog isn't capped", coll->isCapped());
        uassert(ErrorCodes::InvalidOptions,
                "invalid size field, size should be a number",
                jsobj["size"].isNumber());

        long long sizeMb = jsobj["size"].numberLong();
        uassert(ErrorCodes::InvalidOptions, "oplog size should be 990MB at least", sizeMb >= 990L);

        const long long kMB = 1024 * 1024;
        const long long kPB = kMB * 1024 * 1024 * 1024;
        uassert(ErrorCodes::InvalidOptions,
                "oplog size in MB cannot exceed maximum of 1PB",
                sizeMb <= kPB / kMB);
        long long size = sizeMb * kMB;

        return writeConflictRetry(opCtx, "replSetResizeOplog", coll->ns().ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            Status status = coll->getRecordStore()->updateCappedSize(opCtx, size);
            uassertStatusOK(status);
            DurableCatalog::get(opCtx)->updateCappedSize(opCtx, coll->ns(), size);
            wunit.commit();
            LOG(0) << "replSetResizeOplog success, currentSize:" << size;
            return true;
        });
    }

} cmdReplSetResizeOplog;

}  // namespace
}  // namespace mongo
