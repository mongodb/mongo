// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries_sharded_cluster.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"

#include <algorithm>
#include <vector>

#include <boost/none.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo::timeseries {

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterEnumeratingTimeseriesCollectionsForFCV);
MONGO_FAIL_POINT_DEFINE(alwaysReportTimeseriesCollectionsNeedConversion);

/**
 * Returns all timeseries collection namespaces across the cluster that need to be converted
 * during FCV upgrade/downgrade.
 *
 * Uses $listClusterCatalog to enumerate timeseries collections. For upgrade, finds
 * system.buckets.* collections and transforms to main namespace. For downgrade, finds
 * viewless timeseries (main namespace with timeseries options, excluding system.buckets.*).
 *
 * This function must be called on the config server.
 * TODO (SERVER-116499): Remove this once 9.0 becomes last LTS.
 */
std::vector<NamespaceString> getAllClusterTimeseriesNamespaces(OperationContext* opCtx,
                                                               bool isUpgrade) {
    std::vector<NamespaceString> out;

    static const BSONObj listStage = fromjson(R"({
         $listClusterCatalog: {}
     })");

    // For upgrade: find legacy timeseries by matching system.buckets collections that have
    // timeseries options and a UUID. We query system.buckets directly (rather than views)
    // because malformed timeseries collections may have a view without a buckets collection,
    // which we should ignore. We then transform the namespace to the main namespace.
    static const BSONObj matchStageUpgrade = fromjson(R"({
         $match: {
             "info.uuid": {$exists: true},
             "options.timeseries": {$exists: true},
             "ns": {$regex: "\\.system\\.buckets\\."}
         }
     })");

    // For upgrade: transform system.buckets.ts to ts
    static const BSONObj projectStageUpgrade = fromjson(R"({
         $project: {
             _id: 0,
             ns: {
                 $replaceOne: {
                     input: "$ns",
                     find: ".system.buckets.",
                     replacement: "."
                 }
             }
         }
     })");

    // For downgrade: find viewless timeseries by matching collections with UUID and timeseries
    // options, excluding system.buckets (since viewless timeseries use the main namespace).
    static const BSONObj matchStageDowngrade = fromjson(R"({
         $match: {
             "info.uuid": {$exists: true},
             "options.timeseries": {$exists: true},
             "ns": {$not: {$regex: "\\.system\\.buckets\\."}}
         }
     })");

    // For downgrade: just project the namespace field as-is.
    static const BSONObj projectStageDowngrade = fromjson(R"({
         $project: {
             _id: 0,
             ns: 1
         }
     })");

    auto dbName = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);

    std::vector<BSONObj> pipeline;
    pipeline.reserve(3);
    pipeline.push_back(listStage);
    pipeline.push_back(isUpgrade ? matchStageUpgrade : matchStageDowngrade);
    pipeline.push_back(isUpgrade ? projectStageUpgrade : projectStageDowngrade);

    AggregateCommandRequest aggRequest{dbName, pipeline};
    aggRequest.setReadConcern(repl::ReadConcernArgs::kLocal);
    aggRequest.setReadPreference(ReadPreferenceSetting{ReadPreference::PrimaryOnly});

    const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();

    uassertStatusOK(configShard->runAggregation(
        opCtx,
        aggRequest,
        Shard::RetryPolicy::kIdempotent,
        [&out](const std::vector<BSONObj>& batch, const boost::optional<BSONObj>&) {
            for (const auto& doc : batch) {
                out.push_back(
                    NamespaceStringUtil::deserialize(boost::none,
                                                     doc.getField("ns").String(),
                                                     SerializationContext::stateDefault()));
            }
            return true;
        },
        [&out](const Status&) { out.clear(); }));

    // Failpoint for testing: always report that there are collections needing conversion.
    // This will cause the retry loop to exhaust its retries and fail setFCV.
    if (MONGO_unlikely(alwaysReportTimeseriesCollectionsNeedConversion.shouldFail())) {
        out.push_back(NamespaceString::createNamespaceString_forTest("test.fakeTimeseries"));
    }

    return out;
}

}  // namespace

void upgradeDowngradeViewlessTimeseriesInShardedCluster(OperationContext* opCtx, bool isUpgrade) {
    auto role = ShardingState::get(opCtx)->pollClusterRole();
    invariant(role->has(ClusterRole::ConfigServer));

    // Maximum number of times we re-enumerate and retry conversions. This protects against
    // infinite loops if there's a persistent issue with certain collections.
    static constexpr size_t kMaxRetries = 3;
    size_t numAttempts = 0;
    size_t numCollectionsConverted = 0;

    // Keep track of namespaces that have metadata inconsistencies and should be skipped across
    // retry iterations.
    NamespaceHashSet namespacesWithInconsistentMetadata;

    // Keep fetching and processing until no more collections need conversion. This protects
    // against a race condition where a collection might be renamed after we enumerate the
    // collections, causing us to miss converting it under its new namespace.
    while (true) {
        auto namespacesToProcess = getAllClusterTimeseriesNamespaces(opCtx, isUpgrade);

        hangAfterEnumeratingTimeseriesCollectionsForFCV.pauseWhileSet(opCtx);

        // Break if no collections need conversion, or if all remaining collections have
        // metadata inconsistencies that won't resolve with retries.
        if (namespacesToProcess.empty() ||
            (!namespacesWithInconsistentMetadata.empty() &&
             std::all_of(namespacesToProcess.begin(),
                         namespacesToProcess.end(),
                         [&namespacesWithInconsistentMetadata](const NamespaceString& nss) {
                             return namespacesWithInconsistentMetadata.contains(nss);
                         }))) {
            break;
        }

        // If we've already tried the maximum number of times and still have collections to
        // convert, fail the setFCV operation. This indicates a persistent issue that won't
        // resolve with more retries.
        uassert(11481600,
                fmt::format("Failed to convert all timeseries collections after {} attempts. "
                            "{} collections still need conversion.",
                            numAttempts,
                            namespacesToProcess.size()),
                numAttempts < kMaxRetries);

        if (numAttempts > 0) {
            logAndBackoff(11481601,
                          logv2::LogComponent::kCommand,
                          logv2::LogSeverity::Info(),
                          numAttempts,
                          "Retrying timeseries conversion after finding collections that still "
                          "need conversion",
                          "isUpgrade"_attr = isUpgrade,
                          "collectionsRemaining"_attr = namespacesToProcess.size(),
                          "collectionsSkippedDueToInconsistentMetadata"_attr =
                              namespacesWithInconsistentMetadata.size());
        }

        for (const auto& nss : namespacesToProcess) {
            // Skip namespaces we've already determined have metadata inconsistencies.
            if (namespacesWithInconsistentMetadata.contains(nss)) {
                continue;
            }

            try {
                ShardsvrUpgradeDowngradeViewlessTimeseries shardsvrReq(nss);
                shardsvrReq.setMode(isUpgrade ? TimeseriesUpgradeDowngradeModeEnum::kToViewless
                                              : TimeseriesUpgradeDowngradeModeEnum::kToLegacy);

                generic_argument_util::setMajorityWriteConcern(shardsvrReq,
                                                               &opCtx->getWriteConcern());

                sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());
                router.route("upgradeDowngradeViewlessTimeseries",
                             [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
                                 auto cmdResponse =
                                     executeCommandAgainstDatabasePrimaryOnlyAttachingDbVersion(
                                         opCtx,
                                         nss.dbName(),
                                         dbInfo,
                                         shardsvrReq.toBSON(),
                                         ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                         Shard::RetryPolicy::kIdempotent);
                                 const auto remoteResponse =
                                     uassertStatusOK(cmdResponse.swResponse);
                                 uassertStatusOK(getStatusFromCommandResult(remoteResponse.data));
                             });
                ++numCollectionsConverted;
            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // Collection was renamed or dropped since enumeration.
                // The loop will re-enumerate and find it under its new name (if renamed).
                LOGV2_DEBUG(11481602,
                            2,
                            "Skipping timeseries conversion for namespace that no longer exists",
                            "isUpgrade"_attr = isUpgrade,
                            logAttrs(nss));
            } catch (const ExceptionFor<ErrorCodes::IllegalOperation>&) {
                // Collection was dropped and recreated as non-timeseries since enumeration.
                LOGV2_DEBUG(11481603,
                            2,
                            "Skipping timeseries conversion for namespace that is no longer a "
                            "timeseries collection",
                            "isUpgrade"_attr = isUpgrade,
                            logAttrs(nss));
            } catch (const ExceptionFor<ErrorCodes::CommandNotSupportedOnView>&) {
                // Collection was dropped and a plain (non-timeseries) view was created with the
                // same name since enumeration. The namespace is no longer a convertible timeseries
                // collection, so skip it.
                LOGV2_DEBUG(12585001,
                            2,
                            "Skipping timeseries conversion for namespace that became a plain view",
                            "isUpgrade"_attr = isUpgrade,
                            logAttrs(nss));
            } catch (const ExceptionFor<ErrorCodes::TimeseriesBucketMetadataInconsistent>&) {
                // Collection has metadata inconsistencies, permanently skip it across all
                // retry iterations since this condition won't resolve with retries.
                namespacesWithInconsistentMetadata.insert(nss);
                LOGV2_WARNING(
                    11481604,
                    "Skipping timeseries conversion for namespace with metadata inconsistencies",
                    "isUpgrade"_attr = isUpgrade,
                    logAttrs(nss));
            }
        }

        ++numAttempts;
    }

    LOGV2_INFO(11481605,
               "Completed timeseries collection conversion",
               "isUpgrade"_attr = isUpgrade,
               "attempts"_attr = numAttempts,
               "collectionsConverted"_attr = numCollectionsConverted,
               "collectionsSkippedDueToInconsistentMetadata"_attr =
                   namespacesWithInconsistentMetadata.size());
}

}  // namespace mongo::timeseries
