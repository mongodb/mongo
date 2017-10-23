/**
 * Copyright (C) 2013 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
Status _performNoopWrite(OperationContext* opCtx, BSONObj msgObj, StringData note) {
    repl::ReplicationCoordinator* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    // Use GlobalLock + lockMMAPV1Flush instead of DBLock to allow return when the lock is not
    // available. It may happen when the primary steps down and a shared global lock is
    // acquired.
    Lock::GlobalLock lock(opCtx, MODE_IX, 1);

    if (!lock.isLocked()) {
        LOG(1) << "Global lock is not available skipping noopWrite";
        return {ErrorCodes::LockFailed, "Global lock is not available"};
    }
    opCtx->lockState()->lockMMAPV1Flush();

    // Its a proxy for being a primary passing "local" will cause it to return true on secondary
    if (!replCoord->canAcceptWritesForDatabase(opCtx, "admin")) {
        return {ErrorCodes::NotMaster, "Not a primary"};
    }

    writeConflictRetry(opCtx, note, NamespaceString::kRsOplogNamespace.ns(), [&opCtx, &msgObj] {
        WriteUnitOfWork uow(opCtx);
        opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(opCtx, msgObj);
        uow.commit();
    });

    return Status::OK();
}
}  // namespace

using std::string;
using std::stringstream;

class AppendOplogNoteCmd : public BasicCommand {
public:
    AppendOplogNoteCmd() : BasicCommand("appendOplogNote") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(stringstream& help) const {
        help << "Adds a no-op entry to the oplog";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::appendOplogNote)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    virtual bool run(OperationContext* opCtx,
                     const string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->isReplEnabled()) {
            return appendCommandStatus(result,
                                       {ErrorCodes::NoReplicationEnabled,
                                        "Must have replication set up to run \"appendOplogNote\""});
        }

        BSONElement dataElement;
        auto dataStatus = bsonExtractTypedField(cmdObj, "data", Object, &dataElement);
        if (!dataStatus.isOK()) {
            return appendCommandStatus(result, dataStatus);
        }

        Timestamp maxClusterTime;
        auto maxClusterTimeStatus =
            bsonExtractTimestampField(cmdObj, "maxClusterTime", &maxClusterTime);

        if (!maxClusterTimeStatus.isOK()) {
            if (maxClusterTimeStatus == ErrorCodes::NoSuchKey) {  // no need to use maxClusterTime
                return appendCommandStatus(
                    result, _performNoopWrite(opCtx, dataElement.Obj(), "appendOpLogNote"));
            }
            return appendCommandStatus(result, maxClusterTimeStatus);
        }

        auto lastAppliedOpTime = replCoord->getMyLastAppliedOpTime().getTimestamp();
        if (maxClusterTime > lastAppliedOpTime) {
            return appendCommandStatus(
                result, _performNoopWrite(opCtx, dataElement.Obj(), "appendOpLogNote"));
        } else {
            std::stringstream ss;
            ss << "Requested maxClusterTime" << LogicalTime(maxClusterTime).toString()
               << " is less or equal to the last primary OpTime: "
               << LogicalTime(lastAppliedOpTime).toString();
            return appendCommandStatus(result, {ErrorCodes::StaleClusterTime, ss.str()});
        }
    }
};

MONGO_INITIALIZER(RegisterAppendOpLogNoteCmd)(InitializerContext* context) {
    new AppendOplogNoteCmd();
    return Status::OK();
}

}  // namespace mongo
