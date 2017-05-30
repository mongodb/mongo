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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/coll_mod.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/views/view_catalog.h"

namespace mongo {

namespace {

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
        if (Command::isGenericArgument(fieldName)) {
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
            auto statusW = coll->parseValidator(e.Obj());
            if (!statusW.isOK())
                return statusW.getStatus();

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

Status _collModInternal(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        BSONObjBuilder* result,
                        bool upgradeUUID,
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

    OldClientContext ctx(opCtx, nss.ns());

    bool userInitiatedWritesAndNotPrimary = opCtx->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nss);

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
        }

        // Save previous TTL index expiration.
        ttlInfo = TTLCollModInfo{Seconds(newExpireSecs.safeNumberLong()),
                                 Seconds(oldExpireSecs.safeNumberLong()),
                                 cmr.idx->indexName()};
    }

    // Validator
    if (!cmr.collValidator.eoo())
        coll->setValidator(opCtx, cmr.collValidator.Obj()).transitional_ignore();

    // ValidationAction
    if (!cmr.collValidationAction.empty())
        coll->setValidationAction(opCtx, cmr.collValidationAction).transitional_ignore();

    // ValidationLevel
    if (!cmr.collValidationLevel.empty())
        coll->setValidationLevel(opCtx, cmr.collValidationLevel).transitional_ignore();

    // UsePowerof2Sizes
    if (!cmr.usePowerOf2Sizes.eoo())
        setCollectionOptionFlag(opCtx, coll, cmr.usePowerOf2Sizes, result);

    // NoPadding
    if (!cmr.noPadding.eoo())
        setCollectionOptionFlag(opCtx, coll, cmr.noPadding, result);

    // Modify collection UUID if we are upgrading or downgrading. This is a no-op if we have
    // already upgraded or downgraded.
    if (upgradeUUID) {
        if (uuid && !coll->uuid()) {
            CollectionCatalogEntry* cce = coll->getCatalogEntry();
            cce->addUUID(opCtx, uuid.get(), coll);
        } else if (!uuid && coll->uuid()) {
            CollectionCatalogEntry* cce = coll->getCatalogEntry();
            cce->removeUUID(opCtx);
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

void _updateDBSchemaVersion(OperationContext* opCtx,
                            const std::string& dbname,
                            bool needUUIDAdded) {
    // Iterate through all collections of database dbname and make necessary UUID changes.
    std::vector<NamespaceString> collNamespaceStrings;
    {
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
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
        Collection* coll = db->getCollection(opCtx, collNSS);
        BSONObjBuilder collModObjBuilder;
        collModObjBuilder.append("collMod", coll->ns().coll());
        BSONObj collModObj = collModObjBuilder.done();
        OptionalCollectionUUID uuid = boost::none;
        if (needUUIDAdded) {
            uuid = UUID::gen();
        }
        if ((needUUIDAdded && !coll->uuid()) || (!needUUIDAdded && coll->uuid())) {
            uassertStatusOK(collModForUUIDUpgrade(opCtx, coll->ns(), collModObj, uuid));
        }
    }
}

void _updateDBSchemaVersionNonReplicated(OperationContext* opCtx,
                                         const std::string& dbname,
                                         bool needUUIDAdded) {
    // Iterate through all collections if we're in the "local" database.
    std::vector<NamespaceString> collNamespaceStrings;
    if (dbname == "local") {
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        for (auto collectionIt = db->begin(); collectionIt != db->end(); ++collectionIt) {
            Collection* coll = *collectionIt;
            collNamespaceStrings.push_back(coll->ns());
        }
    } else {
        // If we're not in the "local" database, the only non-replicated collection
        // is system.profile, if present.
        collNamespaceStrings.push_back(NamespaceString(dbname, "system.profile"));
    }
    for (auto& collNSS : collNamespaceStrings) {
        // Skip system.namespaces until SERVER-30095 is addressed.
        if (collNSS.coll() == "system.namespaces") {
            continue;
        }
        AutoGetDb autoDb(opCtx, dbname, MODE_X);
        Database* const db = autoDb.getDb();
        Collection* coll = db->getCollection(opCtx, collNSS);
        if (!coll) {
            // This will only occur if we incorrectly assumed there was a
            // system.profile collection present.
            return;
        }
        BSONObjBuilder collModObjBuilder;
        collModObjBuilder.append("collMod", coll->ns().coll());
        BSONObj collModObj = collModObjBuilder.done();
        OptionalCollectionUUID uuid = boost::none;
        if (needUUIDAdded) {
            uuid = UUID::gen();
        }
        if ((needUUIDAdded && !coll->uuid()) || (!needUUIDAdded && coll->uuid())) {
            BSONObjBuilder resultWeDontCareAbout;
            uassertStatusOK(_collModInternal(
                opCtx, coll->ns(), collModObj, &resultWeDontCareAbout, /*upgradeUUID*/ true, uuid));
        }
    }
}

void updateUUIDSchemaVersionNonReplicated(OperationContext* opCtx, bool upgrade) {
    if (!enableCollectionUUIDs) {
        return;
    }
    // Update UUIDs on all collections of all non-replicated databases.
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    {
        Lock::GlobalLock lk(opCtx, MODE_IS, UINT_MAX);
        storageEngine->listDatabases(&dbNames);
    }
    for (auto it = dbNames.begin(); it != dbNames.end(); ++it) {
        auto dbName = *it;
        _updateDBSchemaVersionNonReplicated(opCtx, dbName, upgrade);
    }
}
}  // namespace

Status collMod(OperationContext* opCtx,
               const NamespaceString& nss,
               const BSONObj& cmdObj,
               BSONObjBuilder* result) {
    return _collModInternal(
        opCtx, nss, cmdObj, result, /*upgradeUUID*/ false, /*UUID*/ boost::none);
}

Status collModForUUIDUpgrade(OperationContext* opCtx,
                             const NamespaceString& nss,
                             const BSONObj& cmdObj,
                             OptionalCollectionUUID uuid) {
    BSONObjBuilder resultWeDontCareAbout;
    // First update all non-replicated collection UUIDs.
    updateUUIDSchemaVersionNonReplicated(opCtx, !!uuid);
    return _collModInternal(opCtx, nss, cmdObj, &resultWeDontCareAbout, /*upgradeUUID*/ true, uuid);
}

void updateUUIDSchemaVersion(OperationContext* opCtx, bool upgrade) {
    if (!enableCollectionUUIDs) {
        return;
    }
    // Update UUIDs on all collections of all databases.
    std::vector<std::string> dbNames;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    {
        Lock::GlobalLock lk(opCtx, MODE_IS, UINT_MAX);
        storageEngine->listDatabases(&dbNames);
    }

    for (auto it = dbNames.begin(); it != dbNames.end(); ++it) {
        auto dbName = *it;
        _updateDBSchemaVersion(opCtx, dbName, upgrade);
    }
    const WriteConcernOptions writeConcern(WriteConcernOptions::kMajority,
                                           WriteConcernOptions::SyncMode::UNSET,
                                           /*timeout*/ INT_MAX);
    repl::getGlobalReplicationCoordinator()->awaitReplicationOfLastOpForClient(opCtx, writeConcern);
}
}  // namespace mongo
