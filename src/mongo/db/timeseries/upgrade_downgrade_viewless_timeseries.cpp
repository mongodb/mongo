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

#include "mongo/db/pipeline/document_source_internal_unpack_bucket.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

namespace timeseries {

// Maximum time to acquire the locks over all affected entities. We do this since:
// - Upgrade/downgrade is a low priority task, so guard against it holding locks for too long.
// - Ensure that by locking all affected namespaces (buckets NS + main NS + system.views) we do not
//   cause a deadlock.
static const auto LOCK_TIMEOUT = Seconds(30);

void upgradeToViewlessTimeseries(OperationContext* opCtx,
                                 const NamespaceString& mainNs,
                                 const boost::optional<UUID>& expectedUUID) {
    LOGV2(11483000, "Started upgrade to viewless timeseries", logAttrs(mainNs));

    tassert(11483001,
            "Expected 'mainNs' for timeseries upgrade to not be a system.buckets namespace",
            !mainNs.isTimeseriesBucketsCollection());
    auto bucketsNs = mainNs.makeTimeseriesBucketsNamespace();

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
    tassert(11483002,
            "Tried to upgrade to viewless timeseries without the feature flag enabled",
            gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
                VersionContext::getDecoration(opCtx)));

    writeConflictRetry(opCtx, "viewlessTimeseriesUpgrade", mainNs, [&] {
        // Acquire locks over the affected namespaces. The buckets collection must be locked first.
        auto lockDeadline = Date_t::now() + LOCK_TIMEOUT;
        AutoGetDb autoDb(opCtx, mainNs.dbName(), MODE_IX, lockDeadline);
        AutoGetCollection bucketsColl(
            opCtx, bucketsNs, MODE_X, auto_get_collection::Options{}.deadline(lockDeadline));
        AutoGetCollection mainColl(opCtx,
                                   mainNs,
                                   MODE_X,
                                   auto_get_collection::Options{}
                                       .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                       .deadline(lockDeadline));

        // Idempotency check
        if (mainColl && mainColl->isTimeseriesCollection() &&
            mainColl->isNewTimeseriesWithoutView() && !bucketsColl) {
            // TODO(SERVER-114517): Investigate if we should relax this check.
            tassert(11483003,
                    "Found an already upgraded timeseries collection but with an unexpected UUID",
                    !expectedUUID || mainColl->uuid() == *expectedUUID);
            return;
        }

        // Sanity checks
        tassert(11483004,
                "Did not find the buckets collection to upgrade to viewless timeseries",
                bucketsColl);
        // TODO(SERVER-114517): Investigate if we should relax this check.
        tassert(11483005,
                "The buckets collection to upgrade does not have the expected UUID",
                !expectedUUID || bucketsColl->uuid() == *expectedUUID);
        tassert(11483006,
                "The buckets collection to upgrade does not have valid timeseries options",
                bucketsColl->isTimeseriesCollection() &&
                    !bucketsColl->isNewTimeseriesWithoutView());

        auto inconsistencies = checkBucketCollectionInconsistencies(opCtx,
                                                                    *bucketsColl,
                                                                    false /* ensureViewExists */,
                                                                    mainColl.getView(),
                                                                    (*mainColl).get());
        if (!inconsistencies.empty()) {
            for (const auto& inconsistency : inconsistencies) {
                LOGV2(
                    11483007,
                    "Skipping timeseries upgrade because we found a buckets metadata inconsistency",
                    logAttrs(mainNs),
                    "issue"_attr = inconsistency.issue,
                    "options"_attr = inconsistency.options);
            }
            return;
        }

        // Operations all lock system.views in the end to prevent deadlock.
        Lock::CollectionLock systemViewsLock(
            opCtx, autoDb.getDb()->getSystemViewsName(), MODE_X, lockDeadline);

        WriteUnitOfWork wuow(opCtx);

        // Run the timeseries upgrade steps without generating oplog entries.
        {
            repl::UnreplicatedWritesBlock uwb(opCtx);

            // Drop view and rename the buckets NSS over it.
            uassertStatusOK(autoDb.getDb()->dropView(opCtx, mainNs));
            uassertStatusOK(
                autoDb.getDb()->renameCollection(opCtx, bucketsNs, mainNs, true /* stayTemp */));

            // Clean up the buckets NSS metadata.
            CollectionWriter collWriter{opCtx, mainNs};
            Collection* writableColl = collWriter.getWritableCollection(opCtx);
            uassertStatusOK(writableColl->updateValidator(opCtx,
                                                          BSONObj() /* newValidator */,
                                                          boost::none /* validationLevel */,
                                                          boost::none /* validatorAction */));
        }

        // Log a oplog entry giving a single, atomic timestamp to all operations done above.
        // TODO(SERVER-114505): Call the OpObserver to emit an oplog entry

        wuow.commit();
    });

    LOGV2(11483008, "Finished upgrade to viewless timeseries", logAttrs(mainNs));
}

void downgradeFromViewlessTimeseries(OperationContext* opCtx,
                                     const NamespaceString& mainNs,
                                     const boost::optional<UUID>& expectedUUID) {
    LOGV2(11483009, "Started downgrade of timeseries collection format", logAttrs(mainNs));

    tassert(11483010,
            "Expected 'mainNs' for timeseries downgrade to not be a system.buckets namespace",
            !mainNs.isTimeseriesBucketsCollection());
    auto bucketsNs = mainNs.makeTimeseriesBucketsNamespace();

    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
    tassert(11483011,
            "Tried to downgrade from viewless timeseries with the feature flag enabled",
            !gFeatureFlagCreateViewlessTimeseriesCollections.isEnabled(
                VersionContext::getDecoration(opCtx)));

    writeConflictRetry(opCtx, "viewlessTimeseriesDowngrade", mainNs, [&] {
        // Acquire locks over the affected namespaces. The buckets collection must be locked first.
        auto lockDeadline = Date_t::now() + LOCK_TIMEOUT;
        AutoGetDb autoDb(opCtx, mainNs.dbName(), MODE_IX, lockDeadline);
        AutoGetCollection bucketsColl(
            opCtx, bucketsNs, MODE_X, auto_get_collection::Options{}.deadline(lockDeadline));
        AutoGetCollection mainColl(opCtx,
                                   mainNs,
                                   MODE_X,
                                   auto_get_collection::Options{}
                                       .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                       .deadline(lockDeadline));

        // Idempotency check
        if (bucketsColl && bucketsColl->isTimeseriesCollection() &&
            !bucketsColl->isNewTimeseriesWithoutView() && !mainColl && mainColl.getView() &&
            mainColl.getView()->timeseries()) {
            // TODO(SERVER-114517): Investigate if we should relax this check.
            tassert(11483012,
                    "Found an already downgraded timeseries collection but with an unexpected UUID",
                    !expectedUUID || bucketsColl->uuid() == *expectedUUID);
            return;
        }

        // Sanity checks
        tassert(11483013, "Did not find the viewless timeseries collection to downgrade", mainColl);
        // TODO(SERVER-114517): Investigate if we should relax this check.
        tassert(11483014,
                "The viewless collection to downgrade does not have the expected UUID",
                !expectedUUID || mainColl->uuid() == *expectedUUID);
        tassert(11483015,
                "The viewless collection to downgrade does not have timeseries options",
                mainColl->isTimeseriesCollection() && mainColl->isNewTimeseriesWithoutView());
        tassert(11483016,
                "While downgrading viewless timeseries, we found a conflicting buckets collection",
                !bucketsColl);

        // Operations all lock system.views in the end to prevent deadlock.
        Lock::CollectionLock systemViewsLock(
            opCtx, autoDb.getDb()->getSystemViewsName(), MODE_X, lockDeadline);
        // Create system.views if it does not exist. This is done in a separate WUOW.
        autoDb.getDb()->createSystemDotViewsIfNecessary(opCtx);

        WriteUnitOfWork wuow(opCtx);

        // Run the timeseries downgrade steps without generating oplog entries.
        {
            repl::UnreplicatedWritesBlock uwb(opCtx);

            // Rename the collection to the buckets NSS and create the view on the main NSS.
            uassertStatusOK(
                autoDb.getDb()->renameCollection(opCtx, mainNs, bucketsNs, true /* stayTemp */));

            CollectionOptions viewOptions;
            viewOptions.viewOn = std::string{bucketsNs.coll()};
            viewOptions.collation = mainColl->getCollectionOptions().collation;
            constexpr bool asArray = true;
            viewOptions.pipeline =
                timeseries::generateViewPipeline(*mainColl->getTimeseriesOptions(), asArray);
            uassertStatusOK(
                autoDb.getDb()->userCreateNS(opCtx, mainNs, viewOptions, /*createIdIndex=*/false));

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
        // TODO(SERVER-114505): Call the OpObserver to emit an oplog entry

        wuow.commit();
    });

    LOGV2(11483017, "Finished downgrade of timeseries collection format", logAttrs(mainNs));
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
