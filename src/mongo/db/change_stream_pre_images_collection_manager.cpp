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
#include "mongo/db/admission/execution_control/execution_admission_context.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failPreimagesCollectionCreation);

// Failpoint for disabling pre-images removal temporarily during testing. This failpoint is used by
// the replica set data consistency checks to keep the contents of the pre-images collection stable
// for the duration of the check.
MONGO_FAIL_POINT_DEFINE(disableChangeStreamPreImagesRemover);
MONGO_FAIL_POINT_DEFINE(preImagesRemovalFailsWithNotWritablePrimary);

// Failpoint that is misused as a counter. In testing mode, the failpoint is always enabled.
// When entering the pre-images removal code in testing mode, the failpoint is always triggered,
// which increases its "timesEntered" counter. The counter is also increased every time the
// pre-images removal code is exited. This way callers can check if the pre-images removal is
// currently executing (timesEntered % 2 == 1) or not (timesEntered % 2 == 0).
// Note: this failpoint is always enabled during testing!
MONGO_FAIL_POINT_DEFINE(changeStreamPreImagesRemoverInvocationCounter);

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

ChangeStreamPreImagesCollectionManager::ChangeStreamPreImagesCollectionManager() {
    // During testing, always enable the 'changeStreamPreImagesRemoverInvocationCounter' failpoint.
    // This failpoint serves as a counter with which tests can observe from the outside if the
    // pre-images removal thread is currently executing or not.
    if (getTestCommandsEnabled()) {
        changeStreamPreImagesRemoverInvocationCounter.setMode(FailPoint::Mode::alwaysOn);
    }
}

void ChangeStreamPreImagesCollectionManager::createPreImagesCollection(OperationContext* opCtx) {
    uassert(5868501,
            "Failpoint failPreimagesCollectionCreation enabled. Throwing exception",
            !MONGO_unlikely(failPreimagesCollectionCreation.shouldFail()));

    CollectionOptions preImagesCollectionOptions;

    // Make the collection clustered by _id.
    preImagesCollectionOptions.clusteredIndex.emplace(
        clustered_util::makeCanonicalClusteredInfoForLegacyFormat());
    const auto status = createCollection(opCtx,
                                         NamespaceString::kChangeStreamPreImagesNamespace,
                                         preImagesCollectionOptions,
                                         BSONObj());
    uassert(status.code(),
            str::stream() << "Failed to create the pre-images collection: "
                          << NamespaceString::kChangeStreamPreImagesNamespace.toStringForErrorMsg()
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

    // This lock acquisition can block on a stronger lock held by another operation modifying
    // the pre-images collection. There are no known cases where an operation holding an
    // exclusive lock on the pre-images collection also waits for oplog visibility.
    AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
        shard_role_details::getLocker(opCtx));
    const auto changeStreamPreImagesCollection = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(NamespaceString::kChangeStreamPreImagesNamespace,
                                     PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
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
    Client* client, bool useReplicatedTruncates) {
    // Increase failpoint's "timesEntered" value when entering this function. This creates an uneven
    // "timesEntered" value.
    {
        changeStreamPreImagesRemoverInvocationCounter.scoped();
    }

    ON_BLOCK_EXIT([&]() {
        // Increase failpoint's "timesEntered" value leaving this function. This creates an even
        // "timesEntered" value.
        changeStreamPreImagesRemoverInvocationCounter.scoped();
    });

    // Failpoint to temporarily disable pre-images removal job for testing.
    if (MONGO_unlikely(disableChangeStreamPreImagesRemover.shouldFail())) {
        return;
    }

    Timer timer;
    const auto startTime = Date_t::now();
    ServiceContext::UniqueOperationContext opCtx;
    try {
        opCtx = client->makeOperationContext();

        // The removal job either issues local or replicated truncates, based on the capabilities of
        // the persistence provider (attached storage / disaggregated storage) OR the value of the
        // FCV-gated feature flag 'useReplicatedTruncatesForDeletions'. The feature flag value can
        // flip during FCV upgrade or downgrade.
        // Using a 'FixedOperationFCVRegion' ensures the FCV cannot change on the primary while the
        // removal is executing. There is no such protection on the secondaries, meaning that the
        // FCV and thus the value of the feature flag can change while the removal is executing on
        // secondaries. This can lead to potential inconsistencies between the primary and the
        // secondaries, which are acceptable due to the following reasoning:
        // - DSC: on DSC, the value of the feature flag is always ignored, because replicated
        //   truncates are ensured on persistence provider level. Only the current primary will ever
        //   execute the removals, and secondaries apply removals with the oplog. There are no
        //   possible inconsistencies.
        // - ASC: on ASC, the value of the feature flag is used, and it can flip its value from
        //   false to true during FCV upgrade, or from true to false during FCV downgrade.
        //   - FCV upgrade: before the FCV upgrade, all nodes execute the removal job locally, so
        //     there can already be inconsistencies between the nodes. The FCV upgrade will stop the
        //     removal job on the secondaries, which means that there will be no _new_
        //     inconsistencies after the FCV upgrade has completed and the secondaries have seen the
        //     FCV upgrade. Inconsistencies continue to exist until all nodes have stepped up once
        //     and were responsible for the removal based on their own view of the data. These
        //     inconsistencies have existed since the introduction of the local removals in v7.1,
        //     and are acceptable.
        //   - FCV downgrade: The FCV downgrade will enable local removal jobs on all nodes, which
        //     brings back possible inconsistencies anyway. This is an unavoidable side effect of
        //     using local removals.
        VersionContext::FixedOperationFCVRegion fixedFcvRegion(opCtx.get());

        // Simulate "NotWritablePrimary" error for usage in tests.
        if (MONGO_unlikely(preImagesRemovalFailsWithNotWritablePrimary.shouldFail())) {
            uasserted(ErrorCodes::NotWritablePrimary, "not primary");
        }

        // Don't execute the removal if the feature flag value has changed compared to when the job
        // was originally started.
        if (const bool currentUseReplicatedTruncates =
                change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx.get());
            currentUseReplicatedTruncates != useReplicatedTruncates) {
            LOGV2_DEBUG(
                12047100,
                2,
                "Skipping periodic change stream expired pre-images removal job because FCV-gated "
                "feature flag value for using replicated truncates changed between the "
                "installation and the execution of the job",
                "currentValue"_attr = currentUseReplicatedTruncates,
                "originalValue"_attr = useReplicatedTruncates);
            return;
        }

        LOGV2_DEBUG(12216000,
                    1,
                    "Periodic change stream expired pre-images removal job starting",
                    "useReplicatedTruncates"_attr = useReplicatedTruncates);

        // The number of removals here can be an estimate based on collection size and count
        // information, which can be inaccurate.
        if (size_t estimatedNumberOfRemovals =
                _deleteExpiredPreImagesWithTruncate(opCtx.get(), useReplicatedTruncates);
            estimatedNumberOfRemovals > 0) {
            LOGV2_DEBUG(5869104,
                        1,
                        "Periodic change stream expired pre-images removal job finished executing. "
                        "The number of removed pre-images reported is only an estimate and its "
                        "accuracy can vary.",
                        "estimatedNumberOfRemovals"_attr = estimatedNumberOfRemovals,
                        "jobDuration"_attr = (Date_t::now() - startTime).toString());
        }
    } catch (const DBException& exception) {
        Status interruptStatus = opCtx ? opCtx.get()->checkForInterruptNoAssert() : Status::OK();
        if (!interruptStatus.isOK()) {
            LOGV2_DEBUG(
                5869105,
                3,
                "Periodic change stream expired pre-images removal job operation was interrupted",
                "errorCode"_attr = interruptStatus);
        } else {
            // Turn 'NotWritablePrimary' errors into info log messages, because they are expected in
            // some situations.
            if (useReplicatedTruncates && exception.code() == ErrorCodes::NotWritablePrimary) {
                LOGV2_INFO(
                    12216001,
                    "Periodic change stream expired pre-images removal job failed because it "
                    "executed on a non-primary. This can happen temporarily during step up, step "
                    "down or FCV change.",
                    "reason"_attr = exception.reason());
            } else {
                LOGV2_ERROR(5869106,
                            "Periodic change stream expired pre-images removal job failed",
                            "errorCode"_attr = exception.codeString(),
                            "reason"_attr = exception.reason());
            }
        }
    }

    _purgingJobStats.timeElapsedMillis.fetchAndAddRelaxed(timer.millis());
    _purgingJobStats.totalPass.fetchAndAddRelaxed(1);
}

void ChangeStreamPreImagesCollectionManager::flushTruncateMarkers() {
    _truncateManager.flushTruncateMarkers();
}

size_t ChangeStreamPreImagesCollectionManager::_deleteExpiredPreImagesWithTruncate(
    OperationContext* opCtx, bool useReplicatedTruncates) {
    const auto truncateStats =
        _truncateManager.truncateExpiredPreImages(opCtx, useReplicatedTruncates);

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
