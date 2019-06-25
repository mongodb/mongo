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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

class CollectionStats : public BasicCommand {
public:
    CollectionStats() : BasicCommand("collStats", "collstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbname, cmdObj).ns();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    void addRequiredPrivileges(const std::string& dbname,
                               const BSONObj& cmdObj,
                               std::vector<Privilege>* out) const override {
        ActionSet actions;
        actions.addAction(ActionType::collStats);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    bool run(OperationContext* opCtx,
             const std::string& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        const NamespaceString nss(parseNs(dbName, cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        if (routingInfo.cm()) {
            result.appendBool("sharded", true);
        } else {
            result.appendBool("sharded", false);
            result.append("primary", routingInfo.db().primaryId().toString());
        }

        auto shardResults = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            routingInfo,
            CommandHelpers::filterCommandRequestForPassthrough(cmdObj),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent,
            {},
            {});

        BSONObjBuilder shardStats;
        std::map<std::string, long long> counts;
        std::map<std::string, long long> indexSizes;

        long long maxSize = 0;
        long long unscaledCollSize = 0;

        int nindexes = 0;
        bool warnedAboutIndexes = false;

        for (const auto& shardResult : shardResults) {
            const auto& shardId = shardResult.shardId;
            const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
            uassertStatusOK(shardResponse.status);

            const auto& res = shardResponse.data;
            uassertStatusOK(getStatusFromCommandResult(res));

            // We don't know the order that we will encounter the count and size, so we save them
            // until we've iterated through all the fields before updating unscaledCollSize
            const auto shardObjCount = static_cast<long long>(res["count"].Number());

            auto fieldIsAnyOf = [](StringData v, std::initializer_list<StringData> il) {
                auto ei = il.end();
                return std::find(il.begin(), ei, v) != ei;
            };
            for (const auto& e : res) {
                StringData fieldName = e.fieldNameStringData();
                if (fieldIsAnyOf(fieldName, {"ns", "ok", "lastExtentSize", "paddingFactor"})) {
                    continue;
                }
                if (fieldIsAnyOf(fieldName,
                                 {"userFlags",
                                  "capped",
                                  "max",
                                  "paddingFactorNote",
                                  "indexDetails",
                                  "wiredTiger"})) {
                    // Fields that are copied from the first shard only, because they need to
                    // match across shards
                    if (!result.hasField(e.fieldName()))
                        result.append(e);
                } else if (fieldIsAnyOf(
                               fieldName,
                               {"count", "size", "storageSize", "totalIndexSize", "totalSize"})) {
                    counts[e.fieldName()] += e.numberLong();
                } else if (fieldName == "avgObjSize") {
                    const auto shardAvgObjSize = e.numberLong();
                    unscaledCollSize += shardAvgObjSize * shardObjCount;
                } else if (fieldName == "maxSize") {
                    const auto shardMaxSize = e.numberLong();
                    maxSize = std::max(maxSize, shardMaxSize);
                } else if (fieldName == "indexSizes") {
                    BSONObjIterator k(e.Obj());
                    while (k.more()) {
                        BSONElement temp = k.next();
                        indexSizes[temp.fieldName()] += temp.numberLong();
                    }
                } else if (fieldName == "nindexes") {
                    int myIndexes = e.numberInt();

                    if (nindexes == 0) {
                        nindexes = myIndexes;
                    } else if (nindexes == myIndexes) {
                        // no-op
                    } else {
                        // hopefully this means we're building an index

                        if (myIndexes > nindexes)
                            nindexes = myIndexes;

                        if (!warnedAboutIndexes) {
                            result.append("warning",
                                          "indexes don't all match - ok if ensureIndex is running");
                            warnedAboutIndexes = true;
                        }
                    }
                } else {
                    log() << "mongos collstats doesn't know about: " << e.fieldName();
                }
            }

            shardStats.append(shardId.toString(), res);
        }

        result.append("ns", nss.ns());

        for (const auto& countEntry : counts) {
            result.appendNumber(countEntry.first, countEntry.second);
        }

        {
            BSONObjBuilder ib(result.subobjStart("indexSizes"));
            for (const auto& entry : indexSizes) {
                ib.appendNumber(entry.first, entry.second);
            }
            ib.done();
        }

        // The unscaled avgObjSize for each shard is used to get the unscaledCollSize because the
        // raw size returned by the shard is affected by the command's scale parameter
        if (counts["count"] > 0)
            result.append("avgObjSize", (double)unscaledCollSize / (double)counts["count"]);
        else
            result.append("avgObjSize", 0.0);

        result.append("maxSize", maxSize);
        result.append("nindexes", nindexes);
        result.append("nchunks", (routingInfo.cm() ? routingInfo.cm()->numChunks() : 1));
        result.append("shards", shardStats.obj());

        return true;
    }

} collectionStatsCmd;

}  // namespace
}  // namespace mongo
