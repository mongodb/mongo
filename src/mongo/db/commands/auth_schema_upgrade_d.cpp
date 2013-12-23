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

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands/user_management_commands.h"

namespace mongo {
namespace {

    class CmdAuthSchemaUpgradeD : public CmdAuthSchemaUpgrade {
        virtual bool run(
                const string& dbname,
                BSONObj& cmdObj,
                int options,
                string& errmsg,
                BSONObjBuilder& result,
                bool fromRepl) {

            int maxSteps;
            bool upgradeShardServers;
            BSONObj writeConcern;
            Status status = auth::parseAuthSchemaUpgradeStepCommand(
                    cmdObj,
                    dbname,
                    &maxSteps,
                    &upgradeShardServers,
                    &writeConcern);
            if (!status.isOK()) {
                return appendCommandStatus(result, status);
            }

            AuthorizationManager* authzManager = getGlobalAuthorizationManager();

            AuthzDocumentsUpdateGuard updateGuard(authzManager);
            if (!updateGuard.tryLock("auth schema upgrade")) {
                return appendCommandStatus(
                        result,
                        Status(ErrorCodes::LockBusy, "Could not lock auth data update lock."));
            }

            status = authzManager->upgradeSchema(maxSteps, writeConcern);
            if (status.isOK())
                result.append("done", true);
            return appendCommandStatus(result, status);
        }

    } cmdAuthSchemaUpgradeStep;

}  // namespace
}  // namespace mongo
