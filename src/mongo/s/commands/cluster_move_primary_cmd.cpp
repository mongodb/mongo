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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;

namespace {

class MoveDatabasePrimaryCommand : public BasicCommand {
public:
    MoveDatabasePrimaryCommand() : BasicCommand("movePrimary", "moveprimary") {}

    virtual bool slaveOk() const {
        return true;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    virtual void help(std::stringstream& help) const {
        help << " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        if (!AuthorizationSession::get(client)->isAuthorizedForActionsOnResource(
                ResourcePattern::forDatabaseName(parseNs(dbname, cmdObj)), ActionType::moveChunk)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        return Status::OK();
    }

    virtual std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const {
        const auto nsElt = cmdObj.firstElement();
        uassert(ErrorCodes::InvalidNamespace,
                "'movePrimary' must be of type String",
                nsElt.type() == BSONType::String);
        return nsElt.str();
    }

    virtual bool run(OperationContext* opCtx,
                     const std::string& dbname,
                     const BSONObj& cmdObj,
                     BSONObjBuilder& result) {

        auto movePrimaryRequest = MovePrimary::parse(IDLParserErrorContext("MovePrimary"), cmdObj);

        const string db = parseNs("", cmdObj);
        const NamespaceString nss(db);

        ConfigsvrMovePrimary configMovePrimaryRequest;
        configMovePrimaryRequest.set_configsvrMovePrimary(nss);
        configMovePrimaryRequest.setTo(movePrimaryRequest.getTo());

        // Invalidate the routing table cache entry for this database so that we reload the
        // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
        ON_BLOCK_EXIT([opCtx, db] { Grid::get(opCtx)->catalogCache()->purgeDatabase(db); });

        auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

        auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            "admin",
            Command::appendMajorityWriteConcern(
                Command::appendPassthroughFields(cmdObj, configMovePrimaryRequest.toBSON())),
            Shard::RetryPolicy::kIdempotent));

        Command::filterCommandReplyForPassthrough(cmdResponse.response, &result);
        return true;
    }

} clusterMovePrimaryCmd;

}  // namespace
}  // namespace mongo
