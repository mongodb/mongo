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

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_write_path.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPreimagesCollectionCreation);

const auto getPreImagesCollectionManager =
    ServiceContext::declareDecoration<ChangeStreamPreImagesCollectionManager>();

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getDeleteExpiredPreImagesExecutor(
    OperationContext* opCtx,
    const ScopedCollectionAcquisition& preImageColl,
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
        preImageColl,
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
        serverGlobalParams.featureCompatibility);
    return res;
}

// Performs a ranged truncate over each expired marker in 'truncateMarkersForNss'. Updates the
// "Output" parameters to communicate the respective docs deleted, bytes deleted, and and maximum
// wall time of documents deleted to the caller.
void truncateExpiredMarkersForCollection(
    OperationContext* opCtx,
    std::shared_ptr<PreImagesTruncateMarkersPerCollection> truncateMarkersForNss,
    const CollectionPtr& preImagesColl,
    const UUID& nsUUID,
    const RecordId& minRecordIdForNs,
    int64_t& totalDocsDeletedOutput,
    int64_t& totalBytesDeletedOutput,
    Date_t& maxWallTimeForNsTruncateOutput) {
    while (auto marker = truncateMarkersForNss->peekOldestMarkerIfNeeded(opCtx)) {
        writeConflictRetry(
            opCtx, "truncate pre-images collection for UUID", preImagesColl->ns().ns(), [&] {
                // The session might be in use from marker initialisation so we must
                // reset it here in order to allow an untimestamped write.
                opCtx->recoveryUnit()->abandonSnapshot();
                opCtx->recoveryUnit()->allowOneUntimestampedWrite();

                WriteUnitOfWork wuow(opCtx);
                auto bytesDeleted = marker->bytes;
                auto docsDeleted = marker->records;
                auto rs = preImagesColl->getRecordStore();
                auto status = rs->rangeTruncate(
                    opCtx, minRecordIdForNs, marker->lastRecord, -bytesDeleted, -docsDeleted);
                invariantStatusOK(status);
                wuow.commit();

                if (marker->wallTime > maxWallTimeForNsTruncateOutput) {
                    maxWallTimeForNsTruncateOutput = marker->wallTime;
                }

                truncateMarkersForNss->popOldestMarker();

                totalDocsDeletedOutput += docsDeleted;
                totalBytesDeletedOutput += bytesDeleted;
            });
    }
}

void updateTruncateMarkersOnInsert(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const ChangeStreamPreImage& preImage,
    int64_t bytesInserted,
    std::shared_ptr<
        ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerCollection, UUID::Hash>>
        preImagesCollectionTruncateMap) {
    auto nsUuid = preImage.getId().getNsUUID();

    auto truncateMarkersForNs = preImagesCollectionTruncateMap->find(nsUuid);

    if (!truncateMarkersForNs) {
        // There are 2 possible scenarios here:
        //  (1) The 'preImagesCollectionTruncateMap' has been created, but isn't done with
        //  initialisation. In this case, the truncate markers created for 'nsUUID' may or may not
        //  be overwritten in the initialisation process. This is okay, lazy initialisation is
        //  performed by the remover thread to avoid blocking writes on startup and is strictly best
        //  effort.
        //
        //  (2) Pre-images were either recently enabled on 'nsUUID' or 'nsUUID' was just created.
        //  Either way, the first pre-images enabled insert to call 'getOrEmplace()' creates the
        //  truncate markers for the 'nsUUID'. Any following calls to 'getOrEmplace()' return a
        //  pointer to the existing truncate markers.
        truncateMarkersForNs = preImagesCollectionTruncateMap->getOrEmplace(
            nsUuid,
            tenantId,
            std::deque<CollectionTruncateMarkers::Marker>{},
            0,
            0,
            gPreImagesCollectionTruncateMarkersMinBytes);
    }

    auto wallTime = preImage.getOperationTime();
    auto recordId = change_stream_pre_image_util::toRecordId(preImage.getId());
    truncateMarkersForNs->updateCurrentMarkerAfterInsertOnCommit(
        opCtx, bytesInserted, recordId, wallTime, 1);
}

// Parses the pre-images collection to create an initial mapping between collections which generate
// pre-images and their corresponding truncate markers.
using Map =
    absl::flat_hash_map<UUID, std::shared_ptr<PreImagesTruncateMarkersPerCollection>, UUID::Hash>;
Map createInitialPreImagesCollectionMapScanning(OperationContext* opCtx,
                                                boost::optional<TenantId> tenantId,
                                                const CollectionPtr* preImagesCollectionPtr) {

    WriteUnitOfWork wuow(opCtx);
    auto rs = preImagesCollectionPtr->get()->getRecordStore();

    Map truncateMap;
    auto minBytesPerMarker = gPreImagesCollectionTruncateMarkersMinBytes;
    boost::optional<UUID> currentCollectionUUID = boost::none;
    Date_t firstWallTime{};

    while ((currentCollectionUUID = change_stream_pre_image_util::findNextCollectionUUID(
                opCtx, preImagesCollectionPtr, currentCollectionUUID, firstWallTime))) {
        Date_t highestSeenWallTime{};
        RecordId highestSeenRecordId{};
        auto initialSetOfMarkers =
            PreImagesTruncateMarkersPerCollection::createTruncateMarkersByScanning(
                opCtx, rs, currentCollectionUUID.get(), highestSeenRecordId, highestSeenWallTime);

        auto truncateMarkers = std::make_shared<PreImagesTruncateMarkersPerCollection>(
            tenantId,
            std::move(initialSetOfMarkers.markers),
            initialSetOfMarkers.leftoverRecordsCount,
            initialSetOfMarkers.leftoverRecordsBytes,
            minBytesPerMarker);

        // Enable immediate partial marker expiration by updating the highest seen recordId and
        // wallTime in case the initialisation resulted in a partially built marker.
        truncateMarkers->updateCurrentMarkerAfterInsertOnCommit(
            opCtx, 0, highestSeenRecordId, highestSeenWallTime, 0);

        truncateMap.emplace(currentCollectionUUID.get(), truncateMarkers);
    }

    wuow.commit();
    return truncateMap;
}

using PreImagesCollectionTruncateMap =
    ConcurrentSharedValuesMap<UUID, PreImagesTruncateMarkersPerCollection, UUID::Hash>;
using TenantToPreImagesCollectionTruncateMap =
    ConcurrentSharedValuesMap<boost::optional<TenantId>, PreImagesCollectionTruncateMap>;

// TODO SERVER-76586: Finalize initialisation methods for constructing truncate markers for
// pre-images.
void initialisePreImagesCollectionTruncateMarkers(
    OperationContext* opCtx,
    boost::optional<TenantId> tenantId,
    const CollectionPtr* preImagesCollectionPtr,
    PreImagesCollectionTruncateMap& finalTruncateMap) {

    // Initialisation requires scanning/sampling the underlying pre-images collection. To reduce the
    // amount of time writes are blocked trying to access the 'finalTruncateMap', first create a
    // temporary map whose contents will eventually replace those of the 'finalTruncateMap'.
    auto initTruncateMap =
        createInitialPreImagesCollectionMapScanning(opCtx, tenantId, preImagesCollectionPtr);

    finalTruncateMap.updateWith([&](const Map& oldMap) {
        // Critical section where no other threads can modify the 'finalTruncateMap'.

        // While scanning/sampling the pre-images collection, it's possible additional entries and
        // collections were added to the 'oldMap'.
        //
        // If the 'oldMap' contains markers for a collection UUID not captured in the
        // 'initTruncateMap', it is safe to append them to the final map.
        //
        // However, if both 'oldMap' and the 'initTruncateMap' contain truncate markers for the same
        // collection UUID, ignore the entries captured in the 'oldMap' since this initialisation is
        // best effort and is prohibited from blocking writes during startup.
        for (const auto& [nsUUID, nsTruncateMarkers] : oldMap) {
            if (initTruncateMap.find(nsUUID) == initTruncateMap.end()) {
                initTruncateMap.emplace(nsUUID, nsTruncateMarkers);
            }
        }
        return initTruncateMap;
    });
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
        _tenantTruncateMarkersMap.erase(tenantId);
    }
}

void ChangeStreamPreImagesCollectionManager::insertPreImage(OperationContext* opCtx,
                                                            boost::optional<TenantId> tenantId,
                                                            const ChangeStreamPreImage& preImage) {
    tassert(6646200,
            "Expected to be executed in a write unit of work",
            opCtx->lockState()->inAWriteUnitOfWork());
    tassert(5869404,
            str::stream() << "Invalid pre-images document applyOpsIndex: "
                          << preImage.getId().getApplyOpsIndex(),
            preImage.getId().getApplyOpsIndex() >= 0);

    const auto preImagesCollectionNamespace = NamespaceString::makePreImageCollectionNSS(
        change_stream_serverless_helpers::resolveTenantId(tenantId));

    // This lock acquisition can block on a stronger lock held by another operation modifying
    // the pre-images collection. There are no known cases where an operation holding an
    // exclusive lock on the pre-images collection also waits for oplog visibility.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
    AutoGetCollection preImagesCollectionRaii(
        opCtx, preImagesCollectionNamespace, LockMode::MODE_IX);
    auto& changeStreamPreImagesCollection = preImagesCollectionRaii.getCollection();
    if (preImagesCollectionNamespace.tenantId() &&
        !change_stream_serverless_helpers::isChangeStreamEnabled(
            opCtx, *preImagesCollectionNamespace.tenantId())) {
        return;
    }
    tassert(6646201,
            "The change stream pre-images collection is not present",
            changeStreamPreImagesCollection);

    auto insertStatement = InsertStatement{preImage.toBSON()};
    const auto insertionStatus = collection_internal::insertDocument(
        opCtx, changeStreamPreImagesCollection, insertStatement, &CurOp::get(opCtx)->debug());
    tassert(5868601,
            str::stream() << "Attempted to insert a duplicate document into the pre-images "
                             "collection. Pre-image id: "
                          << preImage.getId().toBSON().toString(),
            insertionStatus != ErrorCodes::DuplicateKey);
    uassertStatusOK(insertionStatus);

    // If the map for this tenant's 'pre-images' collection doesn't exist yet, it will be lazily
    // initialised when its time to delete expired documents.
    auto preImagesCollectionTruncateMap =
        useUnreplicatedTruncates() ? _tenantTruncateMarkersMap.find(tenantId) : nullptr;
    auto bytesInserted = insertStatement.doc.objsize();

    if (preImagesCollectionTruncateMap && bytesInserted > 0) {
        updateTruncateMarkersOnInsert(
            opCtx, tenantId, preImage, bytesInserted, preImagesCollectionTruncateMap);
    }
}

void ChangeStreamPreImagesCollectionManager::performExpiredChangeStreamPreImagesRemovalPass(
    Client* client) {
    Timer timer;

    Date_t currentTimeForTimeBasedExpiration =
        change_stream_pre_image_util::getCurrentTimeForPreImageRemoval();

    const auto startTime = Date_t::now();
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = client->makeOperationContext();
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
                // A serverless enviornment is enabled and removal logic must take the tenantId into
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
    const ScopedCollectionAcquisition& preImageColl,
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
            NamespaceString::makePreImageCollectionNSS(boost::none).ns(),
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
            opCtx, DatabaseName::kConfig.toString())) {
        return 0;
    }

    // Get the timestamp of the earliest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    const auto preImageExpirationTime = change_stream_pre_image_util::getPreImageExpirationTime(
        opCtx, currentTimeForTimeBasedExpiration);

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
            opCtx, DatabaseName::kConfig.toString())) {
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
    const auto preImagesColl = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::makePreImageCollectionNSS(tenantId),
                                     PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);


    if (!preImagesColl.exists()) {
        return 0;
    }

    auto preImagesCollectionMap = _tenantTruncateMarkersMap.find(tenantId);
    if (!preImagesCollectionMap) {
        // Lazy initialisation of truncate markers to reduce the time writes are blocked
        // on startup.
        preImagesCollectionMap = _tenantTruncateMarkersMap.getOrEmplace(tenantId);
        initialisePreImagesCollectionTruncateMarkers(
            opCtx, tenantId, &preImagesColl.getCollectionPtr(), *preImagesCollectionMap);
    }

    auto snapShottedTruncateMap = preImagesCollectionMap->getUnderlyingSnapshot();
    int64_t numRecordsDeleted = 0;
    for (auto& [nsUUID, truncateMarkersForNss] : *snapShottedTruncateMap) {
        RecordIdBound minRecordId =
            change_stream_pre_image_util::getAbsoluteMinPreImageRecordIdBoundForNs(nsUUID);

        int64_t docsDeletedForNs = 0;
        int64_t bytesDeletedForNs = 0;
        Date_t maxWallTimeForNsTruncate{};
        truncateExpiredMarkersForCollection(opCtx,
                                            truncateMarkersForNss,
                                            preImagesColl.getCollectionPtr(),
                                            nsUUID,
                                            minRecordId.recordId(),
                                            docsDeletedForNs,
                                            bytesDeletedForNs,
                                            maxWallTimeForNsTruncate);

        // Best effort for removing all expired pre-images from 'nsUUID'. If there is a partial
        // marker which can be made into an expired marker, try to remove the new marker as well.
        truncateMarkersForNss->createPartialMarkerIfNecessary(opCtx);
        truncateExpiredMarkersForCollection(opCtx,
                                            truncateMarkersForNss,
                                            preImagesColl.getCollectionPtr(),
                                            nsUUID,
                                            minRecordId.recordId(),
                                            docsDeletedForNs,
                                            bytesDeletedForNs,
                                            maxWallTimeForNsTruncate);

        if (maxWallTimeForNsTruncate > _purgingJobStats.maxStartWallTime.load()) {
            _purgingJobStats.maxStartWallTime.store(maxWallTimeForNsTruncate);
        }
        _purgingJobStats.docsDeleted.fetchAndAddRelaxed(docsDeletedForNs);
        _purgingJobStats.bytesDeleted.fetchAndAddRelaxed(bytesDeletedForNs);
        _purgingJobStats.scannedInternalCollections.fetchAndAddRelaxed(1);
    }

    _purgingJobStats.scannedCollections.fetchAndAddRelaxed(1);
    return numRecordsDeleted;
}

}  // namespace mongo
