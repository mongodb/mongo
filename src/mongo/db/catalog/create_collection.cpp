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

#include "mongo/db/catalog/create_collection.h"

#include <fmt/printf.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/unique_collection_name.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(failTimeseriesViewCreation);
MONGO_FAIL_POINT_DEFINE(clusterAllCollectionsByDefault);

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

    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // Operations all lock system.views in the end to prevent deadlock.
        Lock::CollectionLock systemViewsLock(
            opCtx,
            NamespaceString(nss.dbName(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        auto db = autoDb.ensureDbExists(opCtx);

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        CollectionShardingState::get(opCtx, nss)->checkShardVersionOrThrow(opCtx);

        if (collectionOptions.changeStreamPreAndPostImagesOptions.getEnabled()) {
            return Status(ErrorCodes::InvalidOptions,
                          "option not supported on a view: changeStreamPreAndPostImages");
        }

        // Cannot directly create a view on a system.buckets collection, only by creating a
        // time-series collection.
        auto viewOnNss = NamespaceString{collectionOptions.viewOn};
        uassert(ErrorCodes::InvalidNamespace,
                "Cannot create view on a system.buckets namespace except by creating a time-series "
                "collection",
                !viewOnNss.isTimeseriesBucketsCollection());

        _createSystemDotViewsIfNecessary(opCtx, db);

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(
            opCtx,
            nss,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));

        // If the view creation rolls back, ensure that the Top entry created for the view is
        // deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

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

Status _createDefaultTimeseriesIndex(OperationContext* opCtx, CollectionWriter& collection) {
    if (!feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return Status::OK();
    }

    auto tsOptions = collection->getCollectionOptions().timeseries;
    if (!tsOptions->getMetaField()) {
        return Status::OK();
    }

    StatusWith<BSONObj> swBucketsSpec = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
        *tsOptions, BSON(*tsOptions->getMetaField() << 1 << tsOptions->getTimeField() << 1));
    if (!swBucketsSpec.isOK()) {
        return swBucketsSpec.getStatus();
    }

    const std::string indexName = str::stream()
        << *tsOptions->getMetaField() << "_1_" << tsOptions->getTimeField() << "_1";
    IndexBuildsCoordinator::get(opCtx)->createIndexesOnEmptyCollection(
        opCtx,
        collection,
        {BSON("v" << 2 << "name" << indexName << "key" << swBucketsSpec.getValue())},
        /*fromMigrate=*/false);
    return Status::OK();
}

Status _createTimeseries(OperationContext* opCtx,
                         const NamespaceString& ns,
                         const CollectionOptions& optionsArg) {
    // This path should only be taken when a user creates a new time-series collection on the
    // primary. Secondaries replicate individual oplog entries.
    invariant(!ns.isTimeseriesBucketsCollection());
    invariant(opCtx->writesAreReplicated());

    auto bucketsNs = ns.makeTimeseriesBucketsNamespace();

    CollectionOptions options = optionsArg;

    // TODO (SERVER-67598) Modify this comment as it will be out of date.
    // Users may not pass a 'bucketMaxSpanSeconds' or 'bucketRoundingSeconds' other than the
    // default. Instead they should rely on the default behavior from the 'granularity'.
    auto granularity = options.timeseries->getGranularity();
    if (feature_flags::gTimeseriesScalabilityImprovements.isEnabled(
            serverGlobalParams.featureCompatibility) &&
        options.timeseries->getBucketRoundingSeconds()) {
        uassert(
            6759501,
            "Timeseries 'bucketMaxSpanSeconds' needs to be set alongside 'bucketRoundingSeconds'",
            options.timeseries->getBucketMaxSpanSeconds());

        auto roundingSeconds = timeseries::getBucketRoundingSecondsFromGranularity(granularity);
        // TODO (SERVER-67598): add checks for bucketRoundingSeconds (that it divides evenly and is
        // less than bucketMaxSpanSeconds)
        options.timeseries->setBucketRoundingSeconds(roundingSeconds);
    }

    auto maxSpanSeconds = timeseries::getMaxSpanSecondsFromGranularity(granularity);
    uassert(5510500,
            fmt::format("Timeseries 'bucketMaxSpanSeconds' is not configurable to a value other "
                        "than the default of {} for the provided granularity",
                        maxSpanSeconds),
            !options.timeseries->getBucketMaxSpanSeconds() ||
                maxSpanSeconds == options.timeseries->getBucketMaxSpanSeconds());
    options.timeseries->setBucketMaxSpanSeconds(maxSpanSeconds);

    // Set the validator option to a JSON schema enforcing constraints on bucket documents.
    // This validation is only structural to prevent accidental corruption by users and
    // cannot cover all constraints. Leave the validationLevel and validationAction to their
    // strict/error defaults.
    auto timeField = options.timeseries->getTimeField();
    auto validatorObj = fromjson(fmt::sprintf(R"(
{
'$jsonSchema' : {
    bsonType: 'object',
    required: ['_id', 'control', 'data'],
    properties: {
        _id: {bsonType: 'objectId'},
        control: {
            bsonType: 'object',
            required: ['version', 'min', 'max'],
            properties: {
                version: {bsonType: 'number'},
                min: {
                    bsonType: 'object',
                    required: ['%s'],
                    properties: {'%s': {bsonType: 'date'}}
                },
                max: {
                    bsonType: 'object',
                    required: ['%s'],
                    properties: {'%s': {bsonType: 'date'}}
                },
                closed: {bsonType: 'bool'},
                count: {bsonType: 'number', minimum: 1}
            },
            additionalProperties: false
        },
        data: {bsonType: 'object'},
        meta: {}
    },
    additionalProperties: false
}
})",
                                              timeField,
                                              timeField,
                                              timeField,
                                              timeField));

    bool existingBucketCollectionIsCompatible = false;

    Status ret =
        writeConflictRetry(opCtx, "createBucketCollection", bucketsNs.ns(), [&]() -> Status {
            AutoGetDb autoDb(opCtx, bucketsNs.dbName(), MODE_IX);
            Lock::CollectionLock bucketsCollLock(opCtx, bucketsNs, MODE_X);
            auto db = autoDb.ensureDbExists(opCtx);

            // Check if there already exist a Collection on the namespace we will later create a
            // view on. We're not holding a Collection lock for this Collection so we may only check
            // if the pointer is null or not. The answer may also change at any point after this
            // call which is fine as we properly handle an orphaned bucket collection. This check is
            // just here to prevent it from being created in the common case.
            Status status = catalog::checkIfNamespaceExists(opCtx, ns);
            if (!status.isOK()) {
                return status;
            }

            if (opCtx->writesAreReplicated() &&
                !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, bucketsNs)) {
                // Report the error with the user provided namespace
                return Status(ErrorCodes::NotWritablePrimary,
                              str::stream() << "Not primary while creating collection " << ns);
            }

            CollectionShardingState::get(opCtx, bucketsNs)->checkShardVersionOrThrow(opCtx);

            WriteUnitOfWork wuow(opCtx);
            AutoStatsTracker bucketsStatsTracker(
                opCtx,
                bucketsNs,
                Top::LockType::NotLocked,
                AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(ns.dbName()));

            // If the buckets collection and time-series view creation roll back, ensure that their
            // Top entries are deleted.
            opCtx->recoveryUnit()->onRollback(
                [serviceContext = opCtx->getServiceContext(), bucketsNs]() {
                    Top::get(serviceContext).collectionDropped(bucketsNs);
                });


            // Prepare collection option and index spec using the provided options. In case the
            // collection already exist we use these to validate that they are the same as being
            // requested here.
            CollectionOptions bucketsOptions = options;
            bucketsOptions.validator = validatorObj;

            // Cluster time-series buckets collections by _id.
            auto expireAfterSeconds = options.expireAfterSeconds;
            if (expireAfterSeconds) {
                uassertStatusOK(index_key_validate::validateExpireAfterSeconds(
                    *expireAfterSeconds,
                    index_key_validate::ValidateExpireAfterSecondsMode::kClusteredTTLIndex));
                bucketsOptions.expireAfterSeconds = expireAfterSeconds;
            }

            bucketsOptions.clusteredIndex =
                clustered_util::makeCanonicalClusteredInfoForLegacyFormat();

            if (auto coll =
                    CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, bucketsNs)) {
                // Compare CollectionOptions and eventual TTL index to see if this bucket collection
                // may be reused for this request.
                existingBucketCollectionIsCompatible =
                    coll->getCollectionOptions().matchesStorageOptions(
                        bucketsOptions, CollatorFactoryInterface::get(opCtx->getServiceContext()));
                return Status(ErrorCodes::NamespaceExists,
                              str::stream() << "Bucket Collection already exists. NS: " << bucketsNs
                                            << ". UUID: " << coll->uuid());
            }

            // Create the buckets collection that will back the view.
            const bool createIdIndex = false;
            uassertStatusOK(db->userCreateNS(opCtx, bucketsNs, bucketsOptions, createIdIndex));

            CollectionWriter collectionWriter(opCtx, bucketsNs);
            uassertStatusOK(_createDefaultTimeseriesIndex(opCtx, collectionWriter));
            wuow.commit();
            return Status::OK();
        });

    // If compatible bucket collection already exists then proceed with creating view definition.
    if (!ret.isOK() && !existingBucketCollectionIsCompatible)
        return ret;

    ret = writeConflictRetry(opCtx, "create", ns.ns(), [&]() -> Status {
        AutoGetCollection autoColl(opCtx, ns, MODE_IX, AutoGetCollectionViewMode::kViewsPermitted);
        Lock::CollectionLock systemDotViewsLock(
            opCtx,
            NamespaceString(ns.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);
        auto db = autoColl.ensureDbExists(opCtx);

        // This is a top-level handler for time-series creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        Status status = catalog::checkIfNamespaceExists(opCtx, ns);
        if (!status.isOK()) {
            return status;
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            return {ErrorCodes::NotWritablePrimary,
                    str::stream() << "Not primary while creating collection " << ns};
        }

        CollectionShardingState::get(opCtx, ns)->checkShardVersionOrThrow(opCtx);

        _createSystemDotViewsIfNecessary(opCtx, db);

        auto catalog = CollectionCatalog::get(opCtx);
        WriteUnitOfWork wuow(opCtx);

        AutoStatsTracker statsTracker(opCtx,
                                      ns,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      catalog->getDatabaseProfileLevel(ns.dbName()));

        // If the buckets collection and time-series view creation roll back, ensure that their
        // Top entries are deleted.
        opCtx->recoveryUnit()->onRollback([serviceContext = opCtx->getServiceContext(), ns]() {
            Top::get(serviceContext).collectionDropped(ns);
        });

        if (MONGO_unlikely(failTimeseriesViewCreation.shouldFail(
                [&ns](const BSONObj& data) { return data["ns"_sd].String() == ns.ns(); }))) {
            LOGV2(5490200,
                  "failTimeseriesViewCreation fail point enabled. Failing creation of view "
                  "definition after bucket collection was created successfully.");
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Timeseries view definition " << ns
                                  << " creation failed due to 'failTimeseriesViewCreation' "
                                     "fail point enabled."};
        }

        CollectionOptions viewOptions;
        viewOptions.viewOn = bucketsNs.coll().toString();
        viewOptions.collation = options.collation;
        constexpr bool asArray = true;
        viewOptions.pipeline = timeseries::generateViewPipeline(*options.timeseries, asArray);

        // Create the time-series view.
        status = db->userCreateNS(opCtx, ns, viewOptions);
        if (!status.isOK()) {
            return status.withContext(str::stream() << "Failed to create view on " << bucketsNs
                                                    << " for time-series collection " << ns
                                                    << " with options " << viewOptions.toBSON());
        }

        wuow.commit();
        return Status::OK();
    });

    return ret;
}

Status _createCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionOptions& collectionOptions,
                         const boost::optional<BSONObj>& idIndex) {
    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto db = autoDb.ensureDbExists(opCtx);

        // This is a top-level handler for collection creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        Status status = catalog::checkIfNamespaceExists(opCtx, nss);
        if (!status.isOK()) {
            return status;
        }

        if (!collectionOptions.clusteredIndex && collectionOptions.expireAfterSeconds) {
            return Status(ErrorCodes::InvalidOptions,
                          "'expireAfterSeconds' requires clustering to be enabled");
        }

        if (auto clusteredIndex = collectionOptions.clusteredIndex) {
            bool clusteredIndexesEnabled =
                feature_flags::gClusteredIndexes.isEnabled(serverGlobalParams.featureCompatibility);
            if (!clusteredIndexesEnabled && !clustered_util::requiresLegacyFormat(nss)) {
                // The 'clusteredIndex' option is only supported in legacy format for specific
                // internal collections when the gClusteredIndexes flag is disabled.
                return Status(ErrorCodes::InvalidOptions,
                              str::stream()
                                  << "The 'clusteredIndex' option is not supported for namespace "
                                  << nss);
            }

            if (clustered_util::requiresLegacyFormat(nss) != clusteredIndex->getLegacyFormat()) {
                return Status(ErrorCodes::Error(5979703),
                              "The 'clusteredIndex' legacy format {clusteredIndex: <bool>} is only "
                              "supported for specific internal collections and vice versa");
            }

            if (idIndex && !idIndex->isEmpty()) {
                return Status(
                    ErrorCodes::InvalidOptions,
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


        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        CollectionShardingState::get(opCtx, nss)->checkShardVersionOrThrow(opCtx);

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(
            opCtx,
            nss,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(nss.dbName()));

        // If the collection creation rolls back, ensure that the Top entry created for the
        // collection is deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

        // Even though 'collectionOptions' is passed by rvalue reference, it is not safe to move
        // because 'userCreateNS' may throw a WriteConflictException.
        if (idIndex == boost::none || collectionOptions.clusteredIndex) {
            status = db->userCreateNS(opCtx, nss, collectionOptions, /*createIdIndex=*/false);
        } else {
            status =
                db->userCreateNS(opCtx, nss, collectionOptions, /*createIdIndex=*/true, *idIndex);
        }
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
}

CollectionOptions clusterByDefaultIfNecessary(const NamespaceString& nss,
                                              CollectionOptions collectionOptions,
                                              const boost::optional<BSONObj>& idIndex) {
    if (MONGO_unlikely(clusterAllCollectionsByDefault.shouldFail()) &&
        !collectionOptions.isView() && !collectionOptions.clusteredIndex.has_value() &&
        (!idIndex || idIndex->isEmpty()) && !collectionOptions.capped &&
        !clustered_util::requiresLegacyFormat(nss) &&
        feature_flags::gClusteredIndexes.isEnabled(serverGlobalParams.featureCompatibility)) {
        // Capped, clustered collections differ in behavior significantly from normal
        // capped collections. Notably, they allow out-of-order insertion.
        //
        // Additionally, don't set the collection to be clustered in the default format if it
        // requires legacy format.
        collectionOptions.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    }
    return collectionOptions;
}

/**
 * Shared part of the implementation of the createCollection versions for replicated and regular
 * collection creation.
 */
Status createCollection(OperationContext* opCtx,
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

    CollectionOptions collectionOptions;
    {
        StatusWith<CollectionOptions> statusWith = CollectionOptions::parse(options, kind);
        if (!statusWith.isOK()) {
            return statusWith.getStatus();
        }
        collectionOptions = statusWith.getValue();
        bool hasExplicitlyDisabledClustering =
            options["clusteredIndex"].isBoolean() && !options["clusteredIndex"].boolean();
        if (!hasExplicitlyDisabledClustering) {
            collectionOptions =
                clusterByDefaultIfNecessary(nss, std::move(collectionOptions), idIndex);
        }
    }

    return createCollection(opCtx, nss, collectionOptions, idIndex);
}
}  // namespace

Status createCollection(OperationContext* opCtx,
                        const DatabaseName& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex) {
    return createCollection(opCtx,
                            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj),
                            cmdObj,
                            idIndex,
                            CollectionOptions::parseForCommand);
}

Status createCollection(OperationContext* opCtx, const CreateCommand& cmd) {
    auto options = CollectionOptions::fromCreateCommand(cmd);
    auto idIndex = std::exchange(options.idIndex, {});
    bool hasExplicitlyDisabledClustering = cmd.getClusteredIndex() &&
        stdx::holds_alternative<bool>(*cmd.getClusteredIndex()) &&
        !stdx::get<bool>(*cmd.getClusteredIndex());
    if (!hasExplicitlyDisabledClustering) {
        options = clusterByDefaultIfNecessary(cmd.getNamespace(), std::move(options), idIndex);
    }
    return createCollection(opCtx, cmd.getNamespace(), options, idIndex);
}

// TODO SERVER-62395 Pass DatabaseName instead of dbName, and pass to isDbLockedForMode.
Status createCollectionForApplyOps(OperationContext* opCtx,
                                   const std::string& dbname,
                                   const boost::optional<UUID>& ui,
                                   const BSONObj& cmdObj,
                                   const bool allowRenameOutOfTheWay,
                                   const boost::optional<BSONObj>& idIndex) {
    const DatabaseName dbName(boost::none, dbname);
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    const NamespaceString newCollName(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
    auto newCmd = cmdObj;

    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto* const db = databaseHolder->getDb(opCtx, dbName);

    // If a UUID is given, see if we need to rename a collection out of the way, and whether the
    // collection already exists under a different name. If so, rename it into place. As this is
    // done during replay of the oplog, the operations do not need to be atomic, just idempotent.
    // We need to do the renaming part in a separate transaction, as we cannot transactionally
    // create a database, which could result in createCollection failing if the database
    // does not yet exist.
    if (ui) {
        auto uuid = ui.value();
        uassert(ErrorCodes::InvalidUUID,
                "Invalid UUID in applyOps create command: " + uuid.toString(),
                uuid.isRFC4122v4());

        auto catalog = CollectionCatalog::get(opCtx);
        const auto currentName = catalog->lookupNSSByUUID(opCtx, uuid);
        auto serviceContext = opCtx->getServiceContext();
        auto opObserver = serviceContext->getOpObserver();
        if (currentName && *currentName == newCollName)
            return Status::OK();

        if (currentName && currentName->isDropPendingNamespace()) {
            LOGV2(20308,
                  "CMD: create -- existing collection with conflicting UUID is in a drop-pending "
                  "state",
                  "newCollection"_attr = newCollName,
                  "conflictingUUID"_attr = uuid,
                  "existingCollection"_attr = *currentName);
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "existing collection " << currentName->toString()
                                        << " with conflicting UUID " << uuid.toString()
                                        << " is in a drop-pending state.");
        }

        // In the case of oplog replay, a future command may have created or renamed a
        // collection with that same name. In that case, renaming this future collection to
        // a random temporary name is correct: once all entries are replayed no temporary
        // names will remain.
        const bool stayTemp = true;
        auto futureColl = db ? catalog->lookupCollectionByNamespace(opCtx, newCollName) : nullptr;
        bool needsRenaming(futureColl);
        invariant(!needsRenaming || allowRenameOutOfTheWay,
                  str::stream() << "Current collection name: " << currentName << ", UUID: " << uuid
                                << ". Future collection name: " << newCollName);

        for (int tries = 0; needsRenaming && tries < 10; ++tries) {
            auto tmpNameResult = makeUniqueCollectionName(opCtx, dbName, "tmp%%%%%.create");
            if (!tmpNameResult.isOK()) {
                return tmpNameResult.getStatus().withContext(str::stream()
                                                             << "Cannot generate temporary "
                                                                "collection namespace for applyOps "
                                                                "create command: collection: "
                                                             << newCollName);
            }

            const auto& tmpName = tmpNameResult.getValue();
            AutoGetCollection tmpCollLock(opCtx, tmpName, LockMode::MODE_X);
            if (tmpCollLock.getCollection()) {
                // Conflicting on generating a unique temp collection name. Try again.
                continue;
            }

            // It is ok to log this because this doesn't happen very frequently.
            LOGV2(20309,
                  "CMD: create -- renaming existing collection with conflicting UUID to "
                  "temporary collection",
                  "newCollection"_attr = newCollName,
                  "conflictingUUID"_attr = uuid,
                  "tempName"_attr = tmpName);
            Status status =
                writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName.ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    Status status = db->renameCollection(opCtx, newCollName, tmpName, stayTemp);
                    if (!status.isOK())
                        return status;
                    auto uuid = futureColl->uuid();
                    opObserver->onRenameCollection(opCtx,
                                                   newCollName,
                                                   tmpName,
                                                   uuid,
                                                   /*dropTargetUUID*/ {},
                                                   /*numRecords*/ 0U,
                                                   stayTemp);

                    wuow.commit();
                    // Re-fetch collection after commit to get a valid pointer
                    futureColl = CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, uuid);
                    return Status::OK();
                });

            if (!status.isOK()) {
                return status;
            }

            // Abort any remaining index builds on the temporary collection.
            IndexBuildsCoordinator::get(opCtx)->abortCollectionIndexBuilds(
                opCtx,
                tmpName,
                futureColl->uuid(),
                "Aborting index builds on temporary collection");

            // The existing collection has been successfully moved out of the way.
            needsRenaming = false;
        }
        if (needsRenaming) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Cannot generate temporary "
                                           "collection namespace for applyOps "
                                           "create command: collection: "
                                        << newCollName);
        }

        // If the collection with the requested UUID already exists, but with a different
        // name, just rename it to 'newCollName'.
        if (catalog->lookupCollectionByUUID(opCtx, uuid)) {
            invariant(currentName);
            uassert(40655,
                    str::stream() << "Invalid name " << newCollName << " for UUID " << uuid,
                    currentName->db() == newCollName.db());
            return writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName.ns(), [&] {
                WriteUnitOfWork wuow(opCtx);
                Status status = db->renameCollection(opCtx, *currentName, newCollName, stayTemp);
                if (!status.isOK())
                    return status;
                opObserver->onRenameCollection(opCtx,
                                               *currentName,
                                               newCollName,
                                               uuid,
                                               /*dropTargetUUID*/ {},
                                               /*numRecords*/ 0U,
                                               stayTemp);

                wuow.commit();
                return Status::OK();
            });
        }

        // A new collection with the specific UUID must be created, so add the UUID to the
        // creation options. Regular user collection creation commands cannot do this.
        auto uuidObj = uuid.toBSON();
        newCmd = cmdObj.addField(uuidObj.firstElement());
    }

    return createCollection(
        opCtx, newCollName, newCmd, idIndex, CollectionOptions::parseForStorage);
}

Status createCollection(OperationContext* opCtx,
                        const NamespaceString& ns,
                        const CollectionOptions& options,
                        const boost::optional<BSONObj>& idIndex) {
    auto status = userAllowedCreateNS(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    if (options.isView()) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create a view in a multi-document "
                                 "transaction.",
                !opCtx->inMultiDocumentTransaction());
        uassert(ErrorCodes::Error(6026500),
                "The 'clusteredIndex' option is not supported with views",
                !options.clusteredIndex);

        return _createView(opCtx, ns, options);
    } else if (options.timeseries && !ns.isTimeseriesBucketsCollection()) {
        // This helper is designed for user-created time-series collections on primaries. If a
        // time-series buckets collection is created explicitly or during replication, treat this as
        // a normal collection creation.
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream()
                    << "Cannot create a time-series collection in a multi-document transaction.",
                !opCtx->inMultiDocumentTransaction());
        return _createTimeseries(opCtx, ns, options);
    } else {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create system collection " << ns
                              << " within a transaction.",
                !opCtx->inMultiDocumentTransaction() || !ns.isSystem());
        return _createCollection(opCtx, ns, options, idIndex);
    }
}

}  // namespace mongo
