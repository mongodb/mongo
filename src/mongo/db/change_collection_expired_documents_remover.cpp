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

#include "mongo/db/change_collection_expired_documents_remover.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/change_streams_cluster_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

// Hangs the change collection remover job before initiating the deletion process of documents.
MONGO_FAIL_POINT_DEFINE(hangBeforeRemovingExpiredChanges);

namespace {

change_stream_serverless_helpers::TenantSet getConfigDbTenants(OperationContext* opCtx) {
    auto tenantIds = change_stream_serverless_helpers::getConfigDbTenants(opCtx);
    if (auto testTenantId = change_stream_serverless_helpers::resolveTenantId(boost::none)) {
        tenantIds.insert(*testTenantId);
    }

    return tenantIds;
}

bool usesUnreplicatedTruncates() {
    // (Ignore FCV check): This feature flag is potentially backported to previous version of the
    // server. We can't rely on the FCV version to see whether it's enabled or not.
    return feature_flags::gFeatureFlagUseUnreplicatedTruncatesForDeletions
        .isEnabledAndIgnoreFCVUnsafe();
}

void removeExpiredDocuments(Client* client) {
    bool useUnreplicatedTruncates = usesUnreplicatedTruncates();

    try {
        auto opCtx = client->makeOperationContext();
        hangBeforeRemovingExpiredChanges.pauseWhileSet(opCtx.get());
        const auto clock = client->getServiceContext()->getFastClockSource();
        auto currentWallTime =
            change_stream_serverless_helpers::getCurrentTimeForChangeCollectionRemoval(opCtx.get());

        // Number of documents removed in the current pass.
        size_t removedCount = 0;
        long long maxStartWallTime = 0;
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx.get());

        for (const auto& tenantId : getConfigDbTenants(opCtx.get())) {
            // Change stream collections can multiply the amount of user data inserted and deleted
            // on each node. It is imperative that removal is prioritized so it can keep up with
            // inserts and prevent users from running out of disk space.
            ScopedAdmissionPriority skipAdmissionControl(opCtx.get(),
                                                         AdmissionContext::Priority::kExempt);

            auto expiredAfterSeconds =
                change_stream_serverless_helpers::getExpireAfterSeconds(tenantId);

            if (useUnreplicatedTruncates) {
                removedCount += ChangeStreamChangeCollectionManager::
                    removeExpiredChangeCollectionsDocumentsWithTruncate(opCtx.get(), tenantId);
            } else {
                auto changeCollection =
                    acquireCollection(opCtx.get(),
                                      CollectionAcquisitionRequest(
                                          NamespaceString::makeChangeCollectionNSS(tenantId),
                                          PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                          repl::ReadConcernArgs::get(opCtx.get()),
                                          AcquisitionPrerequisites::kWrite),
                                      MODE_IX);

                // Early exit if the collection does not exist, or when running on a secondary with
                // replicated deletes.
                if (!changeCollection.exists() ||
                    !repl::ReplicationCoordinator::get(opCtx.get())
                         ->canAcceptWritesForDatabase(opCtx.get(), DatabaseName::kConfig)) {
                    break;
                }

                // Get the metadata required for the removal of the expired change collection
                // documents. Early exit if the metadata is missing, indicating that there is
                // nothing to remove.
                auto purgingJobMetadata =
                    ChangeStreamChangeCollectionManager::getChangeCollectionPurgingJobMetadata(
                        opCtx.get(), changeCollection);
                if (!purgingJobMetadata) {
                    continue;
                }

                removedCount += ChangeStreamChangeCollectionManager::
                    removeExpiredChangeCollectionsDocumentsWithCollScan(
                        opCtx.get(),
                        changeCollection,
                        purgingJobMetadata->maxRecordIdBound,
                        currentWallTime - Seconds(expiredAfterSeconds));
                maxStartWallTime =
                    std::max(maxStartWallTime, purgingJobMetadata->firstDocWallTimeMillis);
            }

            changeCollectionManager.getPurgingJobStats().scannedCollections.fetchAndAddRelaxed(1);
        }

        // The purging job metadata will be 'boost::none' if none of the change collections have
        // more than one oplog entry, as such the 'maxStartWallTimeMillis' will be zero. Avoid
        // reporting 0 as 'maxStartWallTimeMillis'. If using unreplicated truncates, this is
        // maintained by the call to removeExpiredChangeCollectionsDocumentsWithTruncate.
        if (!useUnreplicatedTruncates && maxStartWallTime > 0) {
            changeCollectionManager.getPurgingJobStats().maxStartWallTimeMillis.store(
                maxStartWallTime);
        }

        const auto jobDurationMillis = clock->now() - currentWallTime;
        if (removedCount > 0) {
            LOGV2_DEBUG(6663503,
                        3,
                        "Periodic expired change collection job finished executing",
                        "numberOfRemovals"_attr = removedCount,
                        "jobDuration"_attr = jobDurationMillis.toString());
        }

        changeCollectionManager.getPurgingJobStats().totalPass.fetchAndAddRelaxed(1);
        changeCollectionManager.getPurgingJobStats().timeElapsedMillis.fetchAndAddRelaxed(
            jobDurationMillis.count());
    } catch (const DBException& exception) {
        if (exception.toStatus() != ErrorCodes::OK) {
            LOGV2_WARNING(6663504,
                          "Periodic expired change collection job was killed",
                          "errorCode"_attr = exception.toStatus());
        } else {
            LOGV2_ERROR(6663505,
                        "Periodic expired change collection job failed",
                        "reason"_attr = exception.reason());
        }
    }
}

/**
 * Defines a periodic background job to remove expired documents from change collections.
 * The job will run every 'changeCollectionExpiredDocumentsRemoverJobSleepSeconds', as defined in
 * the cluster parameter.
 */
class ChangeCollectionExpiredDocumentsRemover {
public:
    ChangeCollectionExpiredDocumentsRemover(ServiceContext* serviceContext) {
        const auto period = Seconds{gChangeCollectionExpiredDocumentsRemoverJobSleepSeconds.load()};
        _jobAnchor =
            serviceContext->getPeriodicRunner()->makeJob({"ChangeCollectionExpiredDocumentsRemover",
                                                          removeExpiredDocuments,
                                                          period,
                                                          true /*isKillableByStepdown*/});
        _jobAnchor.start();
    }

    static void start(ServiceContext* serviceContext) {
        auto& changeCollectionExpiredDocumentsRemover = _serviceDecoration(serviceContext);
        changeCollectionExpiredDocumentsRemover =
            std::make_unique<ChangeCollectionExpiredDocumentsRemover>(serviceContext);
    }

    static void shutdown(ServiceContext* serviceContext) {
        auto& changeCollectionExpiredDocumentsRemover = _serviceDecoration(serviceContext);
        if (changeCollectionExpiredDocumentsRemover) {
            changeCollectionExpiredDocumentsRemover->_jobAnchor.stop();
            changeCollectionExpiredDocumentsRemover.reset();
        }
    }

private:
    inline static const auto _serviceDecoration = ServiceContext::declareDecoration<
        std::unique_ptr<ChangeCollectionExpiredDocumentsRemover>>();

    PeriodicJobAnchor _jobAnchor;
};
}  // namespace

void startChangeCollectionExpiredDocumentsRemover(ServiceContext* serviceContext) {
    if (change_stream_serverless_helpers::canInitializeServices()) {
        LOGV2(6663507, "Starting the ChangeCollectionExpiredChangeRemover");
        ChangeCollectionExpiredDocumentsRemover::start(serviceContext);
    }
}

void shutdownChangeCollectionExpiredDocumentsRemover(ServiceContext* serviceContext) {
    if (change_stream_serverless_helpers::canInitializeServices()) {
        LOGV2(6663508, "Shutting down the ChangeCollectionExpiredChangeRemover");
        ChangeCollectionExpiredDocumentsRemover::shutdown(serviceContext);
    }
}
}  // namespace mongo
