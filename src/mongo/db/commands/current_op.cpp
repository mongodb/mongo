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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <string>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/stats/fill_locker_info.h"
#include "mongo/util/log.h"

namespace mongo {

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

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::inprog);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    bool run(OperationContext* txn,
             const std::string& db,
             BSONObj& cmdObj,
             int options,
             std::string& errmsg,
             BSONObjBuilder& result) final {
        const bool includeAll = cmdObj["$all"].trueValue();

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
                }

                b.append(e);
            }
            filter = b.obj();
        }

        // We use ExtensionsCallbackReal here instead of ExtensionsCallbackNoop in order to support
        // the use case of having a $where filter with currentOp. However, since we don't have a
        // collection, we pass in a fake collection name (and this is okay, because $where parsing
        // only relies on the database part of the namespace).
        const NamespaceString fakeNS(db, "$cmd");
        const CollatorInterface* collator = nullptr;
        const Matcher matcher(filter, ExtensionsCallbackReal(txn, &fakeNS), collator);

        BSONArrayBuilder inprogBuilder(result.subarrayStart("inprog"));

        for (ServiceContext::LockedClientsCursor cursor(txn->getClient()->getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            stdx::lock_guard<Client> lk(*client);
            const OperationContext* opCtx = client->getOperationContext();

            if (!includeAll) {
                // Skip over inactive connections.
                if (!opCtx)
                    continue;
            }

            BSONObjBuilder infoBuilder;

            // The client information
            client->reportState(infoBuilder);

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

            infoBuilder.done();

            const BSONObj info = infoBuilder.obj();

            if (includeAll || matcher.matches(info)) {
                inprogBuilder.append(info);
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

}  // namespace mongo
