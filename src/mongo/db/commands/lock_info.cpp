/**
 * Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <map>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

using std::string;
using std::stringstream;

/**
 * Admin command to display global lock information
 */
class CmdLockInfo : public Command {
public:
    virtual bool slaveOk() const {
        return true;
    }

    virtual bool slaveOverrideOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "show all lock info on the server";
    }

    Status checkAuthForCommand(ClientBasic* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) final {
        bool isAuthorized = AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
            ResourcePattern::forClusterResource(), ActionType::serverStatus);
        return isAuthorized ? Status::OK() : Status(ErrorCodes::Unauthorized, "Unauthorized");
    }

    CmdLockInfo() : Command("lockInfo", true) {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        std::map<LockerId, BSONObj> lockToClientMap;

        for (ServiceContext::LockedClientsCursor cursor(txn->getClient()->getServiceContext());
             Client* client = cursor.next();) {
            invariant(client);

            stdx::lock_guard<Client> lk(*client);
            const OperationContext* opCtx = client->getOperationContext();

            // Operation context specific information
            if (opCtx) {
                BSONObjBuilder infoBuilder;
                // The client information
                client->reportState(infoBuilder);

                infoBuilder.append("opid", opCtx->getOpID());
                LockerId lockerId = opCtx->lockState()->getId();
                lockToClientMap.insert({lockerId, infoBuilder.obj()});
            }
        }

        getGlobalLockManager()->getLockInfoBSON(lockToClientMap, &result);
        return true;
    }
} cmdLockInfo;
}
