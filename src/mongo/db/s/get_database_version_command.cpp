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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/s/request_types/get_database_version_gen.h"
#include "mongo/util/stringutils.h"

namespace mongo {
namespace {

class GetDatabaseVersion : public BasicCommand {
public:
    GetDatabaseVersion() : BasicCommand("getDatabaseVersion") {}

    std::string help() const override {
        return " example: { getDatabaseVersion : 'foo'  } ";
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const override {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(cmdObj.firstElement().str()),
                ActionType::getDatabaseVersion)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }
        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const std::string& dbname_unused,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << getName() << " can only be run on shard servers",
                serverGlobalParams.clusterRole == ClusterRole::ShardServer);

        const auto request = GetDatabaseVersionRequest::parse(
            IDLParserErrorContext("getDatabaseVersion command"), cmdObj);

        AutoGetDb autoDb(opCtx, request.getDatabaseVersion(), MODE_IS);
        if (!autoDb.getDb()) {
            result.append("dbVersion", BSONObj());
            return true;
        }

        auto const dbVersion = DatabaseShardingState::get(autoDb.getDb()).getDbVersion(opCtx);
        if (dbVersion) {
            result.append("dbVersion", dbVersion->toBSON());
        } else {
            result.append("dbVersion", BSONObj());
        }

        return true;
    }

} getDatabaseVersionCmd;

}  // namespace
}  // namespace mongo
