/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using std::string;

namespace {

class MoveDatabasePrimaryCommand : public BasicCommand {
public:
    MoveDatabasePrimaryCommand() : BasicCommand("movePrimary", "moveprimary") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    virtual bool adminOnly() const {
        return true;
    }

    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return true;
    }

    std::string help() const override {
        return " example: { moveprimary : 'foo' , to : 'localhost:9999' }";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
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
        auto request = MovePrimary::parse(IDLParserContext("MovePrimary"), cmdObj);

        const string db = parseNs("", cmdObj);
        const StringData toShard(request.getTo());

        // Invalidate the routing table cache entry for this database so that we reload the
        // collection the next time it's accessed, even if we receive a failure, e.g. NetworkError.
        ON_BLOCK_EXIT([opCtx, db] { Grid::get(opCtx)->catalogCache()->purgeDatabase(db); });

        ShardMovePrimary movePrimaryRequest;
        movePrimaryRequest.set_shardsvrMovePrimary(NamespaceString(db));
        movePrimaryRequest.setTo(toShard);

        auto catalogCache = Grid::get(opCtx)->catalogCache();
        const auto dbInfo = uassertStatusOK(catalogCache->getDatabase(opCtx, db));

        auto cmdResponse = executeCommandAgainstDatabasePrimary(
            opCtx,
            "admin",
            dbInfo,
            CommandHelpers::appendMajorityWriteConcern(
                CommandHelpers::appendGenericCommandArgs(cmdObj, movePrimaryRequest.toBSON())),
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            Shard::RetryPolicy::kIdempotent);

        const auto remoteResponse = uassertStatusOK(cmdResponse.swResponse);
        CommandHelpers::filterCommandReplyForPassthrough(remoteResponse.data, &result);
        return true;
    }

} clusterMovePrimaryCmd;

}  // namespace
}  // namespace mongo
