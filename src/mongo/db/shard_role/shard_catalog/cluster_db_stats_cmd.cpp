// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/dbcommands_gen.h"
#include "mongo/db/metrics_filtering_util.h"
#include "mongo/db/metrics_policy_manager.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

void aggregateResults(const DBStatsCommand& cmd,
                      const std::vector<AsyncRequestsSender::Response>& responses,
                      BSONObjBuilder& output) {
    int scale = cmd.getScale();
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
    double freeStorageSize = 0;
    double totalFreeStorageSize = 0;
    double indexFreeStorageSize = 0;

    for (const auto& response : responses) {
        invariant(response.swResponse.getStatus());
        const BSONObj& b = response.swResponse.getValue().data;
        auto resp = DBStats::parse(b, IDLParserContext{"dbstats"});

        // The following fields are optional because they are filtered out when underlying client is
        // subject to metrics filtering. However, there is no metrics filtering for internal
        // clients, i.e. including when a mongos run a metrics command against a shardsvr or
        // configsvr mongod. So these fields should be present in the response from each shard.
        uassert(ErrorCodes::InternalError,
                "Missing required field storageSize in dbStats response",
                resp.getStorageSize().has_value());
        uassert(ErrorCodes::InternalError,
                "Missing required field indexSize in dbStats response",
                resp.getIndexSize().has_value());
        uassert(ErrorCodes::InternalError,
                "Missing required field totalSize in dbStats response",
                resp.getTotalSize().has_value());

        collections += resp.getCollections();
        views += resp.getViews();
        objects += resp.getObjects();
        unscaledDataSize += resp.getAvgObjSize() * resp.getObjects();
        dataSize += resp.getDataSize();
        storageSize += *resp.getStorageSize();
        totalSize += *resp.getTotalSize();
        indexes += resp.getIndexes();
        indexSize += *resp.getIndexSize();
        fsUsedSize += resp.getFsUsedSize().get_value_or(0);
        fsTotalSize += resp.getFsTotalSize().get_value_or(0);
        freeStorageSize += resp.getFreeStorageSize().get_value_or(0);
        totalFreeStorageSize += resp.getTotalFreeStorageSize().get_value_or(0);
        indexFreeStorageSize += resp.getIndexFreeStorageSize().get_value_or(0);
    }

    output.appendNumber("collections", collections);
    output.appendNumber("views", views);
    output.appendNumber("objects", objects);

    bool freeStorage = cmd.getFreeStorage();

    // avgObjSize on mongod is not scaled based on the argument to db.stats(), so we use
    // unscaledDataSize here for consistency.  See SERVER-7347.
    output.appendNumber("avgObjSize", objects == 0 ? 0 : unscaledDataSize / double(objects));
    output.appendNumber("dataSize", dataSize);
    output.appendNumber("storageSize", storageSize);
    if (freeStorage) {
        output.appendNumber("freeStorageSize", freeStorageSize);
    }
    output.appendNumber("indexes", indexes);
    output.appendNumber("indexSize", indexSize);
    if (freeStorage) {
        output.appendNumber("indexFreeStorageSize", indexFreeStorageSize);
    }
    output.appendNumber("totalSize", totalSize);
    if (freeStorage) {
        output.appendNumber("totalFreeStorageSize", totalFreeStorageSize);
    }
    output.appendNumber("scaleFactor", scale);
    output.appendNumber("fsUsedSize", fsUsedSize);
    output.appendNumber("fsTotalSize", fsTotalSize);
}

/**
 * Filters and appends results when metrics filtering is enabled. Filters both the per-shard
 * metrics in the "raw" field and cluster metrics using the provided matcher.
 */
void appendFilteredResults(BSONObjBuilder& inputResultBuilder,
                           const BSONObj& unfilteredResult,
                           const PathMatcherNode& matcher) {
    // Filter and append per-shard metrics in the "raw" field.
    if (unfilteredResult.hasField("raw")) {
        auto rawElement = unfilteredResult.getObjectField("raw");
        BSONObjBuilder rawBuilder(inputResultBuilder.subobjStart("raw"));
        for (const auto& elem : rawElement) {
            auto shardResponse = elem.embeddedObject();
            // Only filter successful responses. Pass through error responses unchanged.
            if (shardResponse["ok"].trueValue()) {
                BSONObjBuilder filteredBuilder;
                metrics_filtering_util::appendPaths(filteredBuilder, shardResponse, matcher);
                rawBuilder.append(elem.fieldName(), filteredBuilder.obj());
            } else {
                rawBuilder.append(elem.fieldName(), shardResponse);
            }
        }
        rawBuilder.doneFast();
    }

    // Filter and append cluster metrics.
    metrics_filtering_util::appendPaths(inputResultBuilder, unfilteredResult, matcher);
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
        if (!as->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                  ActionType::dbStats)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }
        return Status::OK();
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& inputResultBuilder) final {
        const auto& cmd = requestParser.request();
        uassert(ErrorCodes::BadValue, "Scale must be greater than zero", cmd.getScale() > 0);

        // If filtering is required by the metrics policy, append the metrics to a temporary result
        // builder and filter them at the end. Otherwise, append directly to the input result
        // builder to avoid additional costs in the non-filtering case.
        auto& metricsPolicyManager = MetricsPolicyManager::get(opCtx);
        bool requireFiltering = metricsPolicyManager.requiresFiltering(
            opCtx, MetricsCategoryEnum::kDbStats, /*forceFiltered=*/false);

        boost::optional<BSONObjBuilder> tmpResultBuilder;
        if (requireFiltering) {
            tmpResultBuilder.emplace();
        }
        BSONObjBuilder& output = requireFiltering ? *tmpResultBuilder : inputResultBuilder;

        auto shardResponses = scatterGatherUnversionedTargetAllShards(
            opCtx,
            dbName,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent);
        std::string errmsg;
        auto appendResult = appendRawResponses(opCtx, &errmsg, &output, shardResponses);
        uassert(ErrorCodes::OperationFailed, errmsg, appendResult.responseOK);

        output.append("db", DatabaseNameUtil::serialize(dbName, cmd.getSerializationContext()));
        aggregateResults(cmd, appendResult.successResponses, output);

        // If filtering is required, we appended the metrics to a temporary result builder.
        // Now extract and append only the ones matching the allowlist to the input result builder.
        if (requireFiltering) {
            const auto& matcher =
                metricsPolicyManager.getAllowlistMatcher(MetricsCategoryEnum::kDbStats);
            appendFilteredResults(inputResultBuilder, tmpResultBuilder->obj(), matcher);
        }

        return true;
    }

    void validateResult(const BSONObj& resultObj) final {
        DBStats::parse(resultObj, IDLParserContext{"dbstats.reply"});
    }
};
MONGO_REGISTER_COMMAND(CmdDBStats).forRouter();

}  // namespace
}  // namespace mongo
