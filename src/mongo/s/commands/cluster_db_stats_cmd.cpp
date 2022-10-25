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

#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

void aggregateResults(int scale,
                      const std::vector<AsyncRequestsSender::Response>& responses,
                      BSONObjBuilder& output) {
    long long collections = 0;
    long long views = 0;
    long long objects = 0;
    double unscaledDataSize = 0;
    double dataSize = 0;
    double storageSize = 0;
    double totalSize = 0;
    long long indexes = 0;
    double indexSize = 0;
    double fsUsedSize = 0;
    double fsTotalSize = 0;

    for (const auto& response : responses) {
        invariant(response.swResponse.getStatus().isOK());
        const BSONObj& b = response.swResponse.getValue().data;
        auto resp = DBStats::parse(IDLParserContext{"dbstats"}, b);

        collections += resp.getCollections();
        views += resp.getViews();
        objects += resp.getObjects();
        unscaledDataSize += resp.getAvgObjSize() * resp.getObjects();
        dataSize += resp.getDataSize();
        storageSize += resp.getStorageSize();
        totalSize += resp.getTotalSize();
        indexes += resp.getIndexes();
        indexSize += resp.getIndexSize();
        fsUsedSize += resp.getFsUsedSize().get_value_or(0);
        fsTotalSize += resp.getFsTotalSize().get_value_or(0);
    }

    output.appendNumber("collections", collections);
    output.appendNumber("views", views);
    output.appendNumber("objects", objects);

    // avgObjSize on mongod is not scaled based on the argument to db.stats(), so we use
    // unscaledDataSize here for consistency.  See SERVER-7347.
    output.appendNumber("avgObjSize", objects == 0 ? 0 : unscaledDataSize / double(objects));
    output.appendNumber("dataSize", dataSize);
    output.appendNumber("storageSize", storageSize);
    output.appendNumber("totalSize", totalSize);
    output.appendNumber("indexes", indexes);
    output.appendNumber("indexSize", indexSize);
    output.appendNumber("scaleFactor", scale);
    output.appendNumber("fsUsedSize", fsUsedSize);
    output.appendNumber("fsTotalSize", fsTotalSize);
}

class CmdDBStats final : public BasicCommandWithRequestParser<CmdDBStats> {
public:
    using Request = DBStatsCommand;
    using Reply = typename Request::Reply;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    bool supportsWriteConcern(const BSONObj&) const final {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbname,
                                 const BSONObj&) const final {
        auto as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname.db()),
                                                  ActionType::dbStats)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }
        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        const auto& cmd = requestParser.request();
        uassert(ErrorCodes::BadValue, "Scale must be greater than zero", cmd.getScale() > 0);

        auto shardResponses = scatterGatherUnversionedTargetAllShards(
            opCtx,
            dbName.db(),
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent);
        std::string errmsg;
        if (!appendRawResponses(opCtx, &errmsg, &output, shardResponses).responseOK) {
            uasserted(ErrorCodes::OperationFailed, errmsg);
        }

        output.append("db", dbName.db());
        aggregateResults(cmd.getScale(), shardResponses, output);
        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        DBStats::parse(IDLParserContext{"dbstats.reply"}, resultObj);
    }
} clusterDBStatsCmd;

}  // namespace
}  // namespace mongo
