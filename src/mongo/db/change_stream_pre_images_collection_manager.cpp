/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_images_collection_manager.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/drop_gen.h"
#include "mongo/db/exec/batched_delete_stage.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/delete_stage.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPreimagesCollectionCreation);

const auto getPreImagesCollectionManager =
    ServiceContext::declareDecoration<ChangeStreamPreImagesCollectionManager>();

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getDeleteExpiredPreImagesExecutor(
    OperationContext* opCtx,
    CollectionAcquisition preImageColl,
    const MatchExpression* filterPtr,
    Timestamp maxRecordIdTimestamp,
    UUID currentCollectionUUID) {
    auto params = std::make_unique<DeleteStageParams>();
    params->isMulti = true;

    std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams;
    batchedDeleteParams = std::make_unique<BatchedDeleteStageParams>();
    RecordIdBound minRecordId =
        change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(
            currentCollectionUUID);
    RecordIdBound maxRecordId =
        RecordIdBound(change_stream_pre_image_util::toRecordId(ChangeStreamPreImageId(
            currentCollectionUUID, maxRecordIdTimestamp, std::numeric_limits<int64_t>::max())));

    return InternalPlanner::deleteWithCollectionScan(
        opCtx,
        std::move(preImageColl),
        std::move(params),
        PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
        InternalPlanner::Direction::FORWARD,
        std::move(minRecordId),
        std::move(maxRecordId),
        CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
        std::move(batchedDeleteParams),
        filterPtr,
        filterPtr != nullptr);
}

bool useUnreplicatedTruncates() {
    bool res = feature_flags::gFeatureFlagUseUnreplicatedTruncatesForDeletions.isEnabled(
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    return res;
}
}  // namespace

BSONObj ChangeStreamPreImagesCollectionManager::PurgingJobStats::toBSON() const {
    BSONObjBuilder builder;
    builder.append("totalPass", totalPass.loadRelaxed())
        .append("docsDeleted", docsDeleted.loadRelaxed())
        .append("bytesDeleted", bytesDeleted.loadRelaxed())
        .append("scannedCollections", scannedCollections.loadRelaxed())
        .append("scannedInternalCollections", scannedInternalCollections.loadRelaxed())
        .append("maxStartWallTimeMillis", maxStartWallTime.loadRelaxed().toMillisSinceEpoch())
        .append("timeElapsedMillis", timeElapsedMillis.loadRelaxed());
    return builder.obj();
}

ChangeStreamPreImagesCollectionManager& ChangeStreamPreImagesCollectionManager::get(
    ServiceContext* service) {
    return getPreImagesCollectionManager(service);
}

ChangeStreamPreImagesCollectionManager& ChangeStreamPreImagesCollectionManager::get(
    OperationContext* opCtx) {
    return getPreImagesCollectionManager(opCtx->getServiceContext());
}

void ChangeStreamPreImagesCollectionManager::createPreImagesCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    uassert(5868501,
            "Failpoint failPreimagesCollectionCreation enabled. Throwing exception",
            !MONGO_unlikely(failPreimagesCollectionCreation.shouldFail()));
    const auto preImagesCollectionNamespace = NamespaceString::makePreImageCollectionNSS(
        change_stream_serverless_helpers::resolveTenantId(tenantId));

    CollectionOptions preImagesCollectionOptions;

    // Make the collection clustered by _id.
    preImagesCollectionOptions.clusteredIndex.emplace(
        clustered_util::makeCanonicalClusteredInfoForLegacyFormat());
    const auto status = createCollection(
        opCtx, preImagesCollectionNamespace, preImagesCollectionOptions, BSONObj());
    uassert(status.code(),
            str::stream() << "Failed to create the pre-images collection: "
                          << preImagesCollectionNamespace.toStringForErrorMsg()
                          << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceExists);
}

void ChangeStreamPreImagesCollectionManager::dropPreImagesCollection(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    const auto preImagesCollectionNamespace = NamespaceString::makePreImageCollectionNSS(
        change_stream_serverless_helpers::resolveTenantId(tenantId));
    DropReply dropReply;
    const auto status =
        dropCollection(opCtx,
                       preImagesCollectionNamespace,
                       &dropReply,
                       DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
    uassert(status.code(),
            str::stream() << "Failed to drop the pre-images collection: "
                          << preImagesCollectionNamespace.toStringForErrorMsg()
                          << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceNotFound);

    if (useUnreplicatedTruncates()) {
        _truncateManager.dropAllMarkersForTenant(tenantId);
    }
}

void ChangeStreamPreImagesCollectionManager::insertPreImage(OperationContext* opCtx,
                                                            boost::optional<TenantId> tenantId,
                                                            const ChangeStreamPreImage& preImage) {
    tassert(6646200,
            "Expected to be executed in a write unit of work",
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    tassert(5869404,
            str::stream() << "Invalid pre-images document applyOpsIndex: "
                          << preImage.getId().getApplyOpsIndex(),
            preImage.getId().getApplyOpsIndex() >= 0);

    const auto preImagesCollectionNamespace = NamespaceString::makePreImageCollectionNSS(
        change_stream_serverless_helpers::resolveTenantId(tenantId));

    // This lock acquisition can block on a stronger lock held by another operation modifying
    // the pre-images collection. There are no known cases where an operation holding an
    // exclusive lock on the pre-images collection also waits for oplog visibility.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    const auto changeStreamPreImagesCollection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(preImagesCollectionNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    if (preImagesCollectionNamespace.tenantId() &&
        !change_stream_serverless_helpers::isChangeStreamEnabled(
            opCtx, *preImagesCollectionNamespace.tenantId())) {
        return;
    }
    tassert(6646201,
            "The change stream pre-images collection is not present",
            changeStreamPreImagesCollection.exists());

    auto insertStatement = InsertStatement{preImage.toBSON()};
    const auto insertionStatus =
        collection_internal::insertDocument(opCtx,
                                            changeStreamPreImagesCollection.getCollectionPtr(),
                                            insertStatement,
                                            &CurOp::get(opCtx)->debug());
    tassert(5868601,
            str::stream() << "Attempted to insert a duplicate document into the pre-images "
                             "collection. Pre-image id: "
                          << preImage.getId().toBSON().toString(),
            insertionStatus != ErrorCodes::DuplicateKey);
    uassertStatusOK(insertionStatus);

    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [this](OperationContext* opCtx, boost::optional<Timestamp>) {
            _docsInserted.fetchAndAddRelaxed(1);
        });

    if (useUnreplicatedTruncates()) {
        // This is a no-op until the 'tenantId' is registered with the 'truncateManager' in the
        // expired pre-image removal path.
        auto bytesInserted = insertStatement.doc.objsize();
        _truncateManager.updateMarkersOnInsert(opCtx, tenantId, preImage, bytesInserted);
    }
}

void ChangeStreamPreImagesCollectionManager::performExpiredChangeStreamPreImagesRemovalPass(
    Client* client) {
    Timer timer;

    const auto startTime = Date_t::now();
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = client->makeOperationContext();

        Date_t currentTimeForTimeBasedExpiration =
            change_stream_pre_image_util::getCurrentTimeForPreImageRemoval(opCtx.get());
        size_t numberOfRemovals = 0;

        if (useUnreplicatedTruncates()) {
            if (change_stream_serverless_helpers::isChangeCollectionsModeActive()) {
                const auto tenantIds =
                    change_stream_serverless_helpers::getConfigDbTenants(opCtx.get());
                for (const auto& tenantId : tenantIds) {
                    numberOfRemovals += _deleteExpiredPreImagesWithTruncate(opCtx.get(), tenantId);
                }
            } else {
                numberOfRemovals =
                    _deleteExpiredPreImagesWithTruncate(opCtx.get(), boost::none /** tenantId **/);
            }
        } else {
            if (change_stream_serverless_helpers::isChangeCollectionsModeActive()) {
                // A serverless environment is enabled and removal logic must take the tenantId into
                // account.
                const auto tenantIds =
                    change_stream_serverless_helpers::getConfigDbTenants(opCtx.get());
                for (const auto& tenantId : tenantIds) {
                    numberOfRemovals += _deleteExpiredPreImagesWithCollScanForTenants(
                        opCtx.get(), tenantId, currentTimeForTimeBasedExpiration);
                }
            } else {
                numberOfRemovals = _deleteExpiredPreImagesWithCollScan(
                    opCtx.get(), currentTimeForTimeBasedExpiration);
            }
        }

        if (numberOfRemovals > 0) {
            LOGV2_DEBUG(5869104,
                        3,
                        "Periodic expired pre-images removal job finished executing",
                        "numberOfRemovals"_attr = numberOfRemovals,
                        "jobDuration"_attr = (Date_t::now() - startTime).toString());
        }
    } catch (const DBException& exception) {
        Status interruptStatus = opCtx ? opCtx.get()->checkForInterruptNoAssert() : Status::OK();
        if (!interruptStatus.isOK()) {
            LOGV2_DEBUG(5869105,
                        3,
                        "Periodic expired pre-images removal job operation was interrupted",
                        "errorCode"_attr = interruptStatus);
        } else {
            LOGV2_ERROR(5869106,
                        "Periodic expired pre-images removal job failed",
                        "reason"_attr = exception.reason());
        }
    }

    _purgingJobStats.timeElapsedMillis.fetchAndAddRelaxed(timer.millis());
    _purgingJobStats.totalPass.fetchAndAddRelaxed(1);
}

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithCollScanCommon(
    OperationContext* opCtx,
    const CollectionAcquisition& preImageColl,
    const MatchExpression* filterPtr,
    Timestamp maxRecordIdTimestamp) {
    size_t numberOfRemovals = 0;
    boost::optional<UUID> currentCollectionUUID = boost::none;

    // Placeholder for the wall time of the first document of the current pre-images internal
    // collection being examined.
    Date_t firstDocWallTime{};

    while (
        (currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
             opCtx, &preImageColl.getCollectionPtr(), currentCollectionUUID, firstDocWallTime))) {
        writeConflictRetry(
            opCtx,
            "ChangeStreamExpiredPreImagesRemover",
            NamespaceString::makePreImageCollectionNSS(boost::none),
            [&] {
                auto exec = getDeleteExpiredPreImagesExecutor(
                    opCtx, preImageColl, filterPtr, maxRecordIdTimestamp, *currentCollectionUUID);
                numberOfRemovals += exec->executeDelete();
                auto batchedDeleteStats = exec->getBatchedDeleteStats();

                _purgingJobStats.docsDeleted.fetchAndAddRelaxed(batchedDeleteStats.docsDeleted);
                _purgingJobStats.bytesDeleted.fetchAndAddRelaxed(batchedDeleteStats.bytesDeleted);
                _purgingJobStats.scannedInternalCollections.fetchAndAddRelaxed(1);
            });
        if (firstDocWallTime > _purgingJobStats.maxStartWallTime.load()) {
            _purgingJobStats.maxStartWallTime.store(firstDocWallTime);
        }
    }
    _purgingJobStats.scannedCollections.fetchAndAddRelaxed(1);
    return numberOfRemovals;
}

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithCollScan(
    OperationContext* opCtx, Date_t currentTimeForTimeBasedExpiration) {
    // Change stream collections can multiply the amount of user data inserted and deleted on each
    // node. It is imperative that removal is prioritized so it can keep up with inserts and prevent
    // users from running out of disk space.
    ScopedAdmissionPriority skipAdmissionControl(opCtx, AdmissionContext::Priority::kExempt);

    // Acquire intent-exclusive lock on the change collection.
    const auto preImageColl = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::makePreImageCollectionNSS(boost::none),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);

    // Early exit if the collection doesn't exist or running on a secondary.
    if (!preImageColl.exists() ||
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
            opCtx, DatabaseName::kConfig)) {
        return 0;
    }

    // Get the timestamp of the earliest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    const auto preImageExpirationTime =
        change_stream_pre_image_util::getPreImageOpTimeExpirationDate(
            opCtx, boost::none /** tenantId **/, currentTimeForTimeBasedExpiration);

    // Configure the filter for the case when expiration parameter is set.
    if (preImageExpirationTime) {
        OrMatchExpression filter;
        filter.add(
            std::make_unique<LTMatchExpression>("_id.ts"_sd, Value(currentEarliestOplogEntryTs)));
        filter.add(std::make_unique<LTEMatchExpression>("operationTime"_sd,
                                                        Value(*preImageExpirationTime)));
        // If 'preImageExpirationTime' is set, set 'maxRecordIdTimestamp' is set to the maximum
        // RecordId for this collection. Whether the pre-image has to be deleted will be determined
        // by the 'filter' parameter.
        return _deleteExpiredPreImagesWithCollScanCommon(
            opCtx, preImageColl, &filter, Timestamp::max() /* maxRecordIdTimestamp */);
    }

    // 'preImageExpirationTime' is not set, so the last expired pre-image timestamp is less than
    // 'currentEarliestOplogEntryTs'.
    return _deleteExpiredPreImagesWithCollScanCommon(
        opCtx,
        preImageColl,
        nullptr /* filterPtr */,
        Timestamp(currentEarliestOplogEntryTs.asULL() - 1) /* maxRecordIdTimestamp */);
}

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithCollScanForTenants(
    OperationContext* opCtx, const TenantId& tenantId, Date_t currentTimeForTimeBasedExpiration) {
    // Change stream collections can multiply the amount of user data inserted and deleted on each
    // node. It is imperative that removal is prioritized so it can keep up with inserts and prevent
    // users from running out of disk space.
    ScopedAdmissionPriority skipAdmissionControl(opCtx, AdmissionContext::Priority::kExempt);

    // Acquire intent-exclusive lock on the change collection.
    const auto preImageColl =
        acquireCollection(opCtx,
                          CollectionAcquisitionRequest(
                              NamespaceString::makePreImageCollectionNSS(
                                  change_stream_serverless_helpers::resolveTenantId(tenantId)),
                              PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                              repl::ReadConcernArgs::get(opCtx),
                              AcquisitionPrerequisites::kWrite),
                          MODE_IX);

    // Early exit if the collection doesn't exist or running on a secondary.
    if (!preImageColl.exists() ||
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
            opCtx, DatabaseName::kConfig)) {
        return 0;
    }

    auto expiredAfterSeconds = change_stream_serverless_helpers::getExpireAfterSeconds(tenantId);
    LTEMatchExpression filter{
        "operationTime"_sd,
        Value(currentTimeForTimeBasedExpiration - Seconds(expiredAfterSeconds))};

    // Set the 'maxRecordIdTimestamp' parameter (upper scan boundary) to maximum possible. Whether
    // the pre-image has to be deleted will be determined by the 'filter' parameter.
    return _deleteExpiredPreImagesWithCollScanCommon(
        opCtx, preImageColl, &filter, Timestamp::max() /* maxRecordIdTimestamp */);
}

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithTruncate(
    OperationContext* opCtx, boost::optional<TenantId> tenantId) {
    const auto truncateStats =
        _truncateManager.truncateExpiredPreImages(opCtx, std::move(tenantId));

    if (truncateStats.maxStartWallTime > _purgingJobStats.maxStartWallTime.loadRelaxed()) {
        _purgingJobStats.maxStartWallTime.store(truncateStats.maxStartWallTime);
    }

    _purgingJobStats.docsDeleted.fetchAndAddRelaxed(truncateStats.docsDeleted);
    _purgingJobStats.bytesDeleted.fetchAndAddRelaxed(truncateStats.bytesDeleted);
    _purgingJobStats.scannedInternalCollections.fetchAndAddRelaxed(
        truncateStats.scannedInternalCollections);

    _purgingJobStats.scannedCollections.fetchAndAddRelaxed(1);

    return truncateStats.docsDeleted;
}

}  // namespace mongo
