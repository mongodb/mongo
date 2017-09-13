/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include <set>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;

namespace {

/**
 * Internal sharding command run on config servers to enable sharding on a database.
 */
class ConfigSvrEnableShardingCommand : public BasicCommand {
public:
    ConfigSvrEnableShardingCommand() : BasicCommand("_configsvrEnableSharding") {}

    virtual bool slaveOk() const {
        return false;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(std::stringstream& help) const override {
        help << "Internal command, which is exported by the sharding config server. Do not call "
                "directly. Enable sharding on a database.";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        return cmdObj.firstElement().str();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) {

        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            return appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation,
                       "_configsvrEnableSharding can only be run on config servers"));
        }

        const std::string dbname = parseNs("", cmdObj);

        uassert(
            ErrorCodes::InvalidNamespace,
            str::stream() << "invalid db name specified: " << dbname,
            NamespaceString::validDBName(dbname, NamespaceString::DollarInDbNameBehavior::Allow));

        if (dbname == NamespaceString::kAdminDb || dbname == NamespaceString::kConfigDb ||
            dbname == NamespaceString::kLocalDb) {
            return appendCommandStatus(result,
                                       {ErrorCodes::InvalidOptions,
                                        str::stream() << "can't shard " + dbname + " database"});
        }

        // Make sure to force update of any stale metadata
        ON_BLOCK_EXIT([opCtx, dbname] { Grid::get(opCtx)->catalogCache()->purgeDatabase(dbname); });

        uassertStatusOK(ShardingCatalogManager::get(opCtx)->enableSharding(opCtx, dbname));
        audit::logEnableSharding(Client::getCurrent(), dbname);

        return true;
    }

} configsvrEnableShardingCmd;

}  // namespace
}  // namespace mongo
