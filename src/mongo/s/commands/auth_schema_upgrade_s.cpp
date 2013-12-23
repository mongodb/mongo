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

#include <boost/scoped_ptr.hpp>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/authz_documents_update_guard.h"
#include "mongo/db/auth/user_management_commands_parser.h"
#include "mongo/db/commands/user_management_commands.h"
#include "mongo/s/cluster_client_internal.h"
#include "mongo/s/config.h"
#include "mongo/s/shard.h"
#include "mongo/s/type_shard.h"

namespace mongo {
namespace {

    /**
     * Returns the ConnectionStrings identifying all of the shards.
     */
    std::vector<ConnectionString> getShardConnectionStrings() {
        std::vector<Shard> allShards;
        Shard::getAllShards(allShards);

        std::vector<ConnectionString> result;
        for (size_t i = 0; i < allShards.size(); ++i) {
            result.push_back(allShards[i].getAddress());
        }
        return result;
    }

    /**
     * Runs the authSchemaUpgrade command on the given connection, with the supplied maxSteps
     * and writeConcern parameters.
     *
     * Used to upgrade individual shards.
     */
    Status runUpgradeOnConnection(DBClientBase* conn, int maxSteps, const BSONObj& writeConcern) {
        std::string errorMessage;
        BSONObj result;
        BSONObjBuilder cmdObjBuilder;
        cmdObjBuilder << "authSchemaUpgrade" << 1 << "maxSteps" << maxSteps;
        if (!writeConcern.isEmpty()) {
            cmdObjBuilder << "writeConcern" << writeConcern;
        }
        try {
            conn->runCommand(
                    "admin",
                    cmdObjBuilder.done(),
                    result);
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
        return Command::getStatusFromCommandResult(result);
    }

    /**
     * Runs the authSchemaUpgrade on all shards, with the given maxSteps and writeConcern
     * parameters.
     *
     * Upgrades each shard serially, and stops on first failure.  Returned error indicates that
     * failure.
     */
    Status runUpgradeOnAllShards(int maxSteps, const BSONObj& writeConcern) {
        std::vector<ConnectionString> shardServers;
        try {
            shardServers = getShardConnectionStrings();
        }
        catch (const DBException& ex) {
            return ex.toStatus();
        }
        // Upgrade each shard in turn, stopping on first failure.
        for (size_t i = 0; i < shardServers.size(); ++i) {
            std::string errorMessage;
            ScopedDbConnection shardConn(shardServers[i]);
            Status status = runUpgradeOnConnection(shardConn.get(), maxSteps, writeConcern);
            if (!status.isOK()) {
                return Status(
                        status.code(),
                        mongoutils::str::stream() << status.reason() << " on shard " <<
                        shardServers[i].toString());
            }
            shardConn.done();
        }
        return Status::OK();
    }

    class CmdAuthSchemaUpgradeS : public CmdAuthSchemaUpgrade {
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
            if (!status.isOK())
                return appendCommandStatus(result, status);

            if (upgradeShardServers) {
                status = runUpgradeOnAllShards(maxSteps, writeConcern);
                if (!status.isOK())
                    return appendCommandStatus(result, status);
            }
            result.append("done", true);
            return true;
        }

    } cmdAuthSchemaUpgradeStep;

}  // namespace
}  // namespace mongo
