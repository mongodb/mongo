/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/db/commands/current_op_common.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/commands/run_aggregate.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/stats/fill_locker_info.h"

namespace mongo {

class CurrentOpCommand final : public CurrentOpCommandBase {
    MONGO_DISALLOW_COPYING(CurrentOpCommand);

public:
    CurrentOpCommand() = default;

    Status checkAuthForCommand(Client* client,
                               const std::string& dbName,
                               const BSONObj& cmdObj) const final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::inprog)) {
            return Status::OK();
        }

        if (authzSession->isAuthenticated() && cmdObj["$ownOps"].trueValue()) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    virtual StatusWith<CursorResponse> runAggregation(
        OperationContext* opCtx, const AggregationRequest& request) const final {
        auto aggCmdObj = request.serializeToCommandObj().toBson();

        BSONObjBuilder responseBuilder;

        auto status = runAggregate(
            opCtx, request.getNamespaceString(), request, std::move(aggCmdObj), responseBuilder);

        if (!status.isOK()) {
            return status;
        }

        CommandHelpers::appendSimpleCommandStatus(responseBuilder, true);

        return CursorResponse::parseFromBSON(responseBuilder.obj());
    }

    virtual void appendToResponse(BSONObjBuilder* result) const final {
        if (lockedForWriting()) {
            result->append("fsyncLock", true);
            result->append("info",
                           "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }
    }

} currentOpCommand;

}  // namespace mongo
