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
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/curop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

// Fail point to set current time for time-based expiration of pre-images.
MONGO_FAIL_POINT_DEFINE(changeStreamPreImageRemoverCurrentTime);
MONGO_FAIL_POINT_DEFINE(failPreimagesCollectionCreation);
}  // namespace

namespace change_stream_pre_image_helpers {

// Get the 'expireAfterSeconds' from the 'ChangeStreamOptions' if not 'off', boost::none otherwise.
boost::optional<std::int64_t> getExpireAfterSecondsFromChangeStreamOptions(
    ChangeStreamOptions& changeStreamOptions) {
    const stdx::variant<std::string, std::int64_t>& expireAfterSeconds =
        changeStreamOptions.getPreAndPostImages().getExpireAfterSeconds();

    if (!stdx::holds_alternative<std::string>(expireAfterSeconds)) {
        return stdx::get<std::int64_t>(expireAfterSeconds);
    }

    return boost::none;
}

// Returns pre-images expiry time in milliseconds since the epoch time if configured, boost::none
// otherwise.
boost::optional<Date_t> getPreImageExpirationTime(OperationContext* opCtx, Date_t currentTime) {
    invariant(!change_stream_serverless_helpers::isChangeCollectionsModeActive());
    boost::optional<std::int64_t> expireAfterSeconds = boost::none;

    // Get the expiration time directly from the change stream manager.
    auto changeStreamOptions = ChangeStreamOptionsManager::get(opCtx).getOptions(opCtx);
    expireAfterSeconds = getExpireAfterSecondsFromChangeStreamOptions(changeStreamOptions);

    // A pre-image is eligible for deletion if:
    //   pre-image's op-time + expireAfterSeconds  < currentTime.
    return expireAfterSeconds ? boost::optional<Date_t>(currentTime - Seconds(*expireAfterSeconds))
                              : boost::none;
}

// TODO SERVER-74981: Investigate whether there is a safer way to extract the Timestamp.
Timestamp getPreImageTimestamp(const RecordId& rid) {
    static constexpr auto kTopLevelFieldName = "ridAsBSON"_sd;
    auto ridAsNestedBSON = record_id_helpers::toBSONAs(rid, kTopLevelFieldName);
    // 'toBSONAs()' discards type bits of the underlying KeyString of the RecordId. However, since
    // the 'ts' field of 'ChangeStreamPreImageId' is distinct CType::kTimestamp, type bits aren't
    // necessary to obtain the original value.

    auto ridBSON = ridAsNestedBSON.getObjectField(kTopLevelFieldName);
    auto tsElem = ridBSON.getField(ChangeStreamPreImageId::kTsFieldName);
    return tsElem.timestamp();
}

RecordId toRecordId(ChangeStreamPreImageId id) {
    return record_id_helpers::keyForElem(
        BSON(ChangeStreamPreImage::kIdFieldName << id.toBSON()).firstElement());
}

}  // namespace change_stream_pre_image_helpers

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
                          << preImagesCollectionNamespace.toStringWithTenantId()
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
                          << preImagesCollectionNamespace.toStringWithTenantId()
                          << causedBy(status.reason()),
            status.isOK() || status.code() == ErrorCodes::NamespaceNotFound);
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

    const auto insertionStatus =
        collection_internal::insertDocument(opCtx,
                                            changeStreamPreImagesCollection,
                                            InsertStatement{preImage.toBSON()},
                                            &CurOp::get(opCtx)->debug());
    tassert(5868601,
            str::stream() << "Attempted to insert a duplicate document into the pre-images "
                             "collection. Pre-image id: "
                          << preImage.getId().toBSON().toString(),
            insertionStatus != ErrorCodes::DuplicateKey);
    uassertStatusOK(insertionStatus);
}

namespace {
/**
 * Finds the next collection UUID in the change stream pre-images collection 'preImagesCollPtr' for
 * which collection UUID is greater than 'collectionUUID'. Returns boost::none if the next
 * collection is not found.
 */
boost::optional<UUID> findNextCollectionUUID(OperationContext* opCtx,
                                             const CollectionPtr* preImagesCollPtr,
                                             boost::optional<UUID> collectionUUID

) {
    BSONObj preImageObj;
    auto minRecordId = collectionUUID
        ? boost::make_optional(
              RecordIdBound(change_stream_pre_image_helpers::toRecordId(ChangeStreamPreImageId(
                  *collectionUUID, Timestamp::max(), std::numeric_limits<int64_t>::max()))))
        : boost::none;
    auto planExecutor =
        InternalPlanner::collectionScan(opCtx,
                                        preImagesCollPtr,
                                        PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                        InternalPlanner::Direction::FORWARD,
                                        boost::none /* resumeAfterRecordId */,
                                        std::move(minRecordId));
    if (planExecutor->getNext(&preImageObj, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }
    auto parsedUUID = UUID::parse(preImageObj["_id"].Obj()["nsUUID"]);
    tassert(7027400, "Pre-image collection UUID must be of UUID type", parsedUUID.isOK());
    return {std::move(parsedUUID.getValue())};
}

/**
 * Scans the 'config.system.preimages' collection and deletes the expired pre-images from it.
 *
 * Pre-images are ordered by collection UUID, ie. if UUID of collection A is ordered before UUID of
 * collection B, then pre-images of collection A will be stored before pre-images of collection B.
 *
 * Pre-images are considered expired based on expiration parameter. In case when expiration
 * parameter is not set a pre-image is considered expired if its timestamp is smaller than the
 * timestamp of the earliest oplog entry. In case when expiration parameter is specified, aside from
 * timestamp check a check on the wall clock time of the pre-image recording ('operationTime') is
 * performed. If the difference between 'currentTimeForTimeBasedExpiration' and 'operationTime' is
 * larger than expiration parameter, the pre-image is considered expired. One of those two
 * conditions must be true for a pre-image to be eligible for deletion.
 *
 *                               +-------------------------+
 *                               | config.system.preimages |
 *                               +------------+------------+
 *                                            |
 *             +--------------------+---------+---------+-----------------------+
 *             |                    |                   |                       |
 * +-----------+-------+ +----------+--------+ +--------+----------+ +----------+--------+
 * |  collA.preImageA  | |  collA.preImageB  | |  collB.preImageC  | |  collB.preImageD  |
 * +-----------+-------+ +----------+--------+ +---------+---------+ +----------+--------+
 * |   timestamp: 1    | |   timestamp: 10   | |   timestamp: 5    | |   timestamp: 9    |
 * |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 0   | |   applyIndex: 1   |
 * +-------------------+ +-------------------+ +-------------------+ +-------------------+
 */
size_t _deleteExpiredChangeStreamPreImagesCommon(OperationContext* opCtx,
                                                 const CollectionPtr& preImageColl,
                                                 const MatchExpression* filterPtr,
                                                 Timestamp maxRecordIdTimestamp) {
    size_t numberOfRemovals = 0;
    boost::optional<UUID> currentCollectionUUID = boost::none;
    while ((currentCollectionUUID =
                findNextCollectionUUID(opCtx, &preImageColl, currentCollectionUUID))) {
        writeConflictRetry(
            opCtx,
            "ChangeStreamExpiredPreImagesRemover",
            NamespaceString::makePreImageCollectionNSS(boost::none).ns(),
            [&] {
                auto params = std::make_unique<DeleteStageParams>();
                params->isMulti = true;

                std::unique_ptr<BatchedDeleteStageParams> batchedDeleteParams;
                batchedDeleteParams = std::make_unique<BatchedDeleteStageParams>();
                RecordIdBound minRecordId(change_stream_pre_image_helpers::toRecordId(
                    ChangeStreamPreImageId(*currentCollectionUUID, Timestamp(), 0)));
                RecordIdBound maxRecordId =
                    RecordIdBound(change_stream_pre_image_helpers::toRecordId(
                        ChangeStreamPreImageId(*currentCollectionUUID,
                                               maxRecordIdTimestamp,
                                               std::numeric_limits<int64_t>::max())));

                auto exec = InternalPlanner::deleteWithCollectionScan(
                    opCtx,
                    &preImageColl,
                    std::move(params),
                    PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                    InternalPlanner::Direction::FORWARD,
                    std::move(minRecordId),
                    std::move(maxRecordId),
                    CollectionScanParams::ScanBoundInclusion::kIncludeBothStartAndEndRecords,
                    std::move(batchedDeleteParams),
                    filterPtr,
                    filterPtr != nullptr);
                numberOfRemovals += exec->executeDelete();
            });
    }
    return numberOfRemovals;
}

size_t deleteExpiredChangeStreamPreImages(OperationContext* opCtx,
                                          Date_t currentTimeForTimeBasedExpiration) {
    // Acquire intent-exclusive lock on the change collection.
    AutoGetCollection preImageColl(
        opCtx, NamespaceString::makePreImageCollectionNSS(boost::none), MODE_IX);

    // Early exit if the collection doesn't exist or running on a secondary.
    if (!preImageColl ||
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesForDatabase(
            opCtx, DatabaseName::kConfig.toString())) {
        return 0;
    }

    // Get the timestamp of the earliest oplog entry.
    const auto currentEarliestOplogEntryTs =
        repl::StorageInterface::get(opCtx->getServiceContext())->getEarliestOplogTimestamp(opCtx);

    const auto preImageExpirationTime = change_stream_pre_image_helpers::getPreImageExpirationTime(
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
        return _deleteExpiredChangeStreamPreImagesCommon(
            opCtx, *preImageColl, &filter, Timestamp::max() /* maxRecordIdTimestamp */);
    }

    // 'preImageExpirationTime' is not set, so the last expired pre-image timestamp is less than
    // 'currentEarliestOplogEntryTs'.
    return _deleteExpiredChangeStreamPreImagesCommon(
        opCtx,
        *preImageColl,
        nullptr /* filterPtr */,
        Timestamp(currentEarliestOplogEntryTs.asULL() - 1) /* maxRecordIdTimestamp */);
}

size_t deleteExpiredChangeStreamPreImagesForTenants(OperationContext* opCtx,
                                                    const TenantId& tenantId,
                                                    Date_t currentTimeForTimeBasedExpiration) {

    // Acquire intent-exclusive lock on the change collection.
    AutoGetCollection preImageColl(opCtx,
                                   NamespaceString::makePreImageCollectionNSS(
                                       change_stream_serverless_helpers::resolveTenantId(tenantId)),
                                   MODE_IX);

    // Early exit if the collection doesn't exist or running on a secondary.
    if (!preImageColl ||
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
    return _deleteExpiredChangeStreamPreImagesCommon(
        opCtx, *preImageColl, &filter, Timestamp::max() /* maxRecordIdTimestamp */);
}
}  // namespace

void ChangeStreamPreImagesCollectionManager::performExpiredChangeStreamPreImagesRemovalPass(
    Client* client) {
    Date_t currentTimeForTimeBasedExpiration = Date_t::now();

    changeStreamPreImageRemoverCurrentTime.execute([&](const BSONObj& data) {
        // Populate the current time for time based expiration of pre-images.
        if (auto currentTimeElem = data["currentTimeForTimeBasedExpiration"]) {
            const BSONType bsonType = currentTimeElem.type();
            tassert(5869300,
                    str::stream() << "Expected type for 'currentTimeForTimeBasedExpiration' is "
                                     "'date', but found: "
                                  << bsonType,
                    bsonType == BSONType::Date);

            currentTimeForTimeBasedExpiration = currentTimeElem.Date();
        }
    });

    const auto startTime = Date_t::now();
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = client->makeOperationContext();
        size_t numberOfRemovals = 0;

        if (change_stream_serverless_helpers::isChangeCollectionsModeActive()) {
            const auto tenantIds =
                change_stream_serverless_helpers::getConfigDbTenants(opCtx.get());
            for (const auto& tenantId : tenantIds) {
                numberOfRemovals += deleteExpiredChangeStreamPreImagesForTenants(
                    opCtx.get(), tenantId, currentTimeForTimeBasedExpiration);
            }
        } else {
            numberOfRemovals =
                deleteExpiredChangeStreamPreImages(opCtx.get(), currentTimeForTimeBasedExpiration);
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
}

}  // namespace mongo
