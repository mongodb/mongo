/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

// Causes the server to hang when it attempts to assign UUIDs to the provided database (or all
// databases if none are provided).
MONGO_FAIL_POINT_DEFINE(hangBeforeDatabaseUpgrade);

struct CollModRequest {
    const IndexDescriptor* idx = nullptr;
    BSONElement indexExpireAfterSeconds = {};
    BSONElement viewPipeLine = {};
    std::string viewOn = {};
    BSONElement collValidator = {};
    std::string collValidationAction = {};
    std::string collValidationLevel = {};
    BSONElement usePowerOf2Sizes = {};
    BSONElement noPadding = {};
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
                                  str::stream() << "cannot find index " << indexName << " for ns "
                                                << nss.ns());
                }
            } else {
                std::vector<IndexDescriptor*> indexes;
                coll->getIndexCatalog()->findIndexesByKeyPattern(
                    opCtx, keyPattern, false, &indexes);

                if (indexes.size() > 1) {
                    return Status(ErrorCodes::AmbiguousIndexKeyPattern,
                                  str::stream() << "index keyPattern " << keyPattern << " matches "
                                                << indexes.size()
                                                << " indexes,"
                                                << " must use index name. "
                                                << "Conflicting indexes:"
                                                << indexes[0]->infoObj()
                                                << ", "
                                                << indexes[1]->infoObj());
                } else if (indexes.empty()) {
                    return Status(ErrorCodes::IndexNotFound,
                                  str::stream() << "cannot find index " << keyPattern << " for ns "
                                                << nss.ns());
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

            // If the feature compatibility version is not 4.0, and we are validating features as
            // master, ban the use of new agg features introduced in 4.0 to prevent them from being
            // persisted in the catalog.
            boost::optional<ServerGlobalParams::FeatureCompatibility::Version>
                maxFeatureCompatibilityVersion;
            if (serverGlobalParams.validateFeaturesAsMaster.load() &&
                currentFCV !=
                    ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
                maxFeatureCompatibilityVersion = currentFCV;
            }
            auto statusW = coll->parseValidator(opCtx,
                                                e.Obj(),
                                                MatchExpressionParser::kDefaultSpecialFeatures,
                                                maxFeatureCompatibilityVersion);
            if (!statusW.isOK()) {
                return statusW.getStatus();
            }

            cmr.collValidator = e;
        } else if (fieldName == "validationLevel" && !isView) {
            auto statusW = coll->parseValidationLevel(e.String());
            if (!statusW.isOK())
                return statusW.getStatus();

            cmr.collValidationLevel = e.String();
        } else if (fieldName == "validationAction" && !isView) {
            auto statusW = coll->parseValidationAction(e.String());
            if (!statusW.isOK())
                return statusW.getStatus();

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
            // As of SERVER-17312 we only support these two options. When SERVER-17320 is
            // resolved this will need to be enhanced to handle other options.
            typedef CollectionOptions CO;

            if (fieldName == "usePowerOf2Sizes")
                cmr.usePowerOf2Sizes = e;
            else if (fieldName == "noPadding")
                cmr.noPadding = e;
            else
                return Status(ErrorCodes::InvalidOptions,
                              str::stream() << "unknown option to collMod: " << fieldName);
        }

        oplogEntryBuilder->append(e);
    }

    return {std::move(cmr)};
}

/**
 * Set a collection option flag for 'UsePowerOf2Sizes' or 'NoPadding'. Appends both the new and
 * old flag setting to the given 'result' builder.
 */
void setCollectionOptionFlag(OperationContext* opCtx,
                             Collection* coll,
                             BSONElement& collOptionElement,
                             BSONObjBuilder* result) {
    const StringData flagName = collOptionElement.fieldNameStringData();

    int flag;

    if (flagName == "usePowerOf2Sizes") {
        flag = CollectionOptions::Flag_UsePowerOf2Sizes;
    } else if (flagName == "noPadding") {
        flag = CollectionOptions::Flag_NoPadding;
    } else {
        flag = 0;
    }

    CollectionCatalogEntry* cce = coll->getCatalogEntry();

    const int oldFlags = cce->getCollectionOptions(opCtx).flags;
    const bool oldSetting = oldFlags & flag;
    const bool newSetting = collOptionElement.trueValue();

    result->appendBool(flagName.toString() + "_old", oldSetting);
    result->appendBool(flagName.toString() + "_new", newSetting);

    const int newFlags = newSetting ? (oldFlags | flag)    // set flag
                                    : (oldFlags & ~flag);  // clear flag

    // NOTE we do this unconditionally to ensure that we note that the user has
    // explicitly set flags, even if they are just setting the default.
    cce->updateFlags(opCtx, newFlags);

    const CollectionOptions newOptions = cce->getCollectionOptions(opCtx);
    invariant(newOptions.flags == newFlags);
    invariant(newOptions.flagsSet);
}

/**
 * If uuid is specified, add it to the collection specified by nss. This will error if the
 * collection already has a UUID.
 */
Status _collModInternal(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result,
                        bool upgradeUniqueIndexes,
                        OptionalCollectionUUID uuid) {
    StringData dbName = nss.db();
    AutoGetDb autoDb(opCtx, dbName, MODE_X);
    Database* const db = autoDb.getDb();
    Collection* coll = db ? db->getCollection(opCtx, nss) : nullptr;

    // May also modify a view instead of a collection.
    boost::optional<ViewDefinition> view;
    if (db && !coll) {
        const auto sharedView = db->getViewCatalog()->lookup(opCtx, nss.ns());
        if (sharedView) {
            // We copy the ViewDefinition as it is modified below to represent the requested state.
            view = {*sharedView};
        }
    }

    // This can kill all cursors so don't allow running it while a background operation is in
    // progress.
    BackgroundOperation::assertNoBgOpInProgForNs(nss);

    // If db/collection/view does not exist, short circuit and return.
    if (!db || (!coll && !view)) {
        return Status(ErrorCodes::NamespaceNotFound, "ns does not exist");
    }

    // This is necessary to set up CurOp and update the Top stats.
    OldClientContext ctx(opCtx, nss.ns());

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while setting collection options on "
                                    << nss.ns());
    }

    BSONObjBuilder oplogEntryBuilder;
    auto statusW = parseCollModRequest(opCtx, nss, coll, cmdObj, &oplogEntryBuilder);
    if (!statusW.isOK()) {
        return statusW.getStatus();
    }

    CollModRequest cmr = statusW.getValue();

    WriteUnitOfWork wunit(opCtx);

    // Handle collMod on a view and return early. The View Catalog handles the creation of oplog
    // entries for modifications on a view.
    if (view) {
        if (!cmr.viewPipeLine.eoo())
            view->setPipeline(cmr.viewPipeLine);

        if (!cmr.viewOn.empty())
            view->setViewOn(NamespaceString(dbName, cmr.viewOn));

        ViewCatalog* catalog = db->getViewCatalog();

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

    CollectionOptions oldCollOptions = coll->getCatalogEntry()->getCollectionOptions(opCtx);
    boost::optional<TTLCollModInfo> ttlInfo;

    // Handle collMod operation type appropriately.

    // TTLIndex
    if (!cmr.indexExpireAfterSeconds.eoo()) {
        BSONElement& newExpireSecs = cmr.indexExpireAfterSeconds;
        BSONElement oldExpireSecs = cmr.idx->infoObj().getField("expireAfterSeconds");

        if (SimpleBSONElementComparator::kInstance.evaluate(oldExpireSecs != newExpireSecs)) {
            result->appendAs(oldExpireSecs, "expireAfterSeconds_old");

            // Change the value of "expireAfterSeconds" on disk.
            coll->getCatalogEntry()->updateTTLSetting(
                opCtx, cmr.idx->indexName(), newExpireSecs.safeNumberLong());

            // Notify the index catalog that the definition of this index changed.
            cmr.idx = coll->getIndexCatalog()->refreshEntry(opCtx, cmr.idx);
            result->appendAs(newExpireSecs, "expireAfterSeconds_new");
            opCtx->recoveryUnit()->onRollback([ opCtx, idx = cmr.idx, coll ]() {
                coll->getIndexCatalog()->refreshEntry(opCtx, idx);
            });
        }

        // Save previous TTL index expiration.
        ttlInfo = TTLCollModInfo{Seconds(newExpireSecs.safeNumberLong()),
                                 Seconds(oldExpireSecs.safeNumberLong()),
                                 cmr.idx->indexName()};
    }

    // The Validator, ValidationAction and ValidationLevel are already parsed and must be OK.
    if (!cmr.collValidator.eoo())
        invariant(coll->setValidator(opCtx, cmr.collValidator.Obj()));
    if (!cmr.collValidationAction.empty())
        invariant(coll->setValidationAction(opCtx, cmr.collValidationAction));
    if (!cmr.collValidationLevel.empty())
        invariant(coll->setValidationLevel(opCtx, cmr.collValidationLevel));

    // UsePowerof2Sizes
    if (!cmr.usePowerOf2Sizes.eoo())
        setCollectionOptionFlag(opCtx, coll, cmr.usePowerOf2Sizes, result);

    // NoPadding
    if (!cmr.noPadding.eoo())
        setCollectionOptionFlag(opCtx, coll, cmr.noPadding, result);

    // Upgrade unique indexes
    if (upgradeUniqueIndexes) {
        // A cmdObj with an empty collMod, i.e. nFields = 1, implies that it is a Unique Index
        // upgrade collMod.
        invariant(cmdObj.nFields() == 1);
        std::vector<std::string> indexNames;
        coll->getCatalogEntry()->getAllUniqueIndexes(opCtx, &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            const IndexDescriptor* desc =
                coll->getIndexCatalog()->findIndexByName(opCtx, indexNames[i]);
            invariant(desc);

            // Update index metadata in storage engine.
            coll->getCatalogEntry()->updateIndexMetadata(opCtx, desc);

            // Refresh the in-memory instance of the index.
            desc = coll->getIndexCatalog()->refreshEntry(opCtx, desc);

            opCtx->recoveryUnit()->onRollback(
                [opCtx, desc, coll]() { coll->getIndexCatalog()->refreshEntry(opCtx, desc); });
        }
    }
    // Add collection UUID if it is missing. This returns an error if a collection already has a
    // different UUID. As we don't assign UUIDs to system.indexes (SERVER-29926), don't implicitly
    // upgrade them on collMod either.
    if (!nss.isSystemDotIndexes()) {
        if (uuid && !coll->uuid()) {
            log() << "Assigning UUID " << uuid.get().toString() << " to collection " << coll->ns();
            CollectionCatalogEntry* cce = coll->getCatalogEntry();
            cce->addUUID(opCtx, uuid.get(), coll);
        } else if (uuid && coll->uuid() && uuid.get() != coll->uuid().get()) {
            return Status(ErrorCodes::Error(40676),
                          str::stream() << "collMod " << redact(cmdObj) << " provides a UUID ("
                                        << uuid.get().toString()
                                        << ") that does not match the UUID ("
                                        << coll->uuid().get().toString()
                                        << ") of the collection "
                                        << nss.ns());
        }
        coll->refreshUUID(opCtx);
    }

    // Only observe non-view collMods, as view operations are observed as operations on the
    // system.views collection.
    getGlobalServiceContext()->getOpObserver()->onCollMod(
        opCtx, nss, coll->uuid(), oplogEntryBuilder.obj(), oldCollOptions, ttlInfo);

    wunit.commit();

    return Status::OK();
}

void _addCollectionUUIDsPerDatabase(OperationContext* opCtx,
                                    const std::string& dbname,
                                    std::map<std::string, UUID>& collToUUID) {
    // Iterate through all collections of database dbname and make necessary UUID changes.
    std::vector<NamespaceString> collNamespaceStrings;
    {
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        // If the database no longer exists, we're done with upgrading.
        if (!db) {
            return;
        }
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;
            collNamespaceStrings.push_back(coll->ns());
        }
    }
    for (auto& collNSS : collNamespaceStrings) {
        // Skip system.namespaces until SERVER-30095 is addressed.
        if (collNSS.coll() == "system.namespaces") {
            continue;
        }
        // Skip all non-replicated collections.
        if (collNSS.db() == "local" || collNSS.coll() == "system.profile") {
            continue;
        }

        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db ? db->getCollection(opCtx, collNSS) : nullptr;
        // If the collection no longer exists, skip it.
        if (!coll) {
            continue;
        }
        BSONObjBuilder collModObjBuilder;
        collModObjBuilder.append("collMod", coll->ns().coll());
        BSONObj collModObj = collModObjBuilder.done();
        OptionalCollectionUUID uuid = boost::none;
        if (collToUUID.find(collNSS.coll().toString()) != collToUUID.end()) {
            // This is a sharded collection. Use the UUID generated by the config server.
            uuid = collToUUID.at(collNSS.coll().toString());
        } else {
            // This is an unsharded collection. Generate a UUID.
            uuid = UUID::gen();
        }

        if (!coll->uuid()) {
            uassertStatusOK(collModForUUIDUpgrade(opCtx, coll->ns(), collModObj, uuid.get()));
        }
    }
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
                            /*upgradeUniqueIndexes*/ false,
                            /*UUID*/ boost::none);
}

Status collModWithUpgrade(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& cmdObj) {
    // A cmdObj with an empty collMod, i.e. nFields = 1, implies that it is a Unique Index
    // upgrade collMod.
    bool upgradeUniqueIndex = createTimestampSafeUniqueIndex && (cmdObj.nFields() == 1);

    // Update all non-replicated unique indexes on upgrade i.e. setFCV=4.2.
    if (upgradeUniqueIndex && nss == NamespaceString::kServerConfigurationNamespace) {
        auto schemaStatus = updateNonReplicatedUniqueIndexes(opCtx);
        if (!schemaStatus.isOK()) {
            return schemaStatus;
        }
    }

    BSONObjBuilder resultWeDontCareAbout;
    return _collModInternal(opCtx,
                            nss,
                            cmdObj,
                            &resultWeDontCareAbout,
                            upgradeUniqueIndex,
                            /*UUID*/ boost::none);
}

Status collModForUUIDUpgrade(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& cmdObj,
                             CollectionUUID uuid) {
    BSONObjBuilder resultWeDontCareAbout;
    return _collModInternal(opCtx,
                            nss,
                            cmdObj,
                            &resultWeDontCareAbout,
                            /* upgradeUniqueIndexes */ false,
                            uuid);
}

void addCollectionUUIDs(OperationContext* opCtx) {
    // A map of the form { db1: { collB: UUID, collA: UUID, ... }, db2: { ... } }
    std::map<std::string, std::map<std::string, UUID>> dbToCollToUUID;
    if (ShardingState::get(opCtx)->enabled()) {
        log() << "obtaining UUIDs for pre-existing sharded collections from config server";

        // Get UUIDs for all existing sharded collections from the config server. Since the sharded
        // collections are not stored per-database in config.collections, it's more efficient to
        // read all the collections at once than to read them by database.
        auto shardedColls =
            uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    repl::ReadConcernLevel::kMajorityReadConcern,
                    CollectionType::ConfigNS,
                    BSON("dropped" << false),  // query
                    BSONObj(),                 // sort
                    boost::none                // limit
                    ))
                .docs;

        for (const auto& coll : shardedColls) {
            auto collType = uassertStatusOK(CollectionType::fromBSON(coll));
            uassert(ErrorCodes::InternalError,
                    str::stream() << "expected entry " << coll << " in config.collections for "
                                  << collType.getNs().ns()
                                  << " to have a UUID, but it did not",
                    collType.getUUID());
            dbToCollToUUID[collType.getNs().db().toString()].emplace(
                collType.getNs().coll().toString(), *collType.getUUID());
        }
    }

    // Add UUIDs to all collections of all databases if they do not already have UUIDs.
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        storageEngine->listDatabases(&dbNames);
    }

    for (auto it = dbNames.begin(); it != dbNames.end(); ++it) {
        auto dbName = *it;

        MONGO_FAIL_POINT_BLOCK(hangBeforeDatabaseUpgrade, customArgs) {
            const auto& data = customArgs.getData();
            const auto dbElem = data["database"];
            if (!dbElem || dbElem.checkAndGetStringData() == dbName) {
                log() << "collMod - hangBeforeDatabaseUpgrade fail point enabled for " << dbName
                      << ". Blocking until fail point is disabled.";
                while (MONGO_FAIL_POINT(hangBeforeDatabaseUpgrade)) {
                    mongo::sleepsecs(1);
                }
            }
        }

        _addCollectionUUIDsPerDatabase(opCtx, dbName, dbToCollToUUID[dbName]);
    }

    const auto& clientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
    auto awaitOpTime = clientInfo.getLastOp();

    log() << "Finished adding UUIDs to collections, waiting for all UUIDs to be committed at optime"
          << awaitOpTime << ".";

    const WriteConcernOptions writeConcern(WriteConcernOptions::kMajority,
                                           WriteConcernOptions::SyncMode::UNSET,
                                           /*timeout*/ INT_MAX);
    repl::ReplicationCoordinator::get(opCtx)->awaitReplication(opCtx, awaitOpTime, writeConcern);
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
                                          /*upgradeUniqueIndexes*/ true,
                                          /*UUID*/ boost::none);
    return collModStatus;
}

Status _updateNonReplicatedUniqueIndexesPerDatabase(OperationContext* opCtx,
                                                    const std::string& dbName) {
    AutoGetDb autoDb(opCtx, dbName, MODE_X);
    Database* const db = autoDb.getDb();

    // Iterate through all collections if we're in the "local" database.
    if (dbName == "local") {
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;

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

        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;
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
    if (!createTimestampSafeUniqueIndex)
        return;

    // Update all unique indexes except the _id index.
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        storageEngine->listDatabases(&dbNames);
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

    const WriteConcernOptions writeConcern(WriteConcernOptions::kMajority,
                                           WriteConcernOptions::SyncMode::UNSET,
                                           /*timeout*/ INT_MAX);
    repl::ReplicationCoordinator::get(opCtx)->awaitReplication(opCtx, awaitOpTime, writeConcern);
}

Status updateNonReplicatedUniqueIndexes(OperationContext* opCtx) {
    if (!createTimestampSafeUniqueIndex)
        return Status::OK();

    // Update all unique indexes belonging to all non-replicated collections.
    // (_id indexes are not updated).
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    {
        Lock::GlobalLock lk(opCtx, MODE_IS);
        storageEngine->listDatabases(&dbNames);
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
