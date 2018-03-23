/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/create_collection_gen.h"
#include "mongo/util/log.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;

namespace {

/**
 * Internal sharding command run on config servers to create a new collection with unassigned shard
 * key. Call with { _configsvrCreateCollection: <string collName>, <other create options ...> }
 */
class ConfigSvrCreateCollectionCommand : public BasicCommand {
public:
    ConfigSvrCreateCollectionCommand() : BasicCommand("_configsvrCreateCollection") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return "Internal command, which is exported by the sharding config server. Do not call "
               "directly. Create a collection.";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forClusterResource(), ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsFullyQualified(cmdObj);
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {

        if (serverGlobalParams.clusterRole != ClusterRole::ConfigServer) {
            return CommandHelpers::appendCommandStatus(
                result,
                Status(ErrorCodes::IllegalOperation,
                       "_configsvrCreateCollection can only be run on config servers"));
        }

        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "createCollection must be called with majority writeConcern, got "
                              << cmdObj,
                opCtx->getWriteConcern().wMode == WriteConcernOptions::kMajority);

        auto createCmd = ConfigsvrCreateCollection::parse(
            IDLParserErrorContext("ConfigsvrCreateCollection"), cmdObj);

        CollectionOptions options;
        if (auto requestOptions = createCmd.getOptions()) {
            uassertStatusOK(options.parse(*requestOptions));
        }

        auto const catalogClient = Grid::get(opCtx)->catalogClient();
        const NamespaceString nss(parseNs(dbname_unused, cmdObj));

        auto dbDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            opCtx, nss.db(), "createCollection", DistLockManager::kDefaultLockTimeout));
        auto collDistLock = uassertStatusOK(catalogClient->getDistLockManager()->lock(
            opCtx, nss.ns(), "createCollection", DistLockManager::kDefaultLockTimeout));

        ShardingCatalogManager::get(opCtx)->createCollection(opCtx, createCmd.getNs(), options);

        return true;
    }

} configsvrCreateCollectionCmd;

}  // namespace
}  // namespace mongo
