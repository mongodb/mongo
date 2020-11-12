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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/create_collection.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
void _createSystemDotViewsIfNecessary(OperationContext* opCtx, const Database* db) {
    // Create 'system.views' in a separate WUOW if it does not exist.
    if (!CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx,
                                                                   db->getSystemViewsName())) {
        WriteUnitOfWork wuow(opCtx);
        invariant(db->createCollection(opCtx, db->getSystemViewsName()));
        wuow.commit();
    }
}

Status _createView(OperationContext* opCtx,
                   const NamespaceString& nss,
                   CollectionOptions&& collectionOptions,
                   const BSONObj& idIndex) {
    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // Operations all lock system.views in the end to prevent deadlock.
        Lock::CollectionLock systemViewsLock(
            opCtx,
            NamespaceString(nss.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        Database* db = autoDb.getDb();

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        _createSystemDotViewsIfNecessary(opCtx, db);

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(
            opCtx,
            nss,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx).getDatabaseProfileLevel(nss.db()));

        // If the view creation rolls back, ensure that the Top entry created for the view is
        // deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

        // Even though 'collectionOptions' is passed by rvalue reference, it is not safe to move
        // because 'userCreateNS' may throw a WriteConflictException.
        Status status = db->userCreateNS(opCtx, nss, collectionOptions, true, idIndex);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
}

Status _createTimeseries(OperationContext* opCtx,
                         const NamespaceString& ns,
                         CollectionOptions&& options) {
    auto bucketsNs = ns.makeTimeseriesBucketsNamespace();

    options.viewOn = bucketsNs.coll().toString();

    // The time field cannot be sparse, so we use it as the exit condition for the loop.
    auto assembleData =
        "function(dataArray) { \
                const assembledData = []; \
                if (dataArray.length === 0) { \
                    return assembledData; \
                } \
                for (let i = 0;;i++) { \
                    const assembledObj = {}; \
                    for (const elem of dataArray) { \
                        if (elem.v.hasOwnProperty(i)) { \
                            assembledObj[elem.k] = elem.v[i]; \
                        } else if (elem.k === '" +
        options.timeseries->getTimeField() +
        "') { \
                            return assembledData; \
                        } \
                    } \
                    assembledData.push(assembledObj); \
                } \
            }";
    options.pipeline =
        BSON_ARRAY(BSON("$project" << BSON("dataArray" << BSON("$objectToArray"
                                                               << "$data")))
                   << BSON("$project" << BSON(
                               "assembledData" << BSON(
                                   "$function" << BSON("body" << assembleData << "args"
                                                              << BSON_ARRAY("$dataArray") << "lang"
                                                              << "js"))))
                   << BSON("$unwind"
                           << "$assembledData")
                   << BSON("$replaceWith"
                           << "$assembledData"));

    return writeConflictRetry(opCtx, "create", ns.ns(), [&]() -> Status {
        AutoGetCollection autoColl(opCtx, ns, MODE_IX, AutoGetCollectionViewMode::kViewsPermitted);
        Lock::CollectionLock bucketsCollLock(opCtx, bucketsNs, MODE_IX);
        Lock::CollectionLock systemDotViewsLock(
            opCtx,
            NamespaceString(ns.db(), NamespaceString::kSystemDotViewsCollectionName),
            MODE_X);

        // This is a top-level handler for time-series creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        if (CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, ns)) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection already exists. NS: " << ns);
        }

        auto db = autoColl.ensureDbExists();
        if (ViewCatalog::get(db)->lookup(opCtx, ns.ns())) {
            return {ErrorCodes::NamespaceExists,
                    str::stream() << "A view already exists. NS: " << ns};
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, ns)) {
            return {ErrorCodes::NotWritablePrimary,
                    str::stream() << "Not primary while creating collection " << ns};
        }

        _createSystemDotViewsIfNecessary(opCtx, db);

        WriteUnitOfWork wuow(opCtx);

        AutoStatsTracker statsTracker(
            opCtx,
            ns,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx).getDatabaseProfileLevel(ns.db()));

        AutoStatsTracker bucketsStatsTracker(
            opCtx,
            bucketsNs,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx).getDatabaseProfileLevel(ns.db()));

        // If the buckets collection and time-series view creation roll back, ensure that their Top
        // entries are deleted.
        opCtx->recoveryUnit()->onRollback(
            [serviceContext = opCtx->getServiceContext(), &ns, &bucketsNs]() {
                Top::get(serviceContext).collectionDropped(ns);
                Top::get(serviceContext).collectionDropped(bucketsNs);
            });

        // Create the buckets collection that will back the view.
        invariant(db->createCollection(opCtx, bucketsNs),
                  str::stream() << "Failed to create buckets collection " << bucketsNs
                                << " for time-series collection " << ns);

        // Create the time-series view. Even though 'options' is passed by rvalue reference, it is
        // not safe to move because 'userCreateNS' may throw a WriteConflictException.
        auto status = db->userCreateNS(opCtx, ns, options);
        if (!status.isOK()) {
            return status.withContext(str::stream() << "Failed to create view on " << bucketsNs
                                                    << " for time-series collection " << ns
                                                    << " with options " << options.toBSON());
        }

        wuow.commit();

        return Status::OK();
    });
}

Status _createCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         CollectionOptions&& collectionOptions,
                         const BSONObj& idIndex) {
    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // This is a top-level handler for collection creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        if (CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss)) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection already exists. NS: " << nss);
        }
        if (ViewCatalog::get(autoDb.getDb())->lookup(opCtx, nss.ns())) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "A view already exists. NS: " << nss);
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotWritablePrimary,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(
            opCtx,
            nss,
            Top::LockType::NotLocked,
            AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
            CollectionCatalog::get(opCtx).getDatabaseProfileLevel(nss.db()));

        // If the collection creation rolls back, ensure that the Top entry created for the
        // collection is deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

        // Even though 'collectionOptions' is passed by rvalue reference, it is not safe to move
        // because 'userCreateNS' may throw a WriteConflictException.
        Status status = autoDb.getDb()->userCreateNS(opCtx, nss, collectionOptions, true, idIndex);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
}

/**
 * Creates the collection or the view as described by 'options'.
 */
Status createCollection(OperationContext* opCtx,
                        const NamespaceString& ns,
                        CollectionOptions&& options,
                        const BSONObj& idIndex) {
    auto status = userAllowedCreateNS(ns);
    if (!status.isOK()) {
        return status;
    }

    if (options.isView()) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create a view in a multi-document "
                                 "transaction.",
                !opCtx->inMultiDocumentTransaction());
        return _createView(opCtx, ns, std::move(options), idIndex);
    } else if (options.timeseries) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream()
                    << "Cannot create a time-series collection in a multi-document transaction.",
                !opCtx->inMultiDocumentTransaction());
        return _createTimeseries(opCtx, ns, std::move(options));
    } else {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create system collection " << ns
                              << " within a transaction.",
                !opCtx->inMultiDocumentTransaction() || !ns.isSystem());
        return _createCollection(opCtx, ns, std::move(options), idIndex);
    }
}

/**
 * Shared part of the implementation of the createCollection versions for replicated and regular
 * collection creation.
 */
Status createCollection(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex,
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
    }

    return createCollection(opCtx, nss, std::move(collectionOptions), idIndex);
}

}  // namespace

Status createCollection(OperationContext* opCtx,
                        const std::string& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex) {
    return createCollection(opCtx,
                            CommandHelpers::parseNsCollectionRequired(dbName, cmdObj),
                            cmdObj,
                            idIndex,
                            CollectionOptions::parseForCommand);
}

Status createCollection(OperationContext* opCtx,
                        const NamespaceString& ns,
                        const CreateCommand& cmd) {
    auto options = CollectionOptions::parse(cmd);
    auto idIndex = std::exchange(options.idIndex, {});
    return createCollection(opCtx, ns, std::move(options), idIndex);
}

Status createCollectionForApplyOps(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const OptionalCollectionUUID& ui,
                                   const BSONObj& cmdObj,
                                   const bool allowRenameOutOfTheWay,
                                   const BSONObj& idIndex) {
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
        auto uuid = ui.get();
        uassert(ErrorCodes::InvalidUUID,
                "Invalid UUID in applyOps create command: " + uuid.toString(),
                uuid.isRFC4122v4());

        auto& catalog = CollectionCatalog::get(opCtx);
        const auto currentName = catalog.lookupNSSByUUID(opCtx, uuid);
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
        auto futureColl = db
            ? CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, newCollName)
            : nullptr;
        bool needsRenaming = static_cast<bool>(futureColl);
        invariant(!needsRenaming || allowRenameOutOfTheWay);

        for (int tries = 0; needsRenaming && tries < 10; ++tries) {
            auto tmpNameResult = db->makeUniqueCollectionNamespace(opCtx, "tmp%%%%%.create");
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
                    futureColl = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, uuid);
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
        if (catalog.lookupCollectionByUUID(opCtx, uuid)) {
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

}  // namespace mongo
