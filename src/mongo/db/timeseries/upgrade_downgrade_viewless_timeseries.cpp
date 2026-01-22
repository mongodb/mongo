/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/timeseries/upgrade_downgrade_viewless_timeseries.h"

#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace timeseries {

struct UpgradeDowngradeTimeseriesLocks {
    AutoGetCollection mainColl;
    AutoGetCollection bucketsColl;
    boost::optional<Lock::CollectionLock> systemViewsLock;
};

UpgradeDowngradeTimeseriesLocks acquireLocksForTimeseriesUpgradeDowngrade(
    OperationContext* opCtx, const NamespaceString& mainNs, LockMode lockMode = MODE_X) {
    LOGV2_DEBUG(11450504,
                1,
                "Locking collection for viewless timeseries upgrade/downgrade",
                logAttrs(mainNs));
    tassert(11450505,
            "Expected 'mainNs' to not be a system.buckets namespace",
            !mainNs.isTimeseriesBucketsCollection());

    for (size_t numAttempts = 0;; numAttempts++) {
        try {
            // Maximum time to acquire the locks over all affected entities. We do this since:
            // - Upgrade/downgrade is a low priority task, so guard against it holding locks for
            //   too long.
            // - Ensure that by locking all affected namespaces (buckets NS + main NS +
            //   system.views) we do not cause a deadlock.
            static const auto LOCK_TIMEOUT = Seconds(30);
            auto lockDeadline = Date_t::now() + LOCK_TIMEOUT;

            // Acquire locks over the affected namespaces. We use AutoGetCollection (rather than
            // acquireCollections) because the buckets collection must be locked first, rather than
            // following the canonical order (increasing ResourceId).
            // AutoGetCollection also doesn't open a storage snapshot, which would break the catalog
            // iteration method used in `forEachTimeseriesCollectionFromDb`.
            AutoGetCollection bucketsColl(opCtx,
                                          mainNs.makeTimeseriesBucketsNamespace(),
                                          lockMode,
                                          auto_get_collection::Options{}.deadline(lockDeadline));
            AutoGetCollection mainColl(opCtx,
                                       mainNs,
                                       lockMode,
                                       auto_get_collection::Options{}
                                           .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                           .deadline(lockDeadline));

            // Locking system.views is needed to create or drop the timeseries view.
            // Operations all lock system.views in the end to prevent deadlock.
            boost::optional<Lock::CollectionLock> systemViewsLock;
            if (auto db = mainColl.getDb()) {
                systemViewsLock.emplace(opCtx, db->getSystemViewsName(), lockMode, lockDeadline);
            }

            return UpgradeDowngradeTimeseriesLocks{
                .mainColl = std::move(mainColl),
                .bucketsColl = std::move(bucketsColl),
                .systemViewsLock = std::move(systemViewsLock),
            };
        } catch (const ExceptionFor<ErrorCodes::LockTimeout>&) {
            // This may happen while long running operations (e.g. dbHash) hold a collection
            // lock, preventing the acquisition for upgrade/downgrade.
            // Back off and retry it (instead of propagating the error to consumers like oplog
            // application, which would trigger in a fatal assertion).
            // TODO(SERVER-115831): Review the lock with deadline + indefinite retry strategy.
            logAndBackoff(11450503,
                          MONGO_LOGV2_DEFAULT_COMPONENT,
                          logv2::LogSeverity::Info(),
                          numAttempts,
                          "Timed out acquiring locks for timeseries upgrade/downgrade, retrying");
        }
    }
}

Status canUpgradeToViewlessTimeseries(OperationContext* opCtx,
                                      const UpgradeDowngradeTimeseriesLocks& locks) {
    const auto& mainColl = locks.mainColl;
    const auto& mainNs = mainColl.getNss();
    const auto& bucketsColl = locks.bucketsColl;

    if (!gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
            VersionContext::getDecoration(opCtx))) {
        return Status(ErrorCodes::IllegalOperation,
                      "Cannot upgrade to viewless timeseries without the feature flag enabled");
    }

    // Idempotency check: already in viewless format
    if (mainColl && mainColl->isTimeseriesCollection() && mainColl->isNewTimeseriesWithoutView() &&
        !bucketsColl) {
        return Status::OK();
    }

    // Buckets collection must exist
    if (!bucketsColl) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Cannot upgrade to viewless timeseries: buckets collection "
                                    << mainNs.makeTimeseriesBucketsNamespace().toStringForErrorMsg()
                                    << " not found");
    }

    // Buckets collection must have valid timeseries options
    if (!bucketsColl->isTimeseriesCollection() || bucketsColl->isNewTimeseriesWithoutView()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot upgrade to viewless timeseries: buckets collection "
                                    << mainNs.makeTimeseriesBucketsNamespace().toStringForErrorMsg()
                                    << " does not have valid timeseries options");
    }

    // Check for metadata inconsistencies. Return an error so the coordinator can handle it.
    auto inconsistencies = checkBucketCollectionInconsistencies(
        opCtx, *bucketsColl, false /* ensureViewExists */, mainColl.getView(), (*mainColl).get());
    if (!inconsistencies.empty()) {
        for (const auto& inconsistency : inconsistencies) {
            LOGV2(11628800,
                  "Timeseries upgrade validation found bucket metadata inconsistency",
                  logAttrs(mainNs),
                  "issue"_attr = inconsistency.issue,
                  "options"_attr = inconsistency.options);
        }
        return Status(ErrorCodes::UserDataInconsistent,
                      str::stream()
                          << "Cannot upgrade to viewless timeseries: metadata inconsistency found "
                             "for collection "
                          << mainNs.toStringForErrorMsg());
    }

    return Status::OK();
}

Status canUpgradeToViewlessTimeseries(OperationContext* opCtx, const NamespaceString& mainNs) {
    // Use MODE_IS for validation since we only need to read, not modify.
    auto locks = acquireLocksForTimeseriesUpgradeDowngrade(opCtx, mainNs, MODE_IS);
    return canUpgradeToViewlessTimeseries(opCtx, locks);
}

Status canDowngradeFromViewlessTimeseries(OperationContext* opCtx,
                                          const UpgradeDowngradeTimeseriesLocks& locks,
                                          bool skipViewCreation) {
    const auto& mainColl = locks.mainColl;
    const auto& mainNs = mainColl.getNss();
    const auto& bucketsColl = locks.bucketsColl;

    if (gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
            VersionContext::getDecoration(opCtx))) {
        return Status(ErrorCodes::IllegalOperation,
                      "Cannot downgrade from viewless timeseries with the feature flag enabled");
    }

    // Idempotency check: already in viewful format.
    if (bucketsColl && bucketsColl->isTimeseriesCollection() &&
        !bucketsColl->isNewTimeseriesWithoutView() && !mainColl) {
        // Verify view exists (unless skipViewCreation is set for non-primary shards)
        if (!skipViewCreation && (!mainColl.getView() || !mainColl.getView()->timeseries())) {
            return Status(ErrorCodes::NamespaceNotFound,
                          str::stream()
                              << "Already downgraded but missing expected timeseries view for "
                              << mainNs.toStringForErrorMsg());
        }
        return Status::OK();
    }

    // Viewless collection must exist
    if (!mainColl) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Cannot downgrade from viewless timeseries: collection "
                                    << mainNs.toStringForErrorMsg() << " not found");
    }

    // Collection must be a viewless timeseries
    if (!mainColl->isTimeseriesCollection() || !mainColl->isNewTimeseriesWithoutView()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot downgrade from viewless timeseries: collection "
                                    << mainNs.toStringForErrorMsg()
                                    << " is not a viewless timeseries collection");
    }

    // No conflicting buckets collection
    if (bucketsColl) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream()
                          << "Cannot downgrade from viewless timeseries: conflicting buckets "
                             "collection exists at "
                          << mainNs.makeTimeseriesBucketsNamespace().toStringForErrorMsg());
    }

    return Status::OK();
}

Status canDowngradeFromViewlessTimeseries(OperationContext* opCtx,
                                          const NamespaceString& mainNs,
                                          bool skipViewCreation) {
    // Use MODE_IS for validation since we only need to read, not modify.
    auto locks = acquireLocksForTimeseriesUpgradeDowngrade(opCtx, mainNs, MODE_IS);
    return canDowngradeFromViewlessTimeseries(opCtx, locks, skipViewCreation);
}

void upgradeToViewlessTimeseries(OperationContext* opCtx,
                                 UpgradeDowngradeTimeseriesLocks&& locks,
                                 const boost::optional<UUID>& expectedUUID) {
    const auto& mainColl = locks.mainColl;
    const auto& mainNs = mainColl.getNss();
    const auto& bucketsColl = locks.bucketsColl;
    const auto& bucketsNs = bucketsColl.getNss();
    auto db = mainColl.getDb();

    LOGV2(11483000, "Started upgrade to viewless timeseries", logAttrs(mainNs));

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);

    writeConflictRetry(opCtx, "viewlessTimeseriesUpgrade", mainNs, [&] {
        // Validate upgrade preconditions (including idempotency check)
        auto canUpgradeStatus = canUpgradeToViewlessTimeseries(opCtx, locks);
        if (canUpgradeStatus.code() == ErrorCodes::UserDataInconsistent) {
            // Metadata inconsistency found - skip upgrade (already logged by canUpgrade)
            return;
        }
        tassert(11483004, canUpgradeStatus.reason(), canUpgradeStatus.isOK());

        // Idempotency check
        if (mainColl && mainColl->isTimeseriesCollection() &&
            mainColl->isNewTimeseriesWithoutView() && !bucketsColl) {
            // TODO(SERVER-114517): Investigate if we should relax this check.
            tassert(11483003,
                    "Found an already upgraded timeseries collection but with an unexpected UUID",
                    !expectedUUID || mainColl->uuid() == *expectedUUID);
            return;
        }

        // TODO(SERVER-114517): Investigate if we should relax this check.
        tassert(11483005,
                "The buckets collection to upgrade does not have the expected UUID",
                !expectedUUID || bucketsColl->uuid() == *expectedUUID);

        WriteUnitOfWork wuow(opCtx);

        // Run the timeseries upgrade steps without generating oplog entries.
        {
            repl::UnreplicatedWritesBlock uwb(opCtx);

            // Drop view and rename the buckets NSS over it.
            uassertStatusOK(db->dropView(opCtx, mainNs));
            uassertStatusOK(db->renameCollection(opCtx, bucketsNs, mainNs, true /* stayTemp */));

            // Clean up the buckets NSS metadata.
            CollectionWriter collWriter{opCtx, mainNs};
            Collection* writableColl = collWriter.getWritableCollection(opCtx);
            uassertStatusOK(writableColl->updateValidator(opCtx,
                                                          BSONObj() /* newValidator */,
                                                          boost::none /* validationLevel */,
                                                          boost::none /* validatorAction */));
        }

        // Log a oplog entry giving a single, atomic timestamp to all operations done above.
        opCtx->getServiceContext()->getOpObserver()->onUpgradeDowngradeViewlessTimeseries(
            opCtx, mainNs, bucketsColl->uuid());

        wuow.commit();
    });

    LOGV2(11483008, "Finished upgrade to viewless timeseries", logAttrs(mainNs));
}

void upgradeToViewlessTimeseries(OperationContext* opCtx,
                                 const NamespaceString& mainNs,
                                 const boost::optional<UUID>& expectedUUID) {
    auto locks = acquireLocksForTimeseriesUpgradeDowngrade(opCtx, mainNs);
    upgradeToViewlessTimeseries(opCtx, std::move(locks), expectedUUID);
}

void downgradeFromViewlessTimeseries(OperationContext* opCtx,
                                     UpgradeDowngradeTimeseriesLocks&& locks,
                                     const boost::optional<UUID>& expectedUUID,
                                     bool skipViewCreation) {
    const auto& mainColl = locks.mainColl;
    const auto& mainNs = mainColl.getNss();
    const auto& bucketsColl = locks.bucketsColl;
    const auto& bucketsNs = bucketsColl.getNss();
    auto db = mainColl.getDb();

    LOGV2(11483009,
          "Started downgrade of timeseries collection format",
          logAttrs(mainNs),
          "skipViewCreation"_attr = skipViewCreation);

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);

    writeConflictRetry(opCtx, "viewlessTimeseriesDowngrade", mainNs, [&] {
        // Validate downgrade preconditions
        auto canDowngradeStatus =
            canDowngradeFromViewlessTimeseries(opCtx, locks, skipViewCreation);
        tassert(11483013, canDowngradeStatus.reason(), canDowngradeStatus.isOK());

        // Idempotency check: already downgraded to viewful format
        if (bucketsColl && bucketsColl->isTimeseriesCollection() &&
            !bucketsColl->isNewTimeseriesWithoutView() && !mainColl) {
            // TODO(SERVER-114517): Investigate if we should relax this check.
            tassert(11483012,
                    "Found an already downgraded timeseries collection but with unexpected UUID",
                    !expectedUUID || bucketsColl->uuid() == *expectedUUID);
            return;
        }

        // TODO(SERVER-114517): Investigate if we should relax this check.
        tassert(11483014,
                "The viewless collection to downgrade does not have the expected UUID",
                !expectedUUID || mainColl->uuid() == *expectedUUID);

        // Create system.views if it does not exist. This is done in a separate WUOW.
        // Only needed if we're creating the view.
        if (!skipViewCreation) {
            db->createSystemDotViewsIfNecessary(opCtx);
        }

        WriteUnitOfWork wuow(opCtx);

        // Run the timeseries downgrade steps without generating oplog entries.
        {
            repl::UnreplicatedWritesBlock uwb(opCtx);

            // Rename the collection to the buckets NSS.
            uassertStatusOK(db->renameCollection(opCtx, mainNs, bucketsNs, true /* stayTemp */));

            // Only create the view on the primary shard.
            if (!skipViewCreation) {
                CollectionOptions viewOptions{};
                viewOptions.viewOn = std::string{bucketsNs.coll()};
                viewOptions.collation = mainColl->getCollectionOptions().collation;
                constexpr bool asArray = true;
                viewOptions.pipeline =
                    timeseries::generateViewPipeline(*mainColl->getTimeseriesOptions(), asArray);
                uassertStatusOK(
                    db->userCreateNS(opCtx, mainNs, viewOptions, /*createIdIndex=*/false));
            }

            // Add validator to the buckets collection.
            CollectionWriter collWriter{opCtx, bucketsNs};
            Collection* writableColl = collWriter.getWritableCollection(opCtx);
            auto timeField = mainColl->getTimeseriesOptions()->getTimeField();
            int bucketVersion = timeseries::kTimeseriesControlLatestVersion;
            uassertStatusOK(writableColl->updateValidator(
                opCtx,
                timeseries::generateTimeseriesValidator(bucketVersion, timeField),
                boost::none /* validationLevel */,
                boost::none /* validatorAction */));
        }

        // Log a oplog entry giving a single, atomic timestamp to all operations done above.
        opCtx->getServiceContext()->getOpObserver()->onUpgradeDowngradeViewlessTimeseries(
            opCtx, mainNs, mainColl->uuid(), skipViewCreation);

        wuow.commit();
    });

    LOGV2(11483017,
          "Finished downgrade of timeseries collection format",
          logAttrs(mainNs),
          "skipViewCreation"_attr = skipViewCreation);
}

void downgradeFromViewlessTimeseries(OperationContext* opCtx,
                                     const NamespaceString& mainNs,
                                     const boost::optional<UUID>& expectedUUID,
                                     bool skipViewCreation) {
    auto locks = acquireLocksForTimeseriesUpgradeDowngrade(opCtx, mainNs);
    downgradeFromViewlessTimeseries(opCtx, std::move(locks), expectedUUID, skipViewCreation);
}

/**
 * Iterate over all viewful or viewless timeseries collections in the shard catalog.
 * This acts similarly to `catalog::forEachCollectionFromDb`, but unlike it, it will
 * lock both the main and buckets namespaces of the timeseries collection (in MODE_X).
 */
template <typename F>
void forEachTimeseriesCollectionFromDb(OperationContext* opCtx,
                                       const DatabaseName& dbName,
                                       bool isViewless,
                                       F&& callback) {
    auto catalogForIteration = CollectionCatalog::get(opCtx);
    for (auto&& coll : catalogForIteration->range(dbName)) {
        auto uuid = coll->uuid();
        if (!catalogForIteration->checkIfCollectionSatisfiable(
                uuid, [&](const Collection* collection) {
                    return collection->isTimeseriesCollection() &&
                        collection->isNewTimeseriesWithoutView() == isViewless;
                })) {
            continue;
        }

        boost::optional<UpgradeDowngradeTimeseriesLocks> locks;

        while (auto nss = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, uuid)) {
            // Lock both the buckets and main namespaces.
            auto mainNs =
                nss->isTimeseriesBucketsCollection() ? nss->getTimeseriesViewNamespace() : *nss;
            locks.emplace(acquireLocksForTimeseriesUpgradeDowngrade(opCtx, mainNs));
            const auto& acquiredColl =
                nss->isTimeseriesBucketsCollection() ? locks->bucketsColl : locks->mainColl;

            // Ensure that we didn't acquire an storage snapshot while taking locks.
            // This ensures we refresh the collection catalog to latest while following a rename.
            tassert(11450506,
                    "Expected to not have an open storage snapshot while iterating the catalog",
                    !shard_role_details::getRecoveryUnit(opCtx)->isActive());

            if (acquiredColl && acquiredColl->uuid() == uuid) {
                // Success: locked the namespace and the UUID still maps to it.
                break;
            }
            // Failed: collection got renamed before locking it, so unlock and try again.
            locks.reset();
        }

        // The NamespaceString couldn't be resolved from the uuid, so the collection was dropped.
        if (!locks.has_value())
            continue;

        callback(std::move(*locks));
    }
}

void upgradeAllTimeseriesToViewless(OperationContext* opCtx) {
    for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
        AutoGetDb autoDb(opCtx, dbName, MODE_IX);

        forEachTimeseriesCollectionFromDb(
            opCtx, dbName, false /* isViewless */, [&](UpgradeDowngradeTimeseriesLocks&& locks) {
                timeseries::upgradeToViewlessTimeseries(
                    opCtx, std::move(locks), boost::none /* expectedUUID */);
            });
    }
};

void downgradeAllTimeseriesFromViewless(OperationContext* opCtx) {
    for (const auto& dbName : DatabaseHolder::get(opCtx)->getNames()) {
        AutoGetDb autoDb(opCtx, dbName, MODE_IX);

        forEachTimeseriesCollectionFromDb(
            opCtx, dbName, true /* isViewless */, [&](UpgradeDowngradeTimeseriesLocks&& locks) {
                timeseries::downgradeFromViewlessTimeseries(opCtx,
                                                            std::move(locks),
                                                            boost::none /* expectedUUID */,
                                                            false /* skipViewCreation */);
            });
    }
}

std::vector<BucketsCollectionInconsistency> checkBucketCollectionInconsistencies(
    OperationContext* opCtx,
    const CollectionPtr& bucketsColl,
    bool ensureViewExists,
    const ViewDefinition* view,
    const Collection* mainColl) {
    std::vector<BucketsCollectionInconsistency> inconsistencies;

    tassert(11483018,
            "Expected 'bucketsColl' to exist and be a timeseries buckets namespace",
            bucketsColl && bucketsColl->ns().isTimeseriesBucketsCollection());
    const auto& nss = bucketsColl->ns();

    const std::string errMsgPrefix = str::stream()
        << nss.toStringForErrorMsg() << " is a bucket collection but is missing";

    // A bucket collection must always have timeseries options
    const bool hasTimeseriesOptions = bucketsColl->isTimeseriesCollection();
    if (!hasTimeseriesOptions) {
        const std::string errMsg = str::stream() << errMsgPrefix << " the timeseries options";
        const BSONObj options = bucketsColl->getCollectionOptions().toBSON();
        inconsistencies.emplace_back(
            BucketsCollectionInconsistency{std::move(errMsg), std::move(options)});
        return inconsistencies;
    }

    // A bucket collection on the primary shard must always be backed by a view in the proper
    // format. Check if there is a valid view, otherwise return current view/collection options (if
    // present).
    const auto [hasValidView, invalidOptions] = [&] {
        if (view) {
            if (view->viewOn() == nss && view->pipeline().size() == 1) {
                const auto expectedViewPipeline = timeseries::generateViewPipeline(
                    *bucketsColl->getTimeseriesOptions(), false /* asArray */);
                const auto expectedInternalUnpackStage =
                    expectedViewPipeline
                        .getField(DocumentSourceInternalUnpackBucket::kStageNameInternal)
                        .Obj();
                const auto actualPipeline = view->pipeline().front();
                if (actualPipeline.hasField(
                        DocumentSourceInternalUnpackBucket::kStageNameInternal)) {
                    const auto actualInternalUnpackStage =
                        actualPipeline
                            .getField(DocumentSourceInternalUnpackBucket::kStageNameInternal)
                            .Obj()
                            // Ignore `exclude` field introduced in v5.0 and removed in v5.1
                            .removeField(DocumentSourceInternalUnpackBucket::kExclude);
                    if (actualInternalUnpackStage.woCompare(expectedInternalUnpackStage) == 0) {
                        // The view is in the expected format
                        return std::make_pair(true, BSONObj());
                    }
                }
            }

            // The view is not in the expected format, return the current options for debugging
            BSONArrayBuilder pipelineArray;
            const auto& pipeline = view->pipeline();
            for (const auto& stage : pipeline) {
                pipelineArray.append(stage);
            }

            const BSONObj currentViewOptions = BSON("viewOn" << toStringForLogging(view->viewOn())
                                                             << "pipeline" << pipelineArray.arr());

            return std::make_pair(false, currentViewOptions);
        }

        if (mainColl) {
            // A collection is present rather than a view, return the current options for debugging
            return std::make_pair(false, mainColl->getCollectionOptions().toBSON());
        }

        return std::make_pair(view || !ensureViewExists, BSONObj());
    }();

    if (!hasValidView) {
        const std::string errMsg = str::stream() << errMsgPrefix << " a valid view backing it";
        inconsistencies.emplace_back(
            BucketsCollectionInconsistency{std::move(errMsg), std::move(invalidOptions)});
    }

    return inconsistencies;
}

}  // namespace timeseries
}  // namespace mongo
