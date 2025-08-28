/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/create_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/clustered_collection_util.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/index_key_validate.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_state.h"
#include "mongo/db/local_catalog/unique_collection_name.h"
#include "mongo/db/local_catalog/virtual_collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/timeseries/viewless_timeseries_collection_creation_helpers.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/printf.h>  // IWYU pragma: keep

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failTimeseriesViewCreation);
MONGO_FAIL_POINT_DEFINE(clusterAllCollectionsByDefault);
MONGO_FAIL_POINT_DEFINE(skipIdIndex);

using IndexVersion = IndexDescriptor::IndexVersion;

Status validateClusteredIndexSpec(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const ClusteredIndexSpec& spec,
                                  boost::optional<int64_t> expireAfterSeconds) {
    if (!spec.getUnique()) {
        return Status(ErrorCodes::Error(5979700),
                      "The clusteredIndex option requires unique: true to be specified");
    }

    bool clusterKeyOnId =
        SimpleBSONObjComparator::kInstance.evaluate(spec.getKey() == BSON("_id" << 1));

    if (!clusterKeyOnId && !gSupportArbitraryClusterKeyIndex) {
        return Status(ErrorCodes::InvalidIndexSpecificationOption,
                      "The clusteredIndex option is only supported for key: {_id: 1}");
    }

    if (nss.isReplicated() && !clusterKeyOnId) {
        return Status(ErrorCodes::Error(5979701),
                      "The clusteredIndex option is only supported for key: {_id: 1} on replicated "
                      "collections");
    }

    if (spec.getKey().nFields() > 1) {
        return Status(ErrorCodes::Error(6053700),
                      "The clusteredIndex option does not support a compound cluster key");
    }

    const auto arbitraryClusterKeyField = clustered_util::getClusterKeyFieldName(spec);
    if (arbitraryClusterKeyField.find(".", 0) != std::string::npos) {
        return Status(
            ErrorCodes::Error(6053701),
            "The clusteredIndex option does not support a cluster key with nested fields");
    }

    const bool isForwardClusterKey = SimpleBSONObjComparator::kInstance.evaluate(
        spec.getKey() == BSON(arbitraryClusterKeyField << 1));
    if (!isForwardClusterKey) {
        return Status(ErrorCodes::Error(6053702),
                      str::stream()
                          << "The clusteredIndex option supports cluster keys like {"
                          << arbitraryClusterKeyField << ": 1}, but got " << spec.getKey());
    }

    if (expireAfterSeconds) {
        // Not included in the indexSpec itself.
        auto status = index_key_validate::validateExpireAfterSeconds(
            *expireAfterSeconds,
            index_key_validate::ValidateExpireAfterSecondsMode::kClusteredTTLIndex);
        if (!status.isOK()) {
            return status;
        }
    }

    auto versionAsInt = spec.getV();
    const IndexVersion indexVersion = static_cast<IndexVersion>(versionAsInt);
    if (indexVersion != IndexVersion::kV2) {
        return {ErrorCodes::Error(5979704),
                str::stream() << "Invalid clusteredIndex specification " << spec.toBSON()
                              << "; cannot create a clusteredIndex with v=" << versionAsInt};
    }

    return Status::OK();
}

Status validateCollectionOptions(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const CollectionOptions& collectionOptions,
                                 const boost::optional<BSONObj>& idIndex) {
    if (collectionOptions.expireAfterSeconds && !collectionOptions.clusteredIndex &&
        !collectionOptions.timeseries) {
        return Status(ErrorCodes::InvalidOptions,
                      "'expireAfterSeconds' can be used only for clustered collections or "
                      "timeseries collections");
    }

    if (auto clusteredIndex = collectionOptions.clusteredIndex) {
        if (clustered_util::requiresLegacyFormat(nss, collectionOptions) !=
            clusteredIndex->getLegacyFormat()) {
            return Status(ErrorCodes::Error(5979703),
                          "The 'clusteredIndex' legacy format {clusteredIndex: <bool>} is only "
                          "supported for specific internal collections and vice versa");
        }

        if (idIndex && !idIndex->isEmpty()) {
            return Status(ErrorCodes::InvalidOptions,
                          "The 'clusteredIndex' option is not supported with the 'idIndex' option");
        }
        if (collectionOptions.autoIndexId == CollectionOptions::NO) {
            return Status(ErrorCodes::Error(6026501),
                          "The 'clusteredIndex' option does not support {autoIndexId: false}");
        }

        if (collectionOptions.recordIdsReplicated) {
            return Status(ErrorCodes::InvalidOptions,
                          "The 'clusteredIndex' option is not supported with the "
                          "'recordIdsReplicated' option");
        }

        auto clusteredIndexStatus = validateClusteredIndexSpec(
            opCtx, nss, clusteredIndex->getIndexSpec(), collectionOptions.expireAfterSeconds);
        if (!clusteredIndexStatus.isOK()) {
            return clusteredIndexStatus;
        }
    }
    return Status::OK();
}

std::tuple<Lock::CollectionLock, Lock::CollectionLock> acquireCollLocksForRename(
    OperationContext* opCtx, const NamespaceString& ns1, const NamespaceString& ns2) {
    if (ResourceId{RESOURCE_COLLECTION, ns1} < ResourceId{RESOURCE_COLLECTION, ns2}) {
        Lock::CollectionLock collLock1{opCtx, ns1, MODE_X};
        Lock::CollectionLock collLock2{opCtx, ns2, MODE_X};
        return {std::move(collLock1), std::move(collLock2)};
    } else {
        Lock::CollectionLock collLock2{opCtx, ns2, MODE_X};
        Lock::CollectionLock collLock1{opCtx, ns1, MODE_X};
        return {std::move(collLock1), std::move(collLock2)};
    }
}

// If 'newCollName' is already occupied by another collection with a different UUID, renames the
// collection with 'newCollName' to a temporary name.
Status renameOutOfTheWayForApplyOps(OperationContext* opCtx,
                                    Database* db,
                                    const NamespaceString& newCollName,
                                    const UUID& uuid,
                                    bool allowRenameOutOfTheWay) {
    // In the case of oplog replay, a future command may have created or renamed a
    // collection with that same name. In that case, renaming this future collection to
    // a random temporary name is correct: once all entries are replayed no temporary
    // names will remain.
    auto futureColl = db
        ? CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, newCollName)
        : nullptr;
    if (!futureColl) {
        return Status::OK();
    }

    invariant(allowRenameOutOfTheWay,
              str::stream() << "Name already exists. Collection name: "
                            << newCollName.toStringForErrorMsg() << ", UUID: " << uuid
                            << ", Future collection UUID: " << futureColl->uuid());

    auto serviceContext = opCtx->getServiceContext();
    auto opObserver = serviceContext->getOpObserver();
    std::string tmpNssPattern("tmp%%%%%.create");
    if (newCollName.isTimeseriesBucketsCollection()) {
        tmpNssPattern =
            std::string{NamespaceString::kTimeseriesBucketsCollectionPrefix} + tmpNssPattern;
    }

    for (int tries = 0; tries < 10; ++tries) {
        auto tmpNameResult = makeUniqueCollectionName(opCtx, newCollName.dbName(), tmpNssPattern);
        if (!tmpNameResult.isOK()) {
            return tmpNameResult.getStatus().withContext(str::stream()
                                                         << "Cannot generate temporary "
                                                            "collection namespace for applyOps "
                                                            "create command: collection: "
                                                         << newCollName.toStringForErrorMsg());
        }

        const auto& tmpName = tmpNameResult.getValue();
        auto [tmpCollLock, newCollLock] = acquireCollLocksForRename(opCtx, tmpName, newCollName);
        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, tmpName)) {
            // Conflicting on generating a unique temp collection name. Try again.
            continue;
        }
        auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
            opCtx, newCollName, uuid);
        // It is ok to log this because this doesn't happen very frequently.
        LOGV2(20309,
              "CMD: create -- renaming existing collection with conflicting UUID to "
              "temporary collection",
              "newCollection"_attr = newCollName,
              "conflictingUUID"_attr = uuid,
              "tempName"_attr = tmpName);

        Status status = writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName, [&] {
            WriteUnitOfWork wuow(opCtx);
            Status status = db->renameCollection(opCtx, newCollName, tmpName, true /*stayTemp*/);
            if (!status.isOK())
                return status;
            auto futureCollUuid = futureColl->uuid();
            opObserver->onRenameCollection(opCtx,
                                           newCollName,
                                           tmpName,
                                           futureCollUuid,
                                           {}, /*dropTargetUUID*/
                                           0U, /*numRecords*/
                                           true /*stayTemp*/,
                                           false /*markFromMigrate*/,
                                           preConditions.isViewlessTimeseriesCollection());
            wuow.commit();
            // Re-fetch collection after commit to get a valid pointer
            futureColl =
                CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, futureCollUuid);
            return Status::OK();
        });

        if (!status.isOK())
            return status;

        // Abort any remaining index builds on the temporary collection.
        IndexBuildsCoordinator::get(opCtx)->abortCollectionIndexBuilds(
            opCtx, tmpName, futureColl->uuid(), "Aborting index builds on temporary collection");

        // The existing collection has been successfully moved out of the way.
        return Status::OK();
    }

    // Renaming was unsuccessful.
    return Status(ErrorCodes::NamespaceExists,
                  str::stream() << "Cannot generate temporary "
                                   "collection namespace for applyOps "
                                   "create command: collection: "
                                << newCollName.toStringForErrorMsg());
}

// Given the 'currentName' of a collection existing with 'uuid', renames the collection to
// 'newCollName'.
Status renameIntoRequestedNSSForApplyOps(OperationContext* opCtx,
                                         Database* db,
                                         const NamespaceString& currentName,
                                         const NamespaceString& newCollName,
                                         const UUID& uuid) {
    uassert(40655,
            str::stream() << "Invalid name " << newCollName.toStringForErrorMsg() << " for UUID "
                          << uuid,
            currentName.isEqualDb(newCollName));

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    return writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName, [&] {
        auto [currentCollLock, newCollLock] =
            acquireCollLocksForRename(opCtx, currentName, newCollName);
        WriteUnitOfWork wuow(opCtx);
        Status status = db->renameCollection(opCtx, currentName, newCollName, true /*stayTemp*/);
        if (!status.isOK())
            return status;
        auto preConditions = timeseries::CollectionPreConditions::getCollectionPreConditions(
            opCtx, currentName, uuid);
        opObserver->onRenameCollection(opCtx,
                                       currentName,
                                       newCollName,
                                       uuid,
                                       {},   /*dropTargetUUID*/
                                       0U,   /*numRecords*/
                                       true, /*stayTemp*/
                                       false /*markFromMigrate*/,
                                       preConditions.isViewlessTimeseriesCollection());
        wuow.commit();
        return Status::OK();
    });
}

void _createSystemDotViewsIfNecessary(OperationContext* opCtx, const Database* db) {
    // Create 'system.views' in a separate WUOW if it does not exist.
    if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                    db->getSystemViewsName())) {
        WriteUnitOfWork wuow(opCtx);
        invariant(db->createCollection(opCtx, db->getSystemViewsName()));
        wuow.commit();
    }
}

Status _createView(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions) {
    // This must be checked before we take locks in order to avoid attempting to take multiple locks
    // on the <db>.system.views namespace: first a IX lock on 'ns' and then a X lock on the database
    // system.views collection.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Cannot create a view called '" << nss.coll()
                          << "': this is a reserved system namespace",
            !nss.isSystemDotViews());

    return writeConflictRetry(opCtx, "create", nss, [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // Operations all lock system.views in the end to prevent deadlock.
        Lock::CollectionLock systemViewsLock(
            opCtx, NamespaceString::makeSystemDotViewsNamespace(nss.dbName()), MODE_X);

        auto db = autoDb.ensureDbExists(opCtx);

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection "
                                        << nss.toStringForErrorMsg());
        }

        // This is a top-level handler for collection creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        Status statusNss = catalog::checkIfNamespaceExists(opCtx, nss);
        if (!statusNss.isOK()) {
            return statusNss;
        }

        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
            ->checkShardVersionOrThrow(opCtx);

        if (collectionOptions.changeStreamPreAndPostImagesOptions.getEnabled()) {
            return Status(ErrorCodes::InvalidOptions,
                          "option not supported on a view: changeStreamPreAndPostImages");
        }

        _createSystemDotViewsIfNecessary(opCtx, db);

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(nss.dbName()));

        // If the view creation rolls back, ensure that the Top entry created for the view is
        // deleted.
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [nss](OperationContext* opCtx) { Top::getDecoration(opCtx).collectionDropped(nss); });

        if (MONGO_unlikely(failTimeseriesViewCreation.shouldFail([&nss](const BSONObj& data) {
                const auto fpNss = NamespaceStringUtil::parseFailPointData(data, "ns");
                return fpNss == nss;
            }))) {
            LOGV2(5490200,
                  "failTimeseriesViewCreation fail point enabled. Failing creation of view "
                  "definition.");
            return Status{ErrorCodes::OperationFailed,
                          str::stream() << "View definition " << nss.toStringForErrorMsg()
                                        << " creation failed due to 'failTimeseriesViewCreation' "
                                           "fail point enabled."};
        }

        // Even though 'collectionOptions' is passed by rvalue reference, it is not safe to move
        // because 'userCreateNS' may throw a WriteConflictException.
        Status status = db->userCreateNS(opCtx, nss, collectionOptions, /*createIdIndex=*/false);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
}

Status _createLegacyTimeseries(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const CollectionOptions& optionsArg,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) {

    // This path should only be taken when a user creates a new time-series collection on the
    // primary. Secondaries replicate individual oplog entries.
    invariant(opCtx->writesAreReplicated());

    const auto& bucketsNs =
        ns.isTimeseriesBucketsCollection() ? ns : ns.makeTimeseriesBucketsNamespace();

    Status bucketsAllowedStatus = userAllowedCreateNS(opCtx, bucketsNs);
    if (!bucketsAllowedStatus.isOK()) {
        return bucketsAllowedStatus;
    }

    Status validateStatus = validateCollectionOptions(opCtx, bucketsNs, optionsArg, idIndex);
    if (!validateStatus.isOK()) {
        return validateStatus;
    }

    CollectionOptions options = optionsArg;

    Status timeseriesOptionsValidateAndSetStatus =
        timeseries::validateAndSetBucketingParameters(options.timeseries.get());

    if (!timeseriesOptionsValidateAndSetStatus.isOK()) {
        return timeseriesOptionsValidateAndSetStatus;
    }

    bool existingBucketCollectionIsCompatible = false;

    Status ret = writeConflictRetry(opCtx, "createBucketCollection", bucketsNs, [&]() -> Status {
        AutoGetDb autoDb(opCtx, bucketsNs.dbName(), MODE_IX);
        Lock::CollectionLock bucketsCollLock(opCtx, bucketsNs, MODE_X);
        auto db = autoDb.ensureDbExists(opCtx);

        // Check if there already exists a Collection on the specified namespace. For legacy
        // time-series collections, this is the namespace of the view that we will create on top of
        // a buckets collection. For viewless time-series collection, this is just the name of the
        // collection.
        // For legacy time-series collections, we're not holding a Collection lock for this
        // namespace so we may only check if the pointer is null or not. The answer may also change
        // at any point after this call which is fine as we properly handle an orphaned bucket
        // collection. This check is just here to prevent it from being created in the common case.
        Status status = catalog::checkIfNamespaceExists(opCtx, ns);
        if (!status.isOK()) {
            return status;
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, bucketsNs)) {
            // Report the error with the user provided namespace
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection "
                                        << ns.toStringForErrorMsg());
        }

        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, bucketsNs)
            ->checkShardVersionOrThrow(opCtx);

        WriteUnitOfWork wuow(opCtx);
        AutoStatsTracker bucketsStatsTracker(
            opCtx,
            bucketsNs,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            DatabaseProfileSettings::get(opCtx->getServiceContext())
                .getDatabaseProfileLevel(ns.dbName()));

        // If the buckets collection and time-series view creation roll back, ensure that their
        // Top entries are deleted.
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [bucketsNs](OperationContext* opCtx) {
                Top::getDecoration(opCtx).collectionDropped(bucketsNs);
            });


        // Prepare collection option and index spec using the provided options. In case the
        // collection already exist we use these to validate that they are the same as being
        // requested here.
        CollectionOptions bucketsOptions = options;

        // Set the validator option to a JSON schema enforcing constraints on bucket documents.
        // This validation is only structural to prevent accidental corruption by users and
        // cannot cover all constraints. Leave the validationLevel and validationAction to their
        // strict/error defaults.
        auto timeField = options.timeseries->getTimeField();
        int bucketVersion = timeseries::kTimeseriesControlLatestVersion;
        bucketsOptions.validator =
            timeseries::generateTimeseriesValidator(bucketVersion, timeField);

        // TODO(SERVER-101611): Initialize timeseriesBucketingParametersHaveChanged to false
        // when TSBucketingParametersUnchanged is enabled. Currently, this is not done because the
        // flag is stored in the WiredTiger config string (SERVER-91195), so doing it changes the
        // output of listCollections and breaks creation idempotency with no straightforward fixes

        // Cluster time-series buckets collections by _id.
        auto expireAfterSeconds = options.expireAfterSeconds;
        if (expireAfterSeconds) {
            uassertStatusOK(index_key_validate::validateExpireAfterSeconds(
                *expireAfterSeconds,
                index_key_validate::ValidateExpireAfterSecondsMode::kClusteredTTLIndex));
            bucketsOptions.expireAfterSeconds = expireAfterSeconds;
        }

        bucketsOptions.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();

        if (auto coll =
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, bucketsNs)) {
            // Compare CollectionOptions and eventual TTL index to see if this bucket collection
            // may be reused for this request.
            existingBucketCollectionIsCompatible =
                coll->getCollectionOptions().matchesStorageOptions(
                    bucketsOptions, CollatorFactoryInterface::get(opCtx->getServiceContext()));

            // We may have a bucket collection created with a previous version of mongod, this
            // is also OK as we do not convert bucket collections to latest version during
            // upgrade.
            while (!existingBucketCollectionIsCompatible &&
                   bucketVersion > timeseries::kTimeseriesControlMinVersion) {
                bucketsOptions.validator =
                    timeseries::generateTimeseriesValidator(--bucketVersion, timeField);

                existingBucketCollectionIsCompatible =
                    coll->getCollectionOptions().matchesStorageOptions(
                        bucketsOptions, CollatorFactoryInterface::get(opCtx->getServiceContext()));
            }

            return Status(ErrorCodes::NamespaceExists,
                          str::stream()
                              << "Bucket Collection already exists. NS: "
                              << bucketsNs.toStringForErrorMsg() << ". UUID: " << coll->uuid());
        }

        // Create the buckets collection that will back the view.
        const bool createIdIndex = false;
        uassertStatusOK(db->userCreateNS(opCtx,
                                         bucketsNs,
                                         bucketsOptions,
                                         createIdIndex,
                                         /*idIndex=*/BSONObj(),
                                         /*fromMigrate=*/false,
                                         catalogIdentifier));

        CollectionWriter collectionWriter(opCtx, bucketsNs);

        auto validatedCollator = bucketsOptions.collation;
        if (!options.collation.isEmpty()) {
            auto swCollator = db->validateCollator(opCtx, bucketsOptions);

            // The userCreateNS already has a uassertStatusOK and validateCollator is called in it,
            // so we should have the case that the status of the swCollator is ok.
            invariant(swCollator.getStatus());
            validatedCollator = swCollator.getValue()->getSpec().toBSON();
        }

        uassertStatusOK(
            timeseries::createDefaultTimeseriesIndex(opCtx, collectionWriter, validatedCollator));
        wuow.commit();
        return Status::OK();
    });

    const auto& bucketCreationStatus = ret;
    if (
        // If we could not create the bucket collection and the pre-existing bucket collection is
        // not compatible we bubble up the error
        (!bucketCreationStatus.isOK() && !existingBucketCollectionIsCompatible) ||
        // If the 'temp' flag is true, we are in the $out stage, and should return without creating
        // the view defintion.
        options.temp ||
        // If the request came directly on the bucket namesapce we do not need to create the view.
        ns.isTimeseriesBucketsCollection()) {
        return bucketCreationStatus;
    }


    CollectionOptions viewOptions;
    viewOptions.viewOn = std::string{bucketsNs.coll()};
    viewOptions.collation = options.collation;
    constexpr bool asArray = true;
    viewOptions.pipeline = timeseries::generateViewPipeline(*options.timeseries, asArray);

    const auto viewCreationStatus = _createView(opCtx, ns, viewOptions);
    if (!viewCreationStatus.isOK()) {
        return viewCreationStatus.withContext(
            str::stream() << "Failed to create view on " << bucketsNs.toStringForErrorMsg()
                          << " for time-series collection " << ns.toStringForErrorMsg()
                          << " with options " << viewOptions.toBSON());
    }

    return Status::OK();
}

Status _createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<VirtualCollectionOptions>& virtualCollectionOptions = boost::none,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none) {
    return writeConflictRetry(opCtx, "create", nss, [&] {
        // If a change collection is to be created, that is, the change streams are being enabled
        // for a tenant, acquire exclusive tenant lock.
        AutoGetDb autoDb(opCtx,
                         nss.dbName(),
                         MODE_IX /* database lock mode*/,
                         boost::make_optional(nss.tenantId() && nss.isChangeCollection(), MODE_X));
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);

        // This is a top-level handler for collection creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        Status status = catalog::checkIfNamespaceExists(opCtx, nss);
        if (!status.isOK()) {
            return status;
        }

        status = validateCollectionOptions(opCtx, nss, collectionOptions, idIndex);
        if (!status.isOK()) {
            return status;
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection "
                                        << nss.toStringForErrorMsg());
        }

        CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, nss)
            ->checkShardVersionOrThrow(opCtx);

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      DatabaseProfileSettings::get(opCtx->getServiceContext())
                                          .getDatabaseProfileLevel(nss.dbName()));

        // If the collection creation rolls back, ensure that the Top entry created for the
        // collection is deleted.
        shard_role_details::getRecoveryUnit(opCtx)->onRollback(
            [nss](OperationContext* opCtx) { Top::getDecoration(opCtx).collectionDropped(nss); });

        // Even though 'collectionOptions' is passed by rvalue reference, it is not safe to move
        // because 'userCreateNS' may throw a WriteConflictException.
        if (idIndex == boost::none || collectionOptions.clusteredIndex) {
            status = virtualCollectionOptions
                ? db->userCreateVirtualNS(opCtx, nss, collectionOptions, *virtualCollectionOptions)
                : db->userCreateNS(opCtx,
                                   nss,
                                   collectionOptions,
                                   /*createIdIndex=*/false,
                                   /*idIndex=*/BSONObj(),
                                   /*fromMigrate=*/false,
                                   catalogIdentifier);
        } else {
            bool createIdIndex = true;
            if (MONGO_unlikely(skipIdIndex.shouldFail())) {
                createIdIndex = false;
            }
            status = db->userCreateNS(opCtx,
                                      nss,
                                      collectionOptions,
                                      createIdIndex,
                                      *idIndex,
                                      /*fromMigrate=*/false,
                                      catalogIdentifier);
        }
        if (!status.isOK()) {
            return status;
        }

        // We create the index on time and meta, which is used for query-based reopening, here for
        // viewless time-series collections if we are creating the collection on a primary. This is
        // done within the same WUOW as the collection creation.
        if (collectionOptions.timeseries && !nss.isTimeseriesBucketsCollection() &&
            opCtx->writesAreReplicated()) {
            CollectionWriter collWriter(opCtx, nss);
            invariant(collWriter->isNewTimeseriesWithoutView());

            auto validatedCollator = collectionOptions.collation;
            if (!collectionOptions.collation.isEmpty()) {
                auto tmpOptions = collectionOptions;
                auto swCollator = db->validateCollator(opCtx, tmpOptions);

                // The userCreateNS already has a uassertStatusOK and validateCollator is called in
                // it, so we should have the case that the status of the swCollator is ok.
                invariant(swCollator.getStatus());
                validatedCollator = swCollator.getValue()->getSpec().toBSON();
            }
            uassertStatusOK(
                timeseries::createDefaultTimeseriesIndex(opCtx, collWriter, validatedCollator));
        }

        wunit.commit();
        return Status::OK();
    });
}

Status _setBucketingParametersAndAddClusteredIndex(CollectionOptions& options) {
    Status timeseriesOptionsValidateAndSetStatus =
        timeseries::validateAndSetBucketingParameters(options.timeseries.get());
    if (!timeseriesOptionsValidateAndSetStatus.isOK()) {
        return timeseriesOptionsValidateAndSetStatus;
    }
    // Cluster time-series buckets collections by _id.
    if (options.expireAfterSeconds) {
        uassertStatusOK(index_key_validate::validateExpireAfterSeconds(
            *options.expireAfterSeconds,
            index_key_validate::ValidateExpireAfterSecondsMode::kClusteredTTLIndex));
    }
    options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    return Status::OK();
}

/**
 * Shared part of the implementation which parses CollectionOptions from a 'cmdObj' with
 * collection creation.
 */
StatusWith<CollectionOptions> parseCollectionOptionsFromCreateCmdObj(
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const boost::optional<BSONObj>& idIndex,
    CollectionOptions::ParseKind kind) {
    BSONObjIterator it(cmdObj);

    // Skip the first cmdObj element.
    BSONElement firstElt = it.next();
    invariant(firstElt.fieldNameStringData() == "create");

    // Build options object from remaining cmdObj elements.
    BSONObjBuilder optionsBuilder;
    while (it.more()) {
        const auto elem = it.next();
        if (!isGenericArgument(elem.fieldNameStringData()))
            optionsBuilder.append(elem);
        if (elem.fieldNameStringData() == "viewOn") {
            // Views don't have UUIDs so it should always be parsed for command.
            kind = CollectionOptions::parseForCommand;
        }
    }

    BSONObj options = optionsBuilder.obj();
    uassert(14832,
            "specify size:<n> when capped is true",
            !options["capped"].trueValue() || options["size"].isNumber());

    StatusWith<CollectionOptions> statusWith = CollectionOptions::parse(options, kind);
    if (!statusWith.isOK()) {
        return statusWith.getStatus();
    }

    CollectionOptions collectionOptions = statusWith.getValue();

    bool hasExplicitlyDisabledClustering =
        options["clusteredIndex"].isBoolean() && !options["clusteredIndex"].boolean();
    if (!hasExplicitlyDisabledClustering) {
        collectionOptions =
            translateOptionsIfClusterByDefault(nss, std::move(collectionOptions), idIndex);
    }
    return collectionOptions;
}

Status createCollectionHelper(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const CollectionOptions& options,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none) {
    auto status = userAllowedCreateNS(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    uassert(ErrorCodes::CommandNotSupported,
            "'recordIdsReplicated' option may not be run without featureFlagRecordIdsReplicated "
            "enabled",
            !options.recordIdsReplicated ||
                gFeatureFlagRecordIdsReplicated.isEnabled(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

    const auto createViewlessTimeseriesColl =
        gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

    uassert(ErrorCodes::InvalidOptions,
            "the 'validator' option cannot be set when creating viewless time-series collection",
            !createViewlessTimeseriesColl || !options.timeseries.has_value() ||
                options.validator.isEmpty());

    // TODO SERVER-109289: Investigate whether this is safe on viewless time-series collections.
    uassert(
        ErrorCodes::OperationNotSupportedInTransaction,
        str::stream() << "Cannot create a time-series collection in a multi-document transaction.",
        !options.timeseries || !opCtx->inMultiDocumentTransaction());

    if (options.isView()) {
        // system.profile will have new document inserts due to profiling. Inserts aren't supported
        // on views.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot create system.profile as a view",
                !ns.isSystemDotProfile());
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create a view in a multi-document "
                                 "transaction.",
                !opCtx->inMultiDocumentTransaction());
        uassert(ErrorCodes::Error(6026500),
                "The 'clusteredIndex' option is not supported with views",
                !options.clusteredIndex);
        uassert(ErrorCodes::InvalidOptions,
                "Cannot create view with collection specific catalog "
                "identifiers",
                !catalogIdentifier.has_value());
        return _createView(opCtx, ns, options);
    } else if (options.timeseries && opCtx->writesAreReplicated() &&
               !createViewlessTimeseriesColl) {
        // system.profile must be a simple collection since new document insertions directly work
        // against the usual collection API. See introspect.cpp for more details.
        uassert(ErrorCodes::IllegalOperation,
                "Cannot create system.profile as a timeseries collection",
                !ns.isSystemDotProfile());
        // This helper is designed for user-created time-series collections on primaries. If a
        // time-series buckets collection is created explicitly or during replication, treat this as
        // a normal collection creation.
        return _createLegacyTimeseries(opCtx, ns, options, idIndex, catalogIdentifier);
    } else {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create system collection " << ns.toStringForErrorMsg()
                              << " within a transaction.",
                !opCtx->inMultiDocumentTransaction() || !ns.isSystem());
        return _createCollection(opCtx,
                                 ns,
                                 options,
                                 idIndex,
                                 /*virtualCollectionOptions=*/boost::none,
                                 catalogIdentifier);
    }
}

}  // namespace

Status createCollection(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex) {
    const auto nss = CommandHelpers::parseNsCollectionRequired(dbName, cmdObj);
    StatusWith<CollectionOptions> swCollectionOptions = parseCollectionOptionsFromCreateCmdObj(
        nss, cmdObj, idIndex, CollectionOptions::parseForCommand);
    if (!swCollectionOptions.isOK()) {
        return swCollectionOptions.getStatus();
    }

    return createCollection(opCtx, nss, swCollectionOptions.getValue(), idIndex);
}

Status createCollection(OperationContext* opCtx, const CreateCommand& cmd) {
    auto options = CollectionOptions::fromCreateCommand(cmd);
    auto idIndex = std::exchange(options.idIndex, {});
    bool hasExplicitlyDisabledClustering = cmd.getClusteredIndex() &&
        holds_alternative<bool>(*cmd.getClusteredIndex()) && !get<bool>(*cmd.getClusteredIndex());
    if (!hasExplicitlyDisabledClustering) {
        options =
            translateOptionsIfClusterByDefault(cmd.getNamespace(), std::move(options), idIndex);
    }

    return createCollection(opCtx, cmd.getNamespace(), options, idIndex);
}

Status createCollectionForApplyOps(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<UUID>& ui,
    const BSONObj& cmdObj,
    bool allowRenameOutOfTheWay,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IX));

    const NamespaceString newCollectionName(
        CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto* const db = databaseHolder->getDb(opCtx, dbName);

    if (ui) {
        auto uuid = ui.value();
        uassert(ErrorCodes::InvalidUUID,
                "Invalid UUID in applyOps create command: " + uuid.toString(),
                uuid.isRFC4122v4());

        // Step 1: Return early if a collection already exists with 'uuid' and 'newCollectionName';
        const auto currentName = CollectionCatalog::get(opCtx)->lookupNSSByUUID(opCtx, uuid);
        if (currentName == newCollectionName) {
            // No work to be done, collection already exists.
            return Status::OK();
        }

        // Step 2: Move any future collection with the same name out of the way.
        Status s = renameOutOfTheWayForApplyOps(
            opCtx, db, newCollectionName, uuid, allowRenameOutOfTheWay);
        if (!s.isOK())
            return s;

        // Step 3: Rename collection with requested UUID if it already exists under a different
        // name.
        if (CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid)) {
            invariant(currentName);
            return renameIntoRequestedNSSForApplyOps(
                opCtx, db, *currentName, newCollectionName, uuid);
        }
    }

    StatusWith<CollectionOptions> swCollectionOptions = parseCollectionOptionsFromCreateCmdObj(
        newCollectionName, cmdObj, idIndex, CollectionOptions::parseForStorage);
    if (!swCollectionOptions.isOK()) {
        return swCollectionOptions.getStatus();
    }

    // Create oplog entries generated by a primary node specify a collection's uuid in the 'ui'
    // field. There's no guarantee operations from an 'applyOps' abide by this rule. Preserve legacy
    // behavior, where 'ui' overwrites an unexpected 'uuid' parsed from the 'cmdObj', if and only if
    // 'ui' is not none.
    //
    // TODO SERVER-106003: Determine and document contract if both 'ui' and 'o.uuid' fields are
    // present and conflict.
    auto& collectionOptions = swCollectionOptions.getValue();
    collectionOptions.uuid = ui ? ui : collectionOptions.uuid;

    return createCollectionHelper(
        opCtx, newCollectionName, collectionOptions, idIndex, catalogIdentifier);
}


Status createCollection(OperationContext* opCtx,
                        const NamespaceString& ns,
                        const CollectionOptions& optionsArg,
                        const boost::optional<BSONObj>& idIndex,
                        const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) {
    const auto createViewlessTimeseriesColl = optionsArg.timeseries &&
        gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
    auto options = optionsArg;
    if (createViewlessTimeseriesColl) {
        auto status = _setBucketingParametersAndAddClusteredIndex(options);
        if (!status.isOK()) {
            return status;
        }
    }

    return createCollectionHelper(opCtx, ns, options, idIndex, catalogIdentifier);
}


Status createVirtualCollection(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const VirtualCollectionOptions& vopts) {
    tassert(6968504,
            "Virtual collection is available when the compute mode is enabled",
            computeModeEnabled);
    CollectionOptions options;
    options.setNoIdIndex();
    return _createCollection(
        opCtx, ns, options, boost::none, vopts, /*catalogIdentifier=*/boost::none);
}

CollectionOptions translateOptionsIfClusterByDefault(const NamespaceString& nss,
                                                     CollectionOptions collectionOptions,
                                                     const boost::optional<BSONObj>& idIndex) {
    if (MONGO_unlikely(clusterAllCollectionsByDefault.shouldFail()) &&
        !collectionOptions.isView() && !collectionOptions.timeseries &&
        !collectionOptions.clusteredIndex.has_value() && (!idIndex || idIndex->isEmpty()) &&
        !collectionOptions.capped &&
        !clustered_util::requiresLegacyFormat(nss, collectionOptions)) {
        // Capped, clustered collections differ in behavior significantly from normal
        // capped collections. Notably, they allow out-of-order insertion.
        //
        // Additionally, don't set the collection to be clustered in the default format if it
        // requires legacy format.
        collectionOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    }
    return collectionOptions;
}

}  // namespace mongo
