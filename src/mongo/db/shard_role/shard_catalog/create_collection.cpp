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

#include "mongo/db/shard_role/shard_catalog/create_collection.h"

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
#include "mongo/db/index_key_validate.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_pre_and_post_images_options_gen.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/write_ops/insert.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/shard_role/shard_catalog/unique_collection_name.h"
#include "mongo/db/shard_role/shard_catalog/virtual_collection_options.h"
#include "mongo/db/shard_role/transaction_resources.h"
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
MONGO_FAIL_POINT_DEFINE(hangCreateCollectionBeforeLockAcquisition);

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

Status _checkNamespaceOrTimeseriesBucketsAlreadyExists(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       bool isForApplyOps) {
    auto statusNss = catalog::checkIfNamespaceExists(opCtx, nss);

    // Skip the cross-namespace check for applyOps paths (secondaries, initial sync, recovery,
    // and user-initiated applyOps).
    if (!statusNss.isOK() || isForApplyOps || !opCtx->isEnforcingConstraints()) {
        return statusNss;
    }
    auto otherTimeseriesNss = nss.isTimeseriesBucketsCollection()
        ? nss.getTimeseriesViewNamespace()
        : nss.makeTimeseriesBucketsNamespace();
    auto statusOtherNss = catalog::checkIfNamespaceExists(opCtx, otherTimeseriesNss);
    if (!statusOtherNss.isOK()) {
        return statusOtherNss.addContext("Conflicting namespace already exists");
    }

    return Status::OK();
}

Status _performCollectionCreationChecks(OperationContext* opCtx,
                                        const NamespaceString& ns,
                                        const CollectionOptions& options) {
    auto status = userAllowedCreateNS(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    const auto createViewlessTimeseriesColl =
        gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx));

    uassert(ErrorCodes::InvalidOptions,
            "the 'validator' option cannot be set when creating viewless time-series collection",
            !createViewlessTimeseriesColl || !options.timeseries.has_value() ||
                options.validator.isEmpty());

    // TODO SERVER-109289: Investigate whether this is safe on viewless time-series collections.
    uassert(
        ErrorCodes::OperationNotSupportedInTransaction,
        str::stream() << "Cannot create a time-series collection in a multi-document transaction.",
        !options.timeseries || !opCtx->inMultiDocumentTransaction());

    // system.profile must be a simple collection since new document insertions directly work
    // against the usual collection API. See introspect.cpp for more details.
    uassert(ErrorCodes::IllegalOperation,
            "Cannot create system.profile as a timeseries collection",
            !options.timeseries || !ns.isSystemDotProfile());

    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot create system collection " << ns.toStringForErrorMsg()
                          << " within a transaction.",
            !opCtx->inMultiDocumentTransaction() || !ns.isSystem());

    uassert(ErrorCodes::BadValue,
            "The 'timeField' or 'metaField' cannot start with '$'",
            !options.timeseries.has_value() ||
                (!options.timeseries->getTimeField().starts_with('$') &&
                 !options.timeseries->getMetaField().value_or("").starts_with('$')));

    return Status::OK();
}

/**
 * Internal function to create a view.
 *
 * Not idempotent, it will throw NamepsaceExists error if the same namespace already exists.
 * Does not retry in case of WriteConflict error.
 *
 * The @viewAcq parameter is not used in practice but it is here to force callers to acquire the
 * corresponding view before creation attempt.
 */
Status _createViewNoRetry(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const CollectionOrViewAcquisition& viewAcq,
                          const CollectionOptions& collectionOptions) {
    // system.profile will have new document inserts due to profiling. Inserts aren't supported
    // on views.
    uassert(ErrorCodes::IllegalOperation,
            "Cannot create system.profile as a view",
            !nss.isSystemDotProfile());
    uassert(ErrorCodes::OperationNotSupportedInTransaction,
            str::stream() << "Cannot create a view in a multi-document "
                             "transaction.",
            !opCtx->inMultiDocumentTransaction());
    uassert(ErrorCodes::Error(6026500),
            "The 'clusteredIndex' option is not supported with views",
            !collectionOptions.clusteredIndex);

    // This must be checked before we acquire X lock on the '<db>.system.views' namespace in order
    // to avoid taking multiple locks on this namespace.
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Cannot create a view called '" << nss.coll()
                          << "': this is a reserved system namespace",
            !nss.isSystemDotViews());

    // Operations all lock system.views in the end to prevent deadlock.
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString::makeSystemDotViewsNamespace(nss.dbName()), MODE_X);

    // The caller provided a CollectionOrViewAcquisition, which means a catalog instance is stashed
    // in the snapshot/RecoveryUnit associated to the OperationContext.
    //
    // Abandon that snapshot so that all subsequent catalog accesses use one taken after we lock
    // system.views in MODE_X. This guarantees the new snapshot reflects all modifications to
    // system.views made so far.
    //
    // See SERVER-117478 for additional details.
    shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

    auto db = DatabaseHolder::get(opCtx)->openDb(opCtx, nss.dbName());

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not primary while creating collection " << nss.toStringForErrorMsg());
    }

    // This is a top-level handler for collection creation name conflicts. New commands coming
    // in, or commands that generated a WriteConflict must return a NamespaceExists error here
    // on conflict.
    Status statusNss = catalog::checkIfNamespaceExists(opCtx, nss);
    if (!statusNss.isOK()) {
        return statusNss;
    }

    if (collectionOptions.changeStreamPreAndPostImagesOptions.getEnabled()) {
        return Status(ErrorCodes::InvalidOptions,
                      "option not supported on a view: changeStreamPreAndPostImages");
    }

    db->createSystemDotViewsIfNecessary(opCtx);

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

    Status status = db->userCreateNS(opCtx, nss, collectionOptions, /*createIdIndex=*/false);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    return Status::OK();
}

BSONObj pipelineAsBsonObj(const std::vector<BSONObj>& pipeline) {
    BSONArrayBuilder builder;
    for (const auto& stage : pipeline) {
        builder.append(stage);
    }
    return builder.obj();
}

/**
 * Given a create timeseries collection options generates and returns the corresponding options for
 * the creation of the corresponding timeseries view.
 */
CollectionOptions _generateLegacyTimeseriesViewOptions(const NamespaceString& bucketsNss,
                                                       const CollectionOptions& requestedOptions) {
    tassert(12128201,
            "Attempt to generate legacy timeseries view options from a non-timeseries collection "
            "options",
            requestedOptions.timeseries.has_value());
    tassert(12128202,
            "Attempt to generate legacy timeseries view options with a target namespace that is "
            "not system.buckets timeseries",
            bucketsNss.isTimeseriesBucketsCollection());
    CollectionOptions timeseriesViewOptions;
    timeseriesViewOptions.viewOn = std::string{bucketsNss.coll()};
    timeseriesViewOptions.collation = requestedOptions.collation;

    constexpr bool asArray = true;
    auto timeseriesOptions = requestedOptions.timeseries.get();
    uassertStatusOK(timeseries::validateAndSetBucketingParameters(timeseriesOptions));
    timeseriesViewOptions.pipeline = timeseries::generateViewPipeline(timeseriesOptions, asArray);

    return timeseriesViewOptions;
}

/**
 * Check if an existing collection `collPtr` is compatible with the given collection options.
 *
 * Throws NamespaceExists error if some of the requested options are not compatible with the already
 * existing collection.
 */
void checkExistingCollectionIsCompatible(OperationContext* opCtx,
                                         const CollectionPtr& collPtr,
                                         const CollectionOptions& requestedOptions) {
    tassert(9086200, "Cannot check options for a non-existing collection", collPtr);

    const auto& nss = collPtr->ns();

    uassert(ErrorCodes::NamespaceExists,
            fmt::format("namespace '{}' already exists, but is a collection rather than a view",
                        nss.toStringForErrorMsg()),
            !requestedOptions.isView());

    auto normalizedRequestedOptions = requestedOptions;
    auto existingOptions = collPtr->getCollectionOptions();
    if (requestedOptions.timeseries) {
        uassert(ErrorCodes::NamespaceExists,
                fmt::format("namespace '{}' already exists, but is not a timeseries collection",
                            nss.toStringForErrorMsg()),
                existingOptions.timeseries);

        // When checking that the options for the timeseries collection are the same,
        // filter out the options that were internally generated upon time-series
        // collection creation (i.e. were not specified by the user).
        uassertStatusOK(timeseries::validateAndSetBucketingParameters(
            normalizedRequestedOptions.timeseries.get()));
        existingOptions = uassertStatusOK(CollectionOptions::parse(existingOptions.toBSON(
            false /* includeUUID */, timeseries::kAllowedCollectionCreationOptions)));
    }

    const auto& collatorFactory = CollatorFactoryInterface::get(opCtx->getServiceContext());
    uassert(
        ErrorCodes::NamespaceExists,
        fmt::format("namespace '{}' already exists, but with different options: {}, requested: {}",
                    nss.toStringForErrorMsg(),
                    existingOptions.toBSON().toString(),
                    normalizedRequestedOptions.toBSON().toString()),
        normalizedRequestedOptions.matchesStorageOptions(existingOptions, collatorFactory));
}

/**
 * Check if an existing view is compatible with the given collection options.
 *
 * Throws NamespaceExists error if some of the requested options are not compatible with the already
 * existing view.
 */
void checkExistingViewIsCompatible(OperationContext* opCtx,
                                   const ViewDefinition& view,
                                   const CollectionOptions& requestedOptions) {
    tassert(11896700,
            fmt::format("Found existing view under prohibited timeseries buckets namespace '{}'",
                        view.name().toStringForErrorMsg()),
            !view.name().isTimeseriesBucketsCollection());

    uassert(ErrorCodes::NamespaceExists,
            fmt::format("namespace '{}' already exists, but is a view",
                        view.name().toStringForErrorMsg()),
            requestedOptions.isView());

    auto requestedTargetNss =
        NamespaceStringUtil::deserialize(view.name().dbName(), requestedOptions.viewOn);
    uassert(ErrorCodes::NamespaceExists,
            fmt::format("namespace '{}' already exists, but is a view on '{}' rather than '{}'",
                        view.name().toStringForErrorMsg(),
                        view.viewOn().toStringForErrorMsg(),
                        requestedTargetNss.toStringForErrorMsg()),
            view.viewOn() == requestedTargetNss);

    // Skip view pipeline equality check for legacy timeseries views.
    // The format of the auto-generated pipeline used for timeseries views have evolved during
    // past versions. For instance in SERVER-55315 (v6.0) we removed the 'exclude' field. Thus we
    // can't easily compare them.
    //
    // TODO SERVER-118970 always perform pipeline check once 9.0 becomes last LTS and all timeseries
    // collection are viewless.
    if (!requestedOptions.timeseries) {
        auto existingPipeline = pipelineAsBsonObj(view.pipeline());
        uassert(ErrorCodes::NamespaceExists,
                fmt::format("namespace '{}' already exists, but with pipeline {} rather than {}",
                            view.name().toStringForErrorMsg(),
                            existingPipeline.toString(),
                            requestedOptions.pipeline.toString()),
                existingPipeline.woCompare(requestedOptions.pipeline) == 0);
    }
    const auto& collatorFactory = CollatorFactoryInterface::get(opCtx->getServiceContext());
    // Note: the server can add more values to collation options which were not
    // specified in the original user request. Use the collator to check for
    // equivalence.
    auto newCollator = requestedOptions.collation.isEmpty()
        ? nullptr
        : uassertStatusOK(collatorFactory->makeFromBSON(requestedOptions.collation));

    if (!CollatorInterface::collatorsMatch(view.defaultCollator(), newCollator.get())) {
        const auto defaultCollatorSpecBSON =
            view.defaultCollator() ? view.defaultCollator()->getSpec().toBSON() : BSONObj();
        uasserted(
            ErrorCodes::NamespaceExists,
            fmt::format("namespace '{}' already exists, but with collation: {} rather than {}",
                        view.name().toStringForErrorMsg(),
                        defaultCollatorSpecBSON.toString(),
                        requestedOptions.collation.toString()));
    }
}

/**
 * Check if we already have a collection or view compatible with the given create command.
 *
 * Returns:
 *  - false: if no conflicting collection or view exists
 *  - true: if the namespace already exists and has same options
 *  - throws NamespaceExists error if a collection or view already exists with different options
 */
bool checkNamespaceAlreadyExistsAndCompatible(OperationContext* opCtx,
                                              const CollectionOrViewAcquisition& collAcq,
                                              const CollectionOptions& requestedOptions) {
    if (collAcq.collectionExists()) {
        checkExistingCollectionIsCompatible(opCtx, collAcq.getCollectionPtr(), requestedOptions);
        return true;
    } else if (collAcq.isView()) {
        // TODO SERVER-118970 remove this once 9.0 becomes last LTS.
        // By then we will have only viewless timeseries and legacy timeseries view will be illegal
        const auto& requestedViewOptions = [&] {
            if (requestedOptions.timeseries) {
                return _generateLegacyTimeseriesViewOptions(
                    collAcq.nss().makeTimeseriesBucketsNamespace(), requestedOptions);
            }
            return requestedOptions;
        }();
        checkExistingViewIsCompatible(
            opCtx, collAcq.getView().getViewDefinition(), requestedViewOptions);
        return true;
    }

    return false;
}

Status _createView(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
                   bool isForApplyOps = false) {
    return writeConflictRetry(opCtx, "create view", nss, [&] {
        auto viewAcq = acquireCollectionOrView(opCtx,
                                               CollectionOrViewAcquisitionRequest::fromOpCtx(
                                                   opCtx, nss, AcquisitionPrerequisites::kWrite),
                                               MODE_IX);
        auto status = _checkNamespaceOrTimeseriesBucketsAlreadyExists(opCtx, nss, isForApplyOps);
        if (!status.isOK()) {
            return status;
        }
        return _createViewNoRetry(opCtx, nss, viewAcq, collectionOptions);
    });
}

Status _setBucketingParametersAndAddClusteredIndex(CollectionOptions& options) {
    Status timeseriesOptionsValidateAndSetStatus =
        timeseries::validateAndSetBucketingParameters(options.timeseries.get());
    if (!timeseriesOptionsValidateAndSetStatus.isOK()) {
        return timeseriesOptionsValidateAndSetStatus;
    }

    // TODO(SERVER-101611): Initialize timeseriesBucketingParametersHaveChanged to false
    // when TSBucketingParametersUnchanged is enabled. Currently, this is not done because the
    // flag is stored in the WiredTiger config string (SERVER-91195), so doing it changes the
    // output of listCollections and breaks creation idempotency with no straightforward fixes

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
 * Creates timeseries buckets collection.
 *
 * This function is used both for the creation of viewless timeseries collection and legacy
 * system.buckets collections.
 */
void _createTimeseriesBucketsCollection(
    OperationContext* opCtx,
    const NamespaceString& targetNs,
    CollectionOptions collOptions,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) {
    WriteUnitOfWork wuow(opCtx);
    auto db = DatabaseHolder::get(opCtx)->openDb(opCtx, targetNs.dbName());
    AutoStatsTracker bucketsStatsTracker(opCtx,
                                         targetNs,
                                         Top::LockType::NotLocked,
                                         AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                         DatabaseProfileSettings::get(opCtx->getServiceContext())
                                             .getDatabaseProfileLevel(targetNs.dbName()));

    // If the buckets collection and time-series view creation roll back, ensure that their
    // Top entries are deleted.
    shard_role_details::getRecoveryUnit(opCtx)->onRollback([targetNs](OperationContext* opCtx) {
        Top::getDecoration(opCtx).collectionDropped(targetNs);
    });
    // Create the buckets collection that will back the view.
    const bool createIdIndex = false;
    uassertStatusOK(db->userCreateNS(opCtx,
                                     targetNs,
                                     collOptions,
                                     createIdIndex,
                                     /*idIndex=*/BSONObj(),
                                     /*fromMigrate=*/false,
                                     catalogIdentifier));

    CollectionWriter collectionWriter(opCtx, targetNs);

    auto validatedCollator = collOptions.collation;
    if (!collOptions.collation.isEmpty()) {
        auto swCollator = db->validateCollator(opCtx, collOptions);

        // The userCreateNS already has a uassertStatusOK and validateCollator is called in it,
        // so we should have the case that the status of the swCollator is ok.
        invariant(swCollator.getStatus());
        validatedCollator = swCollator.getValue()->getSpec().toBSON();
    }

    // We create the index on time and meta, which is used for query-based reopening, here for
    // viewless time-series collections if we are creating the collection on a primary. This is
    // done within the same WUOW as the collection creation.
    //
    // It should be safe to create the index in a separate WUOW from the collection creation,
    // this is currently what is done on secondaries. There is a risk that the index creation
    // entry gets rolled back, leaving the collection without the index, but this is easily
    // remedied by manually creating the index again. If the collection creation and index
    // creation are moved to separate WUOWs, which could simplify the code a bit, we would need
    // to take the X lock instead of the IX lock to prevent writes to the collection from being
    // performed before we have built the default index on it. TODO SERVER-107681 consider
    // performing the two creations in separate WUOWs on primaries as well.
    uassertStatusOK(
        timeseries::createDefaultTimeseriesIndex(opCtx, collectionWriter, validatedCollator));
    wuow.commit();
}

/**
 * Creates a timeseries collection.
 *
 * Throws NamespaceExists if a collection or view already exists with the same namespace but
 * incompatible metadata.
 *
 * The newly created collection will be either in viewless (new) or viewful (legacy) format,
 * depending on the status of the CreateViewlessTimeseries feature flag.
 *
 * This function is idempotent: if a timeseries collection already exists, in either viewless (new)
 * or viewful (legacy) format, with compatible metadata, this function is a no-op.
 *
 * If it encounters a legacy system.buckets timeseries collection with compatible metadata but
 * without the corresponding timeseries view, it will create the missing view.
 *
 * This function should only be used on the primary node. Creation of timeseries collections during
 * oplog application is done through the _createCollection function.
 *
 * TODO SERVER-118970 merge this function with the non-timeseries creation path once 9.0 becomes
 *      last LTS.
 */
void _createTimeseriesCollection(
    OperationContext* opCtx,
    const NamespaceString& ns,
    const CollectionOptions& optionsArg,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier) {

    // This path should only be taken when a user creates a new time-series collection on the
    // primary. Secondaries replicate individual oplog entries.
    invariant(opCtx->writesAreReplicated());

    const auto viewlessTimeseriesEnabled =
        gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx));

    tassert(12128910,
            fmt::format("Received illegal create timeseries collection request on legacy system "
                        "buckets namespace '{}' when viewless timeseries feature flag is enabled",
                        ns.toStringForErrorMsg()),
            !viewlessTimeseriesEnabled || !ns.isTimeseriesBucketsCollection());

    const auto& [mainNs, bucketsNs] = [&]() -> std::tuple<NamespaceString, NamespaceString> {
        if (ns.isTimeseriesBucketsCollection()) {
            return {ns.getTimeseriesViewNamespace(), ns};
        } else {
            return {ns, ns.makeTimeseriesBucketsNamespace()};
        }
    }();

    const auto& namespaceForValidation = viewlessTimeseriesEnabled ? mainNs : bucketsNs;
    uassertStatusOK(userAllowedCreateNS(opCtx, namespaceForValidation));
    uassertStatusOK(validateCollectionOptions(opCtx, namespaceForValidation, optionsArg, idIndex));

    writeConflictRetry(opCtx, "createTimeseriesCollection", ns, [&] {
        CollectionOrViewAcquisitionRequests requests{
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, mainNs, AcquisitionPrerequisites::kWrite),
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, bucketsNs, AcquisitionPrerequisites::kWrite),
        };
        auto acquisitions = makeAcquisitionMap(acquireCollectionsOrViews(opCtx, requests, MODE_X));
        auto& mainAcq = acquisitions.at(mainNs);
        auto& bucketsAcq = acquisitions.at(bucketsNs);

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, bucketsNs)) {
            // Report the error with the user provided namespace
            uasserted(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not primary while creating collection " << ns.toStringForErrorMsg());
        }

        const auto mainNssExists =
            checkNamespaceAlreadyExistsAndCompatible(opCtx, mainAcq, optionsArg);
        if (mainNssExists && mainAcq.isCollection()) {
            // A viewless timeseries collection with compatible options already exists
            return;
        }

        const auto bucketsCollExists =
            checkNamespaceAlreadyExistsAndCompatible(opCtx, bucketsAcq, optionsArg);

        if (mainNssExists && mainAcq.isView() && !bucketsCollExists && viewlessTimeseriesEnabled) {
            // We are attempting to create a viewless timeseries collection, we found a legacy
            // timeseries view but the corresponding system.buckets collection does not exist.
            uasserted(ErrorCodes::NamespaceExists,
                      fmt::format("A legacy timeseries view already exists for namespace '{}', but "
                                  "the corresponding system.buckets collection is missing",
                                  mainNs.toStringForErrorMsg()));
        }

        if (!bucketsCollExists) {
            auto bucketsOptions = optionsArg;
            uassertStatusOK(_setBucketingParametersAndAddClusteredIndex(bucketsOptions));

            if (viewlessTimeseriesEnabled) {
                _createTimeseriesBucketsCollection(
                    opCtx, mainNs, bucketsOptions, catalogIdentifier);
                return;
            } else {
                // Set the validator option to a JSON schema enforcing constraints on bucket
                // documents. This validation is only structural to prevent accidental corruption by
                // users and cannot cover all constraints. Leave the validationLevel and
                // validationAction to their strict/error defaults.
                auto timeField = bucketsOptions.timeseries->getTimeField();
                bucketsOptions.validator = timeseries::generateTimeseriesValidator(
                    timeseries::kTimeseriesControlLatestVersion, timeField);

                // Create the buckets collection that will back the view.
                _createTimeseriesBucketsCollection(
                    opCtx, bucketsNs, bucketsOptions, catalogIdentifier);
            }
        }

        if (
            // If the request came directly on the bucket namespace
            // we do not need to create the view.
            ns.isTimeseriesBucketsCollection() ||
            // If the 'temp' flag is true, we are in the $out stage, and should return
            // without creating the view definition.
            optionsArg.temp ||
            // The timeseries view already exists and is compatible
            mainNssExists) {
            return;
        }

        CollectionOptions viewOptions = _generateLegacyTimeseriesViewOptions(bucketsNs, optionsArg);
        const auto viewCreationStatus = _createViewNoRetry(opCtx, mainNs, mainAcq, viewOptions);
        uassertStatusOKWithContext(
            viewCreationStatus,
            str::stream() << "Failed to create view on " << bucketsNs.toStringForErrorMsg()
                          << " for time-series collection " << ns.toStringForErrorMsg()
                          << " with options " << viewOptions.toBSON());
    });
}

Status _createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& collectionOptions,
    const bool isForApplyOps,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<VirtualCollectionOptions>& virtualCollectionOptions = boost::none,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier = boost::none,
    boost::optional<bool> recordIdsReplicated = boost::none) {

    return writeConflictRetry(opCtx, "create", nss, [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX /* database lock mode*/, boost::none);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);

        // This is a top-level handler for collection creation name conflicts. New commands
        // coming in, or commands that generated a WriteConflict must return a NamespaceExists
        // error here on conflict.
        auto status = _checkNamespaceOrTimeseriesBucketsAlreadyExists(opCtx, nss, isForApplyOps);
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
                                   catalogIdentifier,
                                   recordIdsReplicated);
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
                                      catalogIdentifier,
                                      recordIdsReplicated);
        }
        if (!status.isOK()) {
            return status;
        }

        // We create the index on time and meta, which is used for query-based reopening, here for
        // viewless time-series collections if we are creating the collection on a primary. This is
        // done within the same WUOW as the collection creation.
        //
        // It should be safe to create the index in a separate WUOW from the collection creation,
        // this is currently what is done on secondaries. There is a risk that the index creation
        // entry gets rolled back, leaving the collection without the index, but this is easily
        // remedied by manually creating the index again. If the collection creation and index
        // creation are moved to separate WUOWs, which could simplify the code a bit, we would need
        // to take the X lock instead of the IX lock to prevent writes to the collection from being
        // performed before we have built the default index on it. TODO SERVER-107681 consider
        // performing the two creations in separate WUOWs on primaries as well.
        if (collectionOptions.timeseries && !nss.isTimeseriesBucketsCollection() &&
            !isForApplyOps) {
            tassert(1111800,
                    "Expected to be creating time-series collection from primary",
                    opCtx->writesAreReplicated());
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

std::pair<CollectionOptions, boost::optional<BSONObj>> getCollectionOptionsFromCreateCmd(
    OperationContext* opCtx, const CreateCommand& cmd) {

    auto options = CollectionOptions::fromCreateCommand(opCtx, cmd);
    auto idIndex = std::exchange(options.idIndex, {});
    bool hasExplicitlyDisabledClustering = cmd.getClusteredIndex() &&
        holds_alternative<bool>(*cmd.getClusteredIndex()) && !get<bool>(*cmd.getClusteredIndex());
    if (!hasExplicitlyDisabledClustering) {
        options =
            translateOptionsIfClusterByDefault(cmd.getNamespace(), std::move(options), idIndex);
    }
    return {std::move(options), std::move(idIndex)};
}

}  // namespace

bool checkNamespaceAndTimeseriesBucketsAlreadyExistsAndCompatible(OperationContext* opCtx,
                                                                  const CreateCommand& cmd) {
    auto originalNss = cmd.getNamespace();
    auto mainNss = originalNss.isTimeseriesBucketsCollection()
        ? originalNss.getTimeseriesViewNamespace()
        : originalNss;
    auto bucketsNss = mainNss.makeTimeseriesBucketsNamespace();

    // TODO SERVER-118970 remove acquisition on the timeseries buckets namespace
    CollectionOrViewAcquisitionRequests requests{
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx, mainNss, AcquisitionPrerequisites::kRead),
        CollectionOrViewAcquisitionRequest::fromOpCtx(
            opCtx, bucketsNss, AcquisitionPrerequisites::kRead),
    };

    auto acquisitions = makeAcquisitionMap(acquireCollectionsOrViews(opCtx, requests, MODE_IS));
    auto& mainAcq = acquisitions.at(mainNss);
    auto& bucketsAcq = acquisitions.at(bucketsNss);

    auto [requestedOptions, _] = getCollectionOptionsFromCreateCmd(opCtx, cmd);

    // TODO SERVER-118970 update this logic once all timeseries collection are viewless
    // By then we should never receive a create operation over a system.buckets collection.
    if (originalNss.isTimeseriesBucketsCollection() &&
        checkNamespaceAlreadyExistsAndCompatible(opCtx, bucketsAcq, requestedOptions)) {
        // The create command is targeting directly a system.buckets collections.  The timeseries
        // buckets collection already exists and is compatible with the requested options.
        return true;
    }

    auto mainNssAlreadyExists =
        checkNamespaceAlreadyExistsAndCompatible(opCtx, mainAcq, requestedOptions);

    // TODO SERVER-118970 remove this if altogether once all timeseries collections are viewless
    if (requestedOptions.timeseries && mainNssAlreadyExists && mainAcq.isView()) {
        // we found a compatible timeseries view, thus we need to check for the corresponding
        // timeseries buckets namespace
        return checkNamespaceAlreadyExistsAndCompatible(opCtx, bucketsAcq, requestedOptions);
    }

    return mainNssAlreadyExists;
}

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
    auto [collOptions, idIndex] = getCollectionOptionsFromCreateCmd(opCtx, cmd);
    return createCollection(opCtx, cmd.getNamespace(), collOptions, idIndex);
}

Status createCollectionForApplyOps(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<UUID>& ui,
    const BSONObj& cmdObj,
    bool allowRenameOutOfTheWay,
    const boost::optional<BSONObj>& idIndex,
    const boost::optional<CreateCollCatalogIdentifier>& catalogIdentifier,
    boost::optional<bool> recordIdsReplicated) {
    // While applying a collection creation op serializes with FCV changes, acquire an OFCV so:
    // - We can check feature flags defined as `check_against_fcv: operation_fcv_only`.
    // - If we enter here through the applyOps command, the OFCV will be replicated in the oplog,
    //   ensuring that oplog entries for create always have an OFCV.
    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
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

    if (collectionOptions.timeseries) {
        auto bucketingStatus = _setBucketingParametersAndAddClusteredIndex(collectionOptions);
        if (!bucketingStatus.isOK()) {
            return bucketingStatus;
        }
    }

    // TODO SERVER-112876: Disallow creating views with 'c' oplog entries via the applyOps command.
    if (collectionOptions.isView()) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot create view with collection specific catalog "
                "identifiers",
                !catalogIdentifier.has_value());
        return _createView(opCtx, newCollectionName, collectionOptions, /*isForApplyOps=*/true);
    }

    return _createCollection(opCtx,
                             newCollectionName,
                             collectionOptions,
                             /*isForApplyOps=*/true,
                             idIndex,
                             /*virtualCollectionOptions=*/boost::none,
                             catalogIdentifier,
                             recordIdsReplicated);
}


/**
 * Main entry point for creating user collections.
 */
Status createCollection(OperationContext* opCtx,
                        const NamespaceString& ns,
                        const CollectionOptions& options,
                        const boost::optional<BSONObj>& idIndex) {
    // Acquire an OFCV to get stable FCV-gated feature flag checks even during concurrent setFCV.
    // We may not have an OFCV yet because e.g. system collection creations (in the config DB) call
    // here directly, without going through the user command.
    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);

    auto status = _performCollectionCreationChecks(opCtx, ns, options);
    if (!status.isOK()) {
        return status;
    }

    hangCreateCollectionBeforeLockAcquisition.pauseWhileSet();

    if (options.isView()) {
        return _createView(opCtx, ns, options);
    }
    if (options.timeseries && opCtx->writesAreReplicated()) {
        try {
            _createTimeseriesCollection(
                opCtx, ns, options, idIndex, /*catalogIdentifier=*/boost::none);
        } catch (const DBException&) {
            return exceptionToStatus().withContext("Failed to create timeseries collection");
        }
        return Status::OK();
    }
    return _createCollection(opCtx,
                             ns,
                             options,
                             /*isForApplyOps=*/false,
                             idIndex,
                             /*virtualCollectionOptions=*/boost::none);
}

Status createVirtualCollection(OperationContext* opCtx,
                               const NamespaceString& ns,
                               const VirtualCollectionOptions& vopts) {
    VersionContext::FixedOperationFCVRegion fixedOfcvRegion(opCtx);
    tassert(6968504,
            "Virtual collection is available when the compute mode is enabled",
            computeModeEnabled);
    CollectionOptions options;
    options.setNoIdIndex();
    return _createCollection(opCtx,
                             ns,
                             options,
                             /*isForApplyOps=*/false,
                             boost::none,
                             vopts,
                             /*catalogIdentifier=*/boost::none);
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
