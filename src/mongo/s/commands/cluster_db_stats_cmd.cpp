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

#include "mongo/db/commands.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

void aggregateResults(int scale,
                      const std::vector<AsyncRequestsSender::Response>& responses,
                      BSONObjBuilder& output) {
    long long objects = 0;
    long long unscaledDataSize = 0;
    long long dataSize = 0;
    long long storageSize = 0;
    long long totalSize = 0;
    long long indexes = 0;
    long long indexSize = 0;
    long long fileSize = 0;

    for (const auto& response : responses) {
        invariant(response.swResponse.getStatus().isOK());
        const BSONObj& b = response.swResponse.getValue().data;

        objects += b["objects"].numberLong();
        unscaledDataSize += b["avgObjSize"].numberLong() * b["objects"].numberLong();
        dataSize += b["dataSize"].numberLong();
        storageSize += b["storageSize"].numberLong();
        totalSize += b["totalSize"].numberLong();
        indexes += b["indexes"].numberLong();
        indexSize += b["indexSize"].numberLong();
        fileSize += b["fileSize"].numberLong();
    }

    output.appendNumber("objects", objects);

    // avgObjSize on mongod is not scaled based on the argument to db.stats(), so we use
    // unscaledDataSize here for consistency.  See SERVER-7347.
    output.append("avgObjSize", objects == 0 ? 0 : double(unscaledDataSize) / double(objects));
    output.appendNumber("dataSize", dataSize);
    output.appendNumber("storageSize", storageSize);
    output.appendNumber("totalSize", totalSize);
    output.appendNumber("indexes", indexes);
    output.appendNumber("indexSize", indexSize);
    output.appendNumber("scaleFactor", scale);
    output.appendNumber("fileSize", fileSize);
}

class DBStatsCmd : public ErrmsgCommandDeprecated {
public:
    DBStatsCmd() : ErrmsgCommandDeprecated("dbStats", "dbstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool maintenanceOk() const override {
        return false;
    }

    bool adminOnly() const override {
        return false;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::dbStats);
        out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbName,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& output) override {
        int scale = 1;
        if (cmdObj["scale"].isNumber()) {
            scale = cmdObj["scale"].numberInt();
            if (scale <= 0) {
                errmsg = "scale has to be > 0";
                return false;
            }
        } else if (cmdObj["scale"].trueValue()) {
            errmsg = "scale has to be a number > 0";
            return false;
        }

        auto shardResponses = scatterGatherUnversionedTargetAllShards(
            opCtx,
            dbName,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent);
        if (!appendRawResponses(opCtx, &errmsg, &output, shardResponses).responseOK) {
            return false;
        }

        aggregateResults(scale, shardResponses, output);
        return true;
    }

} clusterDBStatsCmd;

}  // namespace
}  // namespace mongo
