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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterDatabaseLock);
MONGO_FAIL_POINT_DEFINE(assertAfterIndexUpdate);

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement viewPipeLine = {};
    std::string viewOn = {};
    boost::optional<BSONObj> collValidator;
    boost::optional<std::string> collValidationAction;
    boost::optional<std::string> collValidationLevel;
};

StatusWith<CollModRequest> parseCollModRequest(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               Collection* coll,
                                               const BSONObj& cmdObj,
                                               BSONObjBuilder* oplogEntryBuilder) {

    bool isView = !coll;

    CollModRequest cmr;

    BSONForEach(e, cmdObj) {
        const auto fieldName = e.fieldNameStringData();
        if (isGenericArgument(fieldName)) {
            continue;  // Don't add to oplog builder.
        } else if (fieldName == "collMod") {
            // no-op
        } else if (fieldName == "index" && !isView) {
            BSONObj indexObj = e.Obj();
            StringData indexName;
            BSONObj keyPattern;

            BSONElement nameElem = indexObj["name"];
            BSONElement keyPatternElem = indexObj["keyPattern"];
            if (nameElem && keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Cannot specify both key pattern and name.");
            }

            if (!nameElem && !keyPatternElem) {
                return Status(ErrorCodes::InvalidOptions,
                              "Must specify either index name or key pattern.");
            }

            if (nameElem) {
                if (nameElem.type() != BSONType::String) {
                    return Status(ErrorCodes::InvalidOptions, "Index name must be a string.");
                }
                indexName = nameElem.valueStringData();
            }

            if (keyPatternElem) {
                if (keyPatternElem.type() != BSONType::Object) {
                    return Status(ErrorCodes::InvalidOptions, "Key pattern must be an object.");
                }
                keyPattern = keyPatternElem.embeddedObject();
            }

            cmr.indexExpireAfterSeconds = indexObj["expireAfterSeconds"];
            if (cmr.indexExpireAfterSeconds.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field");
            }
            if (!cmr.indexExpireAfterSeconds.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "expireAfterSeconds field must be a number");
            }

            if (!indexName.empty()) {
                cmr.idx = coll->getIndexCatalog()->findIndexByName(opCtx, indexName);
                if (!cmr.idx) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << indexName << " for ns " << nss);
                }
            } else {
                std::vector<const IndexDescriptor*> indexes;
                coll->getIndexCatalog()->findIndexesByKeyPattern(
                    opCtx, keyPattern, false, &indexes);

                if (indexes.size() > 1) {
                    return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                                  str::stream() << "index keyPattern " << keyPattern << " matches "
                                                << indexes.size() << " indexes,"
                                                << " must use index name. "
                                                << "Conflicting indexes:" << indexes[0]->infoObj()
                                                << ", " << indexes[1]->infoObj());
                } else if (indexes.empty()) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream()
                                      << "cannot find index " << keyPattern << " for ns " << nss);
                }

                cmr.idx = indexes[0];
            }

            BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");
            if (oldExpireSecs.eoo()) {
                return Status(ErrorCodes::InvalidOptions, "no expireAfterSeconds field to update");
            }
            if (!oldExpireSecs.isNumber()) {
                return Status(ErrorCodes::InvalidOptions,
                              "existing expireAfterSeconds field is not a number");
            }

        } else if (fieldName == "validator" && !isView) {
            // Save this to a variable to avoid reading the atomic variable multiple times.
            const auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();

            // If the feature compatibility version is not 4.2, and we are validating features as
            // master, ban the use of new agg features introduced in 4.2 to prevent them from being
            // persisted in the catalog.
            boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                maxFeatureCompatibilityVersion;
            if (serverGlobalParams.validateFeaturesAsMaster.load() &&
                currentFCV !=
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
                maxFeatureCompatibilityVersion = currentFCV;
            }
            auto swValidator = coll->parseValidator(opCtx,
                                                    e.Obj(),
                                                    MatchExpressionParser::kDefaultSpecialFeatures,
                                                    maxFeatureCompatibilityVersion);
            if (!swValidator.isOK()) {
                return swValidator.getStatus();
            }
            cmr.collValidator = e.embeddedObject().getOwned();
        } else if (fieldName == "validationLevel" && !isView) {
            auto status = coll->parseValidationLevel(e.String());
            if (!status.isOK())
                return status;

            cmr.collValidationLevel = e.String();
        } else if (fieldName == "validationAction" && !isView) {
            auto status = coll->parseValidationAction(e.String());
            if (!status.isOK())
                return status;

            cmr.collValidationAction = e.String();
        } else if (fieldName == "pipeline") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'pipeline' option only supported on a view");
            }
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::InvalidOptions, "not a valid aggregation pipeline");
            }
            cmr.viewPipeLine = e;
        } else if (fieldName == "viewOn") {
            if (!isView) {
                return Status(ErrorCodes::InvalidOptions,
                              "'viewOn' option only supported on a view");
            }
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::InvalidOptions, "'viewOn' option must be a string");
            }
            cmr.viewOn = e.str();
        } else {
            if (isView) {
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "option not supported on a view: " << fieldName);
            }

            return Status(ErrorCodes::InvalidOptions,
                          str::stream() << "unknown option to collMod: " << fieldName);
        }

        oplogEntryBuilder->append(e);
    }

    return {std::move(cmr)};
}

/**
 * If uuid is specified, add it to the collection specified by nss. This will error if the
 * collection already has a UUID.
 */
Status _collModInternal(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result,
                        bool upgradeUniqueIndexes) {
    StringData dbName = nss.db();
    AutoGetCollection autoColl(
        opCtx, nss, MODE_IX, MODE_X, AutoGetCollection::ViewMode::kViewsPermitted);
    Lock::CollectionLock systemViewsLock(
        opCtx, NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName), MODE_X);

    Database* const db = autoColl.getDb();
    Collection* coll = autoColl.getCollection();

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterDatabaseLock);

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (db && !coll) {
        const auto sharedView = ViewCatalog::get(db)->lookup(opCtx, nss.ns());
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    BackgroundOperation::assertNoBgOpInProgForNs(nss);
    if (coll) {
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            coll->uuid().get());
    }

    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp and update the Top stats.
    OldClientContext ctx(opCtx, nss.ns());

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while setting collection options on " << nss);
    }

    BSONObjBuilder oplogEntryBuilder;
    auto statusW = parseCollModRequest(opCtx, nss, coll, cmdObj, &oplogEntryBuilder);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }

    CollModRequest cmr = statusW.getValue();

    if (!serverGlobalParams.quiet.load()) {
        log() << "CMD: collMod: " << cmdObj;
    }

    WriteUnitOfWork wunit(opCtx);

    // Handle collMod on a view and return early. The View Catalog handles the creation of oplog
    // entries for modifications on a view.
    if (view) {
        if (!cmr.viewPipeLine.eoo())
            view->setPipeline(cmr.viewPipeLine);

        if (!cmr.viewOn.empty())
            view->setViewOn(NamespaceString(dbName, cmr.viewOn));

        ViewCatalog* catalog = ViewCatalog::get(db);

        BSONArrayBuilder pipeline;
        for (auto& item : view->pipeline()) {
            pipeline.append(item);
        }
        auto errorStatus =
            catalog->modifyView(opCtx, nss, view->viewOn(), BSONArray(pipeline.obj()));
        if (!errorStatus.isOK()) {
            return errorStatus;
        }

        wunit.commit();
        return Status::OK();
    }

    // In order to facilitate the replication rollback process, which makes a best effort attempt to
    // "undo" a set of oplog operations, we store a snapshot of the old collection options to
    // provide to the OpObserver. TTL index updates aren't a part of collection options so we
    // save the relevant TTL index data in a separate object.

    CollectionOptions oldCollOptions = DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, nss);

    boost::optional<TTLCollModInfo> ttlInfo;

    // Handle collMod operation type appropriately.

    // TTLIndex
    if (!cmr.indexExpireAfterSeconds.eoo()) {
        BSONElement& newExpireSecs = cmr.indexExpireAfterSeconds;
        BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");

        if (SimpleBSONElementComparator::kInstance.evaluate(oldExpireSecs != newExpireSecs)) {
            result->appendAs(oldExpireSecs, "expireAfterSeconds_old");

            // Change the value of "expireAfterSeconds" on disk.
            DurableCatalog::get(opCtx)->updateTTLSetting(
                opCtx, coll->ns(), cmr.idx->indexName(), newExpireSecs.safeNumberLong());

            // Notify the index catalog that the definition of this index changed.
            cmr.idx = coll->getIndexCatalog()->refreshEntry(opCtx, cmr.idx);
            result->appendAs(newExpireSecs, "expireAfterSeconds_new");

            if (MONGO_FAIL_POINT(assertAfterIndexUpdate)) {
                log() << "collMod - assertAfterIndexUpdate fail point enabled.";
                uasserted(50970, "trigger rollback after the index update");
            }
        }

        // Save previous TTL index expiration.
        ttlInfo = TTLCollModInfo{Seconds(newExpireSecs.safeNumberLong()),
                                 Seconds(oldExpireSecs.safeNumberLong()),
                                 cmr.idx->indexName()};
    }

    if (cmr.collValidator) {
        uassertStatusOK(coll->setValidator(opCtx, std::move(*cmr.collValidator)));
    }
    if (cmr.collValidationAction)
        uassertStatusOKWithContext(coll->setValidationAction(opCtx, *cmr.collValidationAction),
                                   "Failed to set validationAction");
    if (cmr.collValidationLevel) {
        uassertStatusOKWithContext(coll->setValidationLevel(opCtx, *cmr.collValidationLevel),
                                   "Failed to set validationLevel");
    }

    // Upgrade unique indexes
    if (upgradeUniqueIndexes) {
        // A cmdObj with an empty collMod, i.e. nFields = 1, implies that it is a Unique Index
        // upgrade collMod.
        invariant(cmdObj.nFields() == 1);
        std::vector<std::string> indexNames;
        DurableCatalog::get(opCtx)->getAllUniqueIndexes(opCtx, nss, &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            const IndexDescriptor* desc =
                coll->getIndexCatalog()->findIndexByName(opCtx, indexNames[i]);
            invariant(desc);

            // Update index metadata in storage engine.
            DurableCatalog::get(opCtx)->updateIndexMetadata(opCtx, nss, desc);

            // Refresh the in-memory instance of the index.
            desc = coll->getIndexCatalog()->refreshEntry(opCtx, desc);

            if (MONGO_FAIL_POINT(assertAfterIndexUpdate)) {
                log() << "collMod - assertAfterIndexUpdate fail point enabled.";
                uasserted(50971, "trigger rollback for unique index update");
            }
        }
    }

    // Only observe non-view collMods, as view operations are observed as operations on the
    // system.views collection.
    getGlobalServiceContext()->getOpObserver()->onCollMod(
        opCtx, nss, coll->uuid(), oplogEntryBuilder.obj(), oldCollOptions, ttlInfo);

    wunit.commit();

    return Status::OK();
}

}  // namespace

Status collMod(OperationContext* opCtx,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    return _collModInternal(opCtx,
                            nss,
                            cmdObj,
                            result,
                            /*upgradeUniqueIndexes*/ false);
}

Status collModWithUpgrade(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& cmdObj) {
    // An empty collMod is used to upgrade unique index during FCV upgrade. If an application
    // executes the empty collMod when the secondary is upgrading FCV it is fine to upgrade the
    // unique index becuase the secondary will eventually get the real empty collMod. If the
    // application issues an empty collMod when FCV is not upgrading or upgraded to 4.2 then the
    // unique index should not be upgraded due to this collMod on the secondary.
    bool upgradeUniqueIndex =
        (cmdObj.nFields() == 1 && serverGlobalParams.featureCompatibility.isVersionInitialized() &&
         serverGlobalParams.featureCompatibility.isVersionUpgradingOrUpgraded());

    // Update all non-replicated unique indexes on upgrade i.e. setFCV=4.2.
    if (upgradeUniqueIndex && nss == NamespaceString::kServerConfigurationNamespace) {
        auto schemaStatus = updateNonReplicatedUniqueIndexes(opCtx);
        if (!schemaStatus.isOK()) {
            return schemaStatus;
        }
    }

    BSONObjBuilder resultWeDontCareAbout;
    return _collModInternal(opCtx, nss, cmdObj, &resultWeDontCareAbout, upgradeUniqueIndex);
}

Status _updateNonReplicatedIndexPerCollection(OperationContext* opCtx, Collection* coll) {
    BSONObjBuilder collModObjBuilder;
    collModObjBuilder.append("collMod", coll->ns().coll());
    BSONObj collModObj = collModObjBuilder.done();

    BSONObjBuilder resultWeDontCareAbout;
    auto collModStatus = _collModInternal(opCtx,
                                          coll->ns(),
                                          collModObj,
                                          &resultWeDontCareAbout,
                                          /*upgradeUniqueIndexes*/ true);
    return collModStatus;
}

Status _updateNonReplicatedUniqueIndexesPerDatabase(OperationContext* opCtx,
                                                    const std::string& dbName) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X);
    Database* const db = autoDb.getDb();

    // Iterate through all collections if we're in the "local" database.
    if (dbName == "local") {
        for (auto collectionIt = db->begin(opCtx); collectionIt != db->end(opCtx); ++collectionIt) {
            Collection* coll = *collectionIt;
            if (!coll) {
                break;
            }

            auto collModStatus = _updateNonReplicatedIndexPerCollection(opCtx, coll);
            if (!collModStatus.isOK())
                return collModStatus;
        }
    } else {
        // If we're not in the "local" database, the only non-replicated collection
        // could be system.profile.
        Collection* coll =
            db ? db->getCollection(opCtx, NamespaceString(dbName, "system.profile")) : nullptr;
        if (!coll)
            return Status::OK();

        auto collModStatus = _updateNonReplicatedIndexPerCollection(opCtx, coll);
        if (!collModStatus.isOK())
            return collModStatus;
    }
    return Status::OK();
}

void _updateUniqueIndexesForDatabase(OperationContext* opCtx, const std::string& dbname) {
    // Iterate through all replicated collections of the database, for unique index update.
    // Non-replicated unique indexes are updated via the upgrade of admin.system.version
    // collection.
    {
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        // If the database no longer exists, nothing more to do.
        if (!db)
            return;

        for (auto collectionIt = db->begin(opCtx); collectionIt != db->end(opCtx); ++collectionIt) {
            Collection* coll = *collectionIt;
            if (!coll) {
                break;
            }

            NamespaceString collNSS = coll->ns();

            // Skip non-replicated collection.
            if (collNSS.coll() == "system.profile")
                continue;

            BSONObjBuilder collModObjBuilder;
            collModObjBuilder.append("collMod", collNSS.coll());
            BSONObj collModObj = collModObjBuilder.done();

            uassertStatusOK(collModWithUpgrade(opCtx, collNSS, collModObj));
        }
    }
}

void updateUniqueIndexesOnUpgrade(OperationContext* opCtx) {
    // Update all unique indexes except the _id index.
    std::vector<std::string> dbNames;
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        dbNames = CollectionCatalog::get(opCtx).getAllDbNames();
    }

    for (auto it = dbNames.begin(); it != dbNames.end(); ++it) {
        auto dbName = *it;

        // Non-replicated unique indexes are updated via the upgrade of admin.system.version
        // collection.
        if (dbName != "local")
            _updateUniqueIndexesForDatabase(opCtx, dbName);
    }

    const auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    auto awaitOpTime = clientInfo.getLastOp();

    log() << "Finished updating version of unique indexes for upgrade, waiting for all"
          << " index updates to be committed at optime " << awaitOpTime;

    auto timeout = opCtx->getWriteConcern().usedDefault ? WriteConcernOptions::kNoTimeout
                                                        : opCtx->getWriteConcern().wTimeout;
    const WriteConcernOptions writeConcern(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, timeout);

    uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)
                        ->awaitReplication(opCtx, awaitOpTime, writeConcern)
                        .status);
}

Status updateNonReplicatedUniqueIndexes(OperationContext* opCtx) {
    // Update all unique indexes belonging to all non-replicated collections.
    // (_id indexes are not updated).
    std::vector<std::string> dbNames;
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        dbNames = CollectionCatalog::get(opCtx).getAllDbNames();
    }
    for (auto it = dbNames.begin(); it != dbNames.end(); ++it) {
        auto dbName = *it;
        auto schemaStatus = _updateNonReplicatedUniqueIndexesPerDatabase(opCtx, dbName);
        if (!schemaStatus.isOK()) {
            return schemaStatus;
        }
    }
    return Status::OK();
}

}  // namespace mongo
