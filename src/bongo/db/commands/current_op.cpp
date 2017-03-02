/**
 *    Copyright (C) 2015 BongoDB Inc.
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

#define BONGO_LOG_DEFAULT_COMPONENT ::bongo::logger::LogComponent::kCommand

#include "bongo/platform/basic.h"

#include <string>

#include "bongo/db/auth/action_type.h"
#include "bongo/db/auth/authorization_session.h"
#include "bongo/db/client.h"
#include "bongo/db/commands.h"
#include "bongo/db/commands/fsync.h"
#include "bongo/db/curop.h"
#include "bongo/db/dbmessage.h"
#include "bongo/db/jsobj.h"
#include "bongo/db/matcher/extensions_callback_real.h"
#include "bongo/db/matcher/matcher.h"
#include "bongo/db/namespace_string.h"
#include "bongo/db/operation_context.h"
#include "bongo/db/stats/fill_locker_info.h"
#include "bongo/rpc/metadata/client_metadata.h"
#include "bongo/rpc/metadata/client_metadata_ismaster.h"
#include "bongo/util/log.h"

namespace bongo {

class CurrentOpCommand : public Command {
public:
    CurrentOpCommand() : Command("currentOp") {}


    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    bool slaveOk() const final {
        return true;
    }

    bool adminOnly() const final {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::inprog)) {
            return Status::OK();
        }

        bool isAuthenticated = authzSession->getAuthenticatedUserNames().more();
        if (isAuthenticated && cmdObj["$ownOps"].trueValue()) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        const bool includeAll = cmdObj["$all"].trueValue();
        const bool ownOpsOnly = cmdObj["$ownOps"].trueValue();

        // Filter the output
        BSONObj filter;
        {
            BSONObjBuilder b;
            BSONObjIterator i(cmdObj);
            invariant(i.more());
            i.next();  // skip {currentOp: 1} which is required to be the first element
            while (i.more()) {
                BSONElement e = i.next();
                if (str::equals("$all", e.fieldName())) {
                    continue;
                } else if (str::equals("$ownOps", e.fieldName())) {
                    continue;
                }

                b.append(e);
            }
            filter = b.obj();
        }

        std::vector<BSONObj> inprogInfos;
        BSONArrayBuilder inprogBuilder(result.subarrayStart("inprog"));

        for (ServiceContext::LockedClientsCursor cursor(txn->getClient()->getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            stdx::lock_guard<Client> lk(*client);

            if (ownOpsOnly &&
                !AuthorizationSession::get(txn->getClient())->isCoauthorizedWithClient(client)) {
                continue;
            }

            const OperationContext* opCtx = client->getOperationContext();

            if (!includeAll) {
                // Skip over inactive connections.
                if (!opCtx)
                    continue;
            }

            BSONObjBuilder infoBuilder;

            // The client information
            client->reportState(infoBuilder);

            const auto& clientMetadata =
                ClientMetadataIsMasterState::get(client).getClientMetadata();
            if (clientMetadata) {
                auto appName = clientMetadata.get().getApplicationName();
                if (!appName.empty()) {
                    infoBuilder.append("appName", appName);
                }
            }

            // Operation context specific information
            infoBuilder.appendBool("active", static_cast<bool>(opCtx));
            if (opCtx) {
                infoBuilder.append("opid", opCtx->getOpID());
                if (opCtx->isKillPending()) {
                    infoBuilder.append("killPending", true);
                }

                CurOp::get(opCtx)->reportState(&infoBuilder);

                // LockState
                Locker::LockerInfo lockerInfo;
                opCtx->lockState()->getLockerInfo(&lockerInfo);
                fillLockerInfo(lockerInfo, infoBuilder);
            }

            // If we want to include all results or if the filter is empty, then we can append
            // straight to the inprogBuilder, but otherwise we should run the filter Matcher
            // outside this loop so we don't lock the ServiceContext while matching - in some cases
            // this can cause deadlocks.
            if (includeAll || filter.isEmpty()) {
                inprogBuilder.append(infoBuilder.obj());
            } else {
                inprogInfos.emplace_back(infoBuilder.obj());
            }
        }

        if (!inprogInfos.empty()) {
            // We use ExtensionsCallbackReal here instead of ExtensionsCallbackNoop in order to
            // support the use case of having a $where filter with currentOp. However, since we
            // don't have a collection, we pass in a fake collection name (and this is okay,
            // because $where parsing only relies on the database part of the namespace).
            const NamespaceString fakeNS(db, "$dummyNamespaceForCurrop");
            const Matcher matcher(filter, ExtensionsCallbackReal(txn, &fakeNS), nullptr);

            for (const auto& info : inprogInfos) {
                if (matcher.matches(info)) {
                    inprogBuilder.append(info);
                }
            }
        }
        inprogBuilder.done();

        if (lockedForWriting()) {
            result.append("fsyncLock", true);
            result.append("info",
                          "use db.fsyncUnlock() to terminate the fsync write/snapshot lock");
        }

        return true;
    }

} currentOpCommand;

}  // namespace bongo
