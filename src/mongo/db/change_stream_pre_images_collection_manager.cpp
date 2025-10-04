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
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPreimagesCollectionCreation);

const auto getPreImagesCollectionManager =
    ServiceContext::declareDecoration<ChangeStreamPreImagesCollectionManager>();
}  // namespace

BSONObj ChangeStreamPreImagesCollectionManager::PurgingJobStats::toBSON() const {
    BSONObjBuilder builder;
    builder.append("totalPass", totalPass.loadRelaxed())
        .append("docsDeleted", docsDeleted.loadRelaxed())
        .append("bytesDeleted", bytesDeleted.loadRelaxed())
        .append("scannedCollections", scannedCollections.loadRelaxed())
        .append("scannedInternalCollections", scannedInternalCollections.loadRelaxed())
        .append("maxTimestampEligibleForTruncate", maxTimestampEligibleForTruncate.loadRelaxed())
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

void ChangeStreamPreImagesCollectionManager::createPreImagesCollection(OperationContext* opCtx) {
    uassert(5868501,
            "Failpoint failPreimagesCollectionCreation enabled. Throwing exception",
            !MONGO_unlikely(failPreimagesCollectionCreation.shouldFail()));
    const auto preImagesCollectionNamespace = NamespaceString::kChangeStreamPreImagesNamespace;

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

void ChangeStreamPreImagesCollectionManager::insertPreImage(OperationContext* opCtx,
                                                            const ChangeStreamPreImage& preImage) {
    tassert(6646200,
            "Expected to be executed in a write unit of work",
            shard_role_details::getLocker(opCtx)->inAWriteUnitOfWork());
    tassert(5869404,
            str::stream() << "Invalid pre-images document applyOpsIndex: "
                          << preImage.getId().getApplyOpsIndex(),
            preImage.getId().getApplyOpsIndex() >= 0);

    const auto preImagesCollectionNamespace = NamespaceString::kChangeStreamPreImagesNamespace;

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
                                     AcquisitionPrerequisites::kUnreplicatedWrite),
        MODE_IX);

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

    auto bytesInserted = insertStatement.doc.objsize();
    _truncateManager.updateMarkersOnInsert(opCtx, preImage, bytesInserted);
}

void ChangeStreamPreImagesCollectionManager::performExpiredChangeStreamPreImagesRemovalPass(
    Client* client) {
    Timer timer;

    const auto startTime = Date_t::now();
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = client->makeOperationContext();
        size_t numberOfRemovals = _deleteExpiredPreImagesWithTruncate(opCtx.get());

        if (numberOfRemovals > 0) {
            LOGV2_DEBUG(5869104,
                        1,
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

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithTruncate(
    OperationContext* opCtx) {
    const auto truncateStats = _truncateManager.truncateExpiredPreImages(opCtx);

    _purgingJobStats.maxTimestampEligibleForTruncate.store(
        truncateStats.maxTimestampEligibleForTruncate);

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
