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
#include "mongo/db/command_generic_argument.h"
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
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

Status _createView(OperationContext* opCtx,
                   const NamespaceString& nss,
                   const CollectionOptions& collectionOptions,
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
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        // Create 'system.views' in a separate WUOW if it does not exist.
        WriteUnitOfWork wuow(opCtx);
        Collection* coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(
            opCtx, NamespaceString(db->getSystemViewsName()));
        if (!coll) {
            coll = db->createCollection(opCtx, NamespaceString(db->getSystemViewsName()));
        }
        invariant(coll);
        wuow.commit();

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      db->getProfilingLevel());

        // If the view creation rolls back, ensure that the Top entry created for the view is
        // deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

        Status status = db->userCreateNS(opCtx, nss, collectionOptions, true, idIndex);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
}

Status _createCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const CollectionOptions& collectionOptions,
                         const BSONObj& idIndex) {
    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        // This is a top-level handler for collection creation name conflicts. New commands coming
        // in, or commands that generated a WriteConflict must return a NamespaceExists error here
        // on conflict.
        if (CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, nss) != nullptr) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "Collection already exists. NS: " << nss);
        }
        if (ViewCatalog::get(autoDb.getDb())->lookup(opCtx, nss.ns())) {
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "A view already exists. NS: " << nss);
        }

        if (opCtx->writesAreReplicated() &&
            !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while creating collection " << nss);
        }

        WriteUnitOfWork wunit(opCtx);

        AutoStatsTracker statsTracker(opCtx,
                                      nss,
                                      Top::LockType::NotLocked,
                                      AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                      autoDb.getDb()->getProfilingLevel());

        // If the collection creation rolls back, ensure that the Top entry created for the
        // collection is deleted.
        opCtx->recoveryUnit()->onRollback([nss, serviceContext = opCtx->getServiceContext()]() {
            Top::get(serviceContext).collectionDropped(nss);
        });

        Status status = autoDb.getDb()->userCreateNS(opCtx, nss, collectionOptions, true, idIndex);
        if (!status.isOK()) {
            return status;
        }
        wunit.commit();

        return Status::OK();
    });
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

    Status status = userAllowedCreateNS(nss.db(), nss.coll());
    if (!status.isOK()) {
        return status;
    }

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

    if (collectionOptions.isView()) {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create a view in a multi-document "
                                 "transaction.",
                !opCtx->inMultiDocumentTransaction());
        return _createView(opCtx, nss, collectionOptions, idIndex);
    } else {
        uassert(ErrorCodes::OperationNotSupportedInTransaction,
                str::stream() << "Cannot create system collection " << nss.toString()
                              << " within a transaction.",
                !opCtx->inMultiDocumentTransaction() || !nss.isSystem());
        return _createCollection(opCtx, nss, collectionOptions, idIndex);
    }
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
    // create a database on MMAPv1, which could result in createCollection failing if the database
    // does not yet exist.
    if (ui) {
        // Return an optional, indicating whether we need to early return (if the collection already
        // exists, or in case of an error).
        using Result = boost::optional<Status>;
        auto result =
            writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                auto uuid = ui.get();
                uassert(ErrorCodes::InvalidUUID,
                        "Invalid UUID in applyOps create command: " + uuid.toString(),
                        uuid.isRFC4122v4());

                auto& catalog = CollectionCatalog::get(opCtx);
                const auto currentName = catalog.lookupNSSByUUID(opCtx, uuid);
                auto serviceContext = opCtx->getServiceContext();
                auto opObserver = serviceContext->getOpObserver();
                if (currentName && *currentName == newCollName)
                    return Result(Status::OK());

                if (currentName && currentName->isDropPendingNamespace()) {
                    LOGV2(20308,
                          "CMD: create {newCollName} - existing collection with conflicting UUID "
                          "{uuid} is in a drop-pending state: {currentName}",
                          "CMD: create -- existing collection with conflicting UUID "
                          "is in a drop-pending state",
                          "newCollection"_attr = newCollName,
                          "conflictingUuid"_attr = uuid,
                          "existingCollection"_attr = *currentName);
                    return Result(Status(ErrorCodes::NamespaceExists,
                                         str::stream()
                                             << "existing collection " << currentName->toString()
                                             << " with conflicting UUID " << uuid.toString()
                                             << " is in a drop-pending state."));
                }

                // In the case of oplog replay, a future command may have created or renamed a
                // collection with that same name. In that case, renaming this future collection to
                // a random temporary name is correct: once all entries are replayed no temporary
                // names will remain.  On MMAPv1 the rename can result in index names that are too
                // long. However this should only happen for initial sync and "resync collection"
                // for rollback, so we can let the error propagate resulting in an abort and restart
                // of the initial sync or result in rollback to fassert, requiring a resync of that
                // node.
                const bool stayTemp = true;
                auto futureColl = db
                    ? CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, newCollName)
                    : nullptr;
                bool needsRenaming = static_cast<bool>(futureColl);
                invariant(!needsRenaming || allowRenameOutOfTheWay);

                for (int tries = 0; needsRenaming && tries < 10; ++tries) {
                    auto tmpNameResult =
                        db->makeUniqueCollectionNamespace(opCtx, "tmp%%%%%.create");
                    if (!tmpNameResult.isOK()) {
                        return Result(tmpNameResult.getStatus().withContext(
                            str::stream() << "Cannot generate temporary "
                                             "collection namespace for applyOps "
                                             "create command: collection: "
                                          << newCollName));
                    }

                    const auto& tmpName = tmpNameResult.getValue();
                    AutoGetCollection tmpCollLock(opCtx, tmpName, LockMode::MODE_X);
                    if (tmpCollLock.getCollection()) {
                        // Conflicting on generating a unique temp collection name. Try again.
                        continue;
                    }

                    // It is ok to log this because this doesn't happen very frequently.
                    LOGV2(20309,
                          "CMD: create {newCollName} - renaming existing collection with "
                          "conflicting UUID {uuid} to temporary collection {tmpName}",
                          "CMD: create -- renaming existing collection with "
                          "conflicting UUID to temporary collection",
                          "newCollection"_attr = newCollName,
                          "conflictingUuid"_attr = uuid,
                          "tempName"_attr = tmpName);
                    Status status = db->renameCollection(opCtx, newCollName, tmpName, stayTemp);
                    if (!status.isOK())
                        return Result(status);
                    opObserver->onRenameCollection(opCtx,
                                                   newCollName,
                                                   tmpName,
                                                   futureColl->uuid(),
                                                   /*dropTargetUUID*/ {},
                                                   /*numRecords*/ 0U,
                                                   stayTemp);

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
                    return Result(Status(ErrorCodes::NamespaceExists,
                                         str::stream() << "Cannot generate temporary "
                                                          "collection namespace for applyOps "
                                                          "create command: collection: "
                                                       << newCollName));
                }

                // If the collection with the requested UUID already exists, but with a different
                // name, just rename it to 'newCollName'.
                if (catalog.lookupCollectionByUUID(opCtx, uuid)) {
                    invariant(currentName);
                    uassert(40655,
                            str::stream() << "Invalid name " << newCollName << " for UUID " << uuid,
                            currentName->db() == newCollName.db());
                    Status status =
                        db->renameCollection(opCtx, *currentName, newCollName, stayTemp);
                    if (!status.isOK())
                        return Result(status);
                    opObserver->onRenameCollection(opCtx,
                                                   *currentName,
                                                   newCollName,
                                                   uuid,
                                                   /*dropTargetUUID*/ {},
                                                   /*numRecords*/ 0U,
                                                   stayTemp);

                    wunit.commit();
                    return Result(Status::OK());
                }

                // A new collection with the specific UUID must be created, so add the UUID to the
                // creation options. Regular user collection creation commands cannot do this.
                auto uuidObj = uuid.toBSON();
                newCmd = cmdObj.addField(uuidObj.firstElement());
                wunit.commit();

                return Result(boost::none);
            });

        if (result) {
            return *result;
        }
    }

    return createCollection(
        opCtx, newCollName, newCmd, idIndex, CollectionOptions::parseForStorage);
}

}  // namespace mongo
