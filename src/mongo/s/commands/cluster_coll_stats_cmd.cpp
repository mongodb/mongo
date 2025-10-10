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


#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/timeseries/timeseries_commands_conversion_helper.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/collection_routing_info_targeter.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

Rarely _sampler;

auto fieldIsAnyOf = [](StringData v, std::initializer_list<StringData> il) {
    auto ei = il.end();
    return std::find(il.begin(), ei, v) != ei;
};

BSONObj scaleIndividualShardStatistics(const BSONObj& shardStats, int scale) {
    BSONObjBuilder builder;

    for (const auto& element : shardStats) {
        std::string fieldName = element.fieldName();

        if (fieldIsAnyOf(fieldName,
                         {"size", "maxSize", "storageSize", "totalIndexSize", "totalSize"})) {
            builder.appendNumber(fieldName, element.numberLong() / scale);
        } else if (fieldName == "scaleFactor") {
            // Explicitly change the scale factor as we removed the scaling before getting the
            // individual shards statistics.
            builder.appendNumber(fieldName, scale);
        } else if (fieldName == "indexSizes") {
            BSONObjBuilder indexSizesBuilder(builder.subobjStart(fieldName));
            for (const auto& entry : shardStats.getField(fieldName).Obj()) {
                indexSizesBuilder.appendNumber(entry.fieldName(), entry.numberLong() / scale);
            }
            indexSizesBuilder.done();
        } else {
            // All the other fields that do not require further scaling.
            builder.append(element);
        }
    }

    return builder.obj();
}

/**
 * Takes the shard's "shardTimeseriesStats" and adds it to the sum across shards saved in
 * "clusterTimeseriesStats". All of the mongod "timeseries" collStats are numbers except for the
 * "bucketsNs" field, which we specially track in "timeseriesBucketsNs". We also track
 * "timeseriesTotalBucketSize" specially for calculating "avgBucketSize".
 * "avgNumMeasurementsPerCommit" is specially calculated using "numMeasurementsCommitted" and
 * "numCommits"
 *
 * Invariants that "shardTimeseriesStats" is non-empty.
 */
void aggregateTimeseriesStats(const BSONObj& shardTimeseriesStats,
                              std::map<std::string, long long>* clusterTimeseriesStats,
                              std::string* timeseriesBucketsNs,
                              long long* timeseriesTotalBucketSize) {
    invariant(!shardTimeseriesStats.isEmpty());

    for (const auto& shardTimeseriesStat : shardTimeseriesStats) {
        // "bucketsNs" is the only timeseries stat that is not a number, so it requires special
        // handling.
        if (shardTimeseriesStat.type() == BSONType::String) {
            invariant(shardTimeseriesStat.fieldNameStringData() == "bucketsNs",
                      str::stream() << "Found an unexpected field '"
                                    << shardTimeseriesStat.fieldNameStringData()
                                    << "' in a shard's 'timeseries' subobject: "
                                    << shardTimeseriesStats.toString());
            if (timeseriesBucketsNs->empty()) {
                *timeseriesBucketsNs = shardTimeseriesStat.String();
            } else {
                // All shards should have the same timeseries buckets collection namespace.
                invariant(*timeseriesBucketsNs == shardTimeseriesStat.String(),
                          str::stream()
                              << "Found different timeseries buckets collection namespaces on "
                              << "different shards, for the same collection. Previous shard's ns: "
                              << *timeseriesBucketsNs
                              << ", current shard's ns: " << shardTimeseriesStat.String());
            }
        } else if (shardTimeseriesStat.fieldNameStringData() == "avgBucketSize") {
            // Special logic to handle average aggregation.
            tassert(5758901,
                    str::stream()
                        << "Cannot aggregate avgBucketSize when bucketCount field is not number.",
                    shardTimeseriesStats.getField("bucketCount").isNumber());
            *timeseriesTotalBucketSize +=
                shardTimeseriesStats.getField("bucketCount").numberLong() *
                shardTimeseriesStat.numberLong();
        } else {
            // Simple summation for other types of stats.
            // Use 'numberLong' to ensure integers are safely converted to long type.
            tassert(5758902,
                    str::stream() << "Index stats '" << shardTimeseriesStat.fieldName()
                                  << "' should be number.",
                    shardTimeseriesStat.isNumber());
            (*clusterTimeseriesStats)[shardTimeseriesStat.fieldName()] +=
                shardTimeseriesStat.numberLong();
        }
    }
    (*clusterTimeseriesStats)["avgBucketSize"] = (*clusterTimeseriesStats)["bucketCount"]
        ? *timeseriesTotalBucketSize / (*clusterTimeseriesStats)["bucketCount"]
        : 0;
    (*clusterTimeseriesStats)["avgNumMeasurementsPerCommit"] =
        (*clusterTimeseriesStats)["numCommits"]
        ? (*clusterTimeseriesStats)["numMeasurementsCommitted"] /
            (*clusterTimeseriesStats)["numCommits"]
        : 0;
}

/**
 * Adds a "timeseries" field to "result" that contains the summed timeseries statistics in
 * "clusterTimeseriesStats". "timeseriesBucketNs" is specially handled and added to the "timeseries"
 * sub-document because it is the only non-number timeseries statistic. "avgBucketSize" is also
 * calculated specially through the aggregated "timeseriesTotalBucketSize".
 *
 * Invariants that "clusterTimeseriesStats" and "timeseriesBucketNs" are set.
 */
void appendTimeseriesInfoToResult(const std::map<std::string, long long>& clusterTimeseriesStats,
                                  const std::string& timeseriesBucketNs,
                                  BSONObjBuilder* result) {
    invariant(!clusterTimeseriesStats.empty());
    invariant(!timeseriesBucketNs.empty());

    BSONObjBuilder timeseriesSubObjBuilder(result->subobjStart("timeseries"));
    timeseriesSubObjBuilder.append("bucketsNs", timeseriesBucketNs);
    for (const auto& statEntry : clusterTimeseriesStats) {
        timeseriesSubObjBuilder.appendNumber(statEntry.first, statEntry.second);
    }
    timeseriesSubObjBuilder.done();
}

class CollectionStats : public BasicCommand {
public:
    CollectionStats() : BasicCommand("collStats", "collstats") {}

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    bool maintenanceOk() const override {
        return false;
    }

    NamespaceString parseNs(const DatabaseName& dbName, const BSONObj& cmdObj) const override {
        return CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::collStats)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Status::OK();
    }

    bool run(OperationContext* opCtx,
             const DatabaseName& dbName,
             const BSONObj& cmdObj,
             BSONObjBuilder& result) override {
        if (_sampler.tick())
            LOGV2_WARNING(7024601,
                          "The collStats command is deprecated. For more information, see "
                          "https://dochub.mongodb.org/core/collStats-deprecated");

        const NamespaceString nss(parseNs(dbName, cmdObj));

        const auto targeter = CollectionRoutingInfoTargeter(opCtx, nss);
        const auto cri = targeter.getRoutingInfo();
        const auto& cm = cri.cm;
        if (cm.isSharded()) {
            result.appendBool("sharded", true);
        } else {
            result.appendBool("sharded", false);
            result.append("primary", cm.dbPrimary().toString());
        }

        int scale = 1;
        if (cmdObj["scale"].isNumber()) {
            scale = cmdObj["scale"].safeNumberInt();
            uassert(4390200, "scale has to be >= 1", scale >= 1);
        } else if (cmdObj["scale"].trueValue()) {
            uasserted(4390201, "scale has to be a number >= 1");
        }

        // Re-construct the command's BSONObj without any scaling to be applied.
        BSONObj cmdObjToSend = cmdObj.removeField("scale");

        // Translate command collection namespace for time-series collection.
        if (targeter.timeseriesNamespaceNeedsRewrite(nss)) {
            cmdObjToSend =
                timeseries::makeTimeseriesCommand(cmdObjToSend, nss, getName(), boost::none);
        }

        // Unscaled individual shard results. This is required to apply scaling after summing the
        // statistics from individual shards as opposed to adding the summation of the scaled
        // statistics.
        auto unscaledShardResults = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.dbName(),
            targeter.getNS(),
            cri,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObjToSend)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent,
            {} /*query*/,
            {} /*collation*/,
            boost::none /*letParameters*/,
            boost::none /*runtimeConstants*/);

        BSONObjBuilder shardStats;
        std::map<std::string, long long> counts;
        std::map<std::string, long long> indexSizes;
        std::map<std::string, long long> clusterTimeseriesStats;

        long long maxSize = 0;
        long long unscaledCollSize = 0;

        int nindexes = 0;
        bool warnedAboutIndexes = false;
        std::string timeseriesBucketsNs;
        long long timeseriesTotalBucketSize = 0;

        for (const auto& shardResult : unscaledShardResults) {
            const auto& shardId = shardResult.shardId;
            const auto shardResponse = uassertStatusOK(std::move(shardResult.swResponse));
            uassertStatusOK(shardResponse.status);

            const auto& res = shardResponse.data;
            uassertStatusOK(getStatusFromCommandResult(res));

            // We don't know the order that we will encounter the count and size, so we save them
            // until we've iterated through all the fields before updating unscaledCollSize
            // Timeseries bucket collection does not provide 'count' or 'avgObjSize'.
            BSONElement countField = res.getField("count");
            const auto shardObjCount =
                static_cast<long long>(!countField.eoo() ? countField.Number() : 0);

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
                } else if (fieldName == "timeseries") {
                    aggregateTimeseriesStats(e.Obj(),
                                             &clusterTimeseriesStats,
                                             &timeseriesBucketsNs,
                                             &timeseriesTotalBucketSize);
                } else if (fieldIsAnyOf(
                               fieldName,
                               {"count", "size", "storageSize", "totalIndexSize", "totalSize"})) {
                    counts[e.fieldName()] += e.numberLong();
                } else if (fieldName == "avgObjSize") {
                    const auto shardAvgObjSize = e.numberLong();
                    uassert(5688300, "'avgObjSize' provided but not 'count'", !countField.eoo());
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
                    LOGV2(22749,
                          "Unexpected field for mongos collStats",
                          "fieldName"_attr = e.fieldName());
                }
            }

            shardStats.append(shardId.toString(), scaleIndividualShardStatistics(res, scale));
        }

        result.append("ns",
                      NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

        for (const auto& countEntry : counts) {
            if (fieldIsAnyOf(countEntry.first,
                             {"size", "storageSize", "totalIndexSize", "totalSize"})) {
                result.appendNumber(countEntry.first, countEntry.second / scale);
            } else {
                result.appendNumber(countEntry.first, countEntry.second);
            }
        }

        if (!clusterTimeseriesStats.empty() || !timeseriesBucketsNs.empty()) {
            // 'clusterTimeseriesStats' and 'timeseriesBucketsNs' should both be set. If only one is
            // ever set, the error will be caught in appendTimeseriesInfoToResult().
            appendTimeseriesInfoToResult(clusterTimeseriesStats, timeseriesBucketsNs, &result);
        }

        {
            BSONObjBuilder ib(result.subobjStart("indexSizes"));
            for (const auto& entry : indexSizes) {
                ib.appendNumber(entry.first, entry.second / scale);
            }
            ib.done();
        }

        // The unscaled avgObjSize for each shard is used to get the unscaledCollSize because the
        // raw size returned by the shard is affected by the command's scale parameter
        if (counts["count"] > 0)
            result.append("avgObjSize", (double)unscaledCollSize / (double)counts["count"]);
        else
            result.append("avgObjSize", 0.0);

        result.append("maxSize", maxSize / scale);
        result.append("nindexes", nindexes);
        result.append("scaleFactor", scale);
        result.append("nchunks", cm.numChunks());
        result.append("shards", shardStats.obj());

        return true;
    }
};
MONGO_REGISTER_COMMAND(CollectionStats).forRouter();

}  // namespace
}  // namespace mongo
