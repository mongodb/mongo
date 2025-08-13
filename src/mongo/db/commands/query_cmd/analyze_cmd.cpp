/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/commands/query_cmd/analyze_cmd.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/analyze_command_gen.h"
#include "mongo/db/query/compiler/stats/scalar_histogram.h"
#include "mongo/db/query/compiler/stats/stats_catalog.h"
#include "mongo/db/query/compiler/stats/stats_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
namespace mongo {
namespace {

StatusWith<BSONObj> analyzeCommandAsAggregationCommand(OperationContext* opCtx,
                                                       StringData collection,
                                                       StringData keyPath,
                                                       boost::optional<double> sampleRate,
                                                       boost::optional<int> numBuckets) {
    // Build a pipeline that accomplishes the analyze request. The building code constructs a
    // pipeline that looks like this, assuming the analyze is on the key "a.b.c"
    //
    //      [
    //          { $match: { $expr: {$lt: [{$rand: {}, sampleRate]} } }, // If sampleRate is
    //          specified, otherwise this stage is omitted
    //          { $project: { val : "$a.b.c" } },
    //          { $group: {
    //              _id: "a.b.c",
    //              statistics: { $_internalConstructStats: {
    //                              val: "$$ROOT",
    //                              sampleRate: sampleRate,
    //                              numberBuckets: numberBuckets }
    //              }
    //          },
    //          { $merge: {
    //              into: "system.statistics." + collection,
    //              on: "key",
    //              whenMatched: "replace",
    //              whenNotMatched: "insert" }
    //          }
    //      ]
    //
    std::string into(str::stream() << NamespaceString::kStatisticsCollectionPrefix << collection);
    FieldPath fieldPath(keyPath);

    BSONArrayBuilder pipelineBuilder;

    if (sampleRate) {
        pipelineBuilder << BSON(
            "$match" << BSON(
                "$expr" << BSON("$lt" << BSON_ARRAY(BSON("$rand" << BSONObj()) << *sampleRate))));
    }

    InternalConstructStatsAccumulatorParams statsAccumParams;
    statsAccumParams.setVal("$$ROOT");
    statsAccumParams.setSampleRate(sampleRate ? *sampleRate : 1.0);
    statsAccumParams.setNumberBuckets(numBuckets ? *numBuckets
                                                 : mongo::stats::ScalarHistogram::kMaxBuckets);

    pipelineBuilder << BSON("$project" << BSON("val" << fieldPath.fullPathWithPrefix()))
                    << BSON("$group" << BSON("_id" << keyPath << "statistics"
                                                   << BSON("$_internalConstructStats"
                                                           << statsAccumParams.toBSON())))
                    << BSON("$merge" << BSON("into" << std::move(into) << "on"
                                                    << "_id"
                                                    << "whenMatched"
                                                    << "replace"
                                                    << "whenNotMatched"
                                                    << "insert"));

    return BSON("aggregate" << collection << "pipeline" << pipelineBuilder.arr() << "cursor"
                            << BSONObj() << "allowDiskUse" << false);
}

class CmdAnalyze final : public TypedCommand<CmdAnalyze> {
public:
    using Request = AnalyzeCommandRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    std::string help() const override {
        return "Command to generate statistics for a collection for use in the optimizer.";
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kWrite;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return request().getNamespace();
        }

        void typedRun(OperationContext* opCtx) {
            const auto& cmd = request();
            const NamespaceString& nss = ns();

            // Sample rate and sample size can't both be present
            auto sampleRate = cmd.getSampleRate();
            auto sampleSize = cmd.getSampleSize();
            uassert(6799705,
                    "Only one of sample rate and sample size may be present",
                    !sampleRate || !sampleSize);

            // Validate collection
            {
                auto coll = acquireCollectionMaybeLockFree(
                    opCtx,
                    CollectionAcquisitionRequest::fromOpCtx(
                        opCtx, nss, AcquisitionPrerequisites::kRead));
                AutoStatsTracker statsTracker(
                    opCtx,
                    nss,
                    Top::LockType::ReadLocked,
                    AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                    DatabaseProfileSettings::get(opCtx->getServiceContext())
                        .getDatabaseProfileLevel(nss.dbName()));
                const auto& collectionPtr = coll.getCollectionPtr();

                // Namespace exists
                uassert(6799700,
                        str::stream() << "Couldn't find collection " << nss.toStringForErrorMsg(),
                        coll.exists());

                // Namespace cannot be capped collection
                const bool isCapped = collectionPtr->isCapped();
                uassert(6799701,
                        str::stream() << "Analyze command is not supported on capped collections",
                        !isCapped);

                // Namespace is normal or clustered collection
                const bool isNormalColl = nss.isNormalCollection();
                const bool isClusteredColl = collectionPtr->isClustered();
                uassert(6799702,
                        str::stream() << nss.toStringForErrorMsg()
                                      << " is not a normal or clustered collection",
                        isNormalColl || isClusteredColl);

                if (sampleSize) {
                    auto numRecords = collectionPtr->numRecords(opCtx);
                    if (numRecords == 0 || *sampleSize > numRecords) {
                        sampleRate = 1.0;
                    } else {
                        sampleRate = double(*sampleSize) / collectionPtr->numRecords(opCtx);
                    }
                }
            }

            // Validate key
            auto key = cmd.getKey();
            if (key) {
                const FieldRef keyFieldRef(*key);

                // Empty path
                uassert(6799703, "Key path is empty", !keyFieldRef.empty());

                for (size_t i = 0; i < keyFieldRef.numParts(); ++i) {
                    uassertStatusOK(FieldPath::validateFieldName(keyFieldRef.getPart(i)));
                }

                // Numerics
                const auto numericPathComponents = keyFieldRef.getNumericPathComponents(0);
                uassert(6799704,
                        str::stream() << "Key path contains numeric component "
                                      << keyFieldRef.getPart(*(numericPathComponents.begin())),
                        numericPathComponents.empty());

                // We need to perform this operation with internal permissions.
                const bool wasInternalClient = isInternalClient(opCtx->getClient());
                if (!wasInternalClient) {
                    opCtx->getClient()->setIsInternalClient(true);
                }

                DBDirectClient client(opCtx);

                // Run Aggregate
                BSONObj analyzeResult;
                client.runCommand(
                    nss.dbName(),
                    analyzeCommandAsAggregationCommand(
                        opCtx, nss.coll(), std::string{*key}, sampleRate, cmd.getNumberBuckets())
                        .getValue(),
                    analyzeResult);

                // We must reset the internal flag.
                if (!wasInternalClient) {
                    opCtx->getClient()->setIsInternalClient(false);
                }

                uassertStatusOK(getStatusFromCommandResult(analyzeResult));

                // Invalidate statistics in the cache for the analyzed path
                stats::StatsCatalog& statsCatalog = stats::StatsCatalog::get(opCtx);
                uassertStatusOK(statsCatalog.invalidatePath(nss, std::string{*key}));

            } else if (sampleSize || sampleRate) {
                uassert(
                    6799706, "It is illegal to pass sampleRate or sampleSize without a key", key);
            }
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            auto* authzSession = AuthorizationSession::get(opCtx->getClient());
            const NamespaceString& ns = request().getNamespace();

            uassert(ErrorCodes::Unauthorized,
                    str::stream() << "Not authorized to call analyze on collection "
                                  << ns.toStringForErrorMsg(),
                    authzSession->isAuthorizedForActionsOnNamespace(ns, ActionType::analyze));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdAnalyze).forShard().testOnly();

}  // namespace
}  // namespace mongo
