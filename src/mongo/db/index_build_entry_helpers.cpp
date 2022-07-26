/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/platform/basic.h"

#include "mongo/db/index_build_entry_helpers.h"

#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeGettingIndexBuildEntry);

Status upsert(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry) {

    return writeConflictRetry(opCtx,
                              "upsertIndexBuildEntry",
                              NamespaceString::kIndexBuildEntryNamespace.ns(),
                              [&]() -> Status {
                                  AutoGetCollection collection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Helpers::upsert(opCtx,
                                                  NamespaceString::kIndexBuildEntryNamespace,
                                                  indexBuildEntry.toBSON(),
                                                  /*fromMigrate=*/false);
                                  wuow.commit();
                                  return Status::OK();
                              });
}

std::pair<const BSONObj, const BSONObj> buildIndexBuildEntryFilterAndUpdate(
    const IndexBuildEntry& indexBuildEntry) {
    // Construct the filter.
    const auto filter =
        BSON(IndexBuildEntry::kBuildUUIDFieldName << indexBuildEntry.getBuildUUID());

    // Construct the update.
    BSONObjBuilder updateMod;

    // If the update commit quorum is same as the value on-disk, we don't update it.
    if (indexBuildEntry.getCommitQuorum().isInitialized()) {
        BSONObjBuilder commitQuorumUpdate;
        indexBuildEntry.getCommitQuorum().appendToBuilder(IndexBuildEntry::kCommitQuorumFieldName,
                                                          &commitQuorumUpdate);
        updateMod.append("$set", commitQuorumUpdate.obj());
    }

    // '$addToSet' to prevent any duplicate entries written to "commitReadyMembers" field.
    if (auto commitReadyMembers = indexBuildEntry.getCommitReadyMembers()) {
        BSONArrayBuilder arrayBuilder;
        for (const auto& item : commitReadyMembers.get()) {
            arrayBuilder.append(item.toString());
        }
        const auto commitReadyMemberList = BSON(IndexBuildEntry::kCommitReadyMembersFieldName
                                                << BSON("$each" << arrayBuilder.arr()));
        updateMod.append("$addToSet", commitReadyMemberList);
    }

    return {filter, updateMod.obj()};
}

Status upsert(OperationContext* opCtx, const BSONObj& filter, const BSONObj& updateMod) {
    return writeConflictRetry(opCtx,
                              "upsertIndexBuildEntry",
                              NamespaceString::kIndexBuildEntryNamespace.ns(),
                              [&]() -> Status {
                                  AutoGetCollection collection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Helpers::upsert(opCtx,
                                                  NamespaceString::kIndexBuildEntryNamespace,
                                                  filter,
                                                  updateMod,
                                                  /*fromMigrate=*/false);
                                  wuow.commit();
                                  return Status::OK();
                              });
}

Status update(OperationContext* opCtx, const BSONObj& filter, const BSONObj& updateMod) {
    return writeConflictRetry(opCtx,
                              "updateIndexBuildEntry",
                              NamespaceString::kIndexBuildEntryNamespace.ns(),
                              [&]() -> Status {
                                  AutoGetCollection collection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Helpers::update(opCtx,
                                                  NamespaceString::kIndexBuildEntryNamespace,
                                                  filter,
                                                  updateMod,
                                                  /*fromMigrate=*/false);
                                  wuow.commit();
                                  return Status::OK();
                              });
}

}  // namespace

namespace indexbuildentryhelpers {

void ensureIndexBuildEntriesNamespaceExists(OperationContext* opCtx) {
    writeConflictRetry(
        opCtx,
        "createIndexBuildCollection",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() -> void {
            AutoGetDb autoDb(opCtx, NamespaceString::kIndexBuildEntryNamespace.dbName(), MODE_IX);
            auto db = autoDb.ensureDbExists(opCtx);

            // Ensure the database exists.
            invariant(db);

            // Create the collection if it doesn't exist.
            if (!CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                    opCtx, NamespaceString::kIndexBuildEntryNamespace)) {
                WriteUnitOfWork wuow(opCtx);
                AutoGetCollection autoColl(
                    opCtx, NamespaceString::kIndexBuildEntryNamespace, LockMode::MODE_IX);
                CollectionOptions defaultCollectionOptions;
                CollectionPtr collection = db->createCollection(
                    opCtx, NamespaceString::kIndexBuildEntryNamespace, defaultCollectionOptions);

                // Ensure the collection exists.
                invariant(collection);
                wuow.commit();
            }
        });
}

Status persistCommitReadyMemberInfo(OperationContext* opCtx,
                                    const IndexBuildEntry& indexBuildEntry) {
    invariant(indexBuildEntry.getCommitReadyMembers() &&
              !indexBuildEntry.getCommitQuorum().isInitialized());

    auto [filter, updateMod] = buildIndexBuildEntryFilterAndUpdate(indexBuildEntry);

    // Only update if the document still exists. We update instead of upsert so that we don't race
    // with the index build commit / abort that deletes the document; upserting after committing /
    // aborting would insert instead, and lead to an orphaned document.
    return update(opCtx, filter, updateMod);
}

Status persistIndexCommitQuorum(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry) {
    invariant(!indexBuildEntry.getCommitReadyMembers() &&
              indexBuildEntry.getCommitQuorum().isInitialized());

    auto [filter, updateMod] = buildIndexBuildEntryFilterAndUpdate(indexBuildEntry);
    return upsert(opCtx, filter, updateMod);
}

Status addIndexBuildEntry(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry) {
    return writeConflictRetry(
        opCtx,
        "addIndexBuildEntry",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() -> Status {
            AutoGetCollection collection(
                opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
            if (!collection) {
                str::stream ss;
                ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
                return Status(ErrorCodes::NamespaceNotFound, ss);
            }

            WriteUnitOfWork wuow(opCtx);

            // Reserve a slot in the oplog as the storage engine is allowed to insert oplog
            // documents out-of-order into the oplog.
            auto oplogInfo = LocalOplogInfo::get(opCtx);
            auto oplogSlot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
            Status status = collection->insertDocument(
                opCtx,
                InsertStatement(kUninitializedStmtId, indexBuildEntry.toBSON(), oplogSlot),
                nullptr);

            if (!status.isOK()) {
                return status;
            }
            wuow.commit();
            return Status::OK();
        });
}

Status removeIndexBuildEntry(OperationContext* opCtx,
                             const CollectionPtr& collection,
                             UUID indexBuildUUID) {
    return writeConflictRetry(
        opCtx,
        "removeIndexBuildEntry",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() -> Status {
            if (!collection) {
                str::stream ss;
                ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
                return Status(ErrorCodes::NamespaceNotFound, ss);
            }

            RecordId rid = Helpers::findById(opCtx, collection, BSON("_id" << indexBuildUUID));
            if (rid.isNull()) {
                str::stream ss;
                ss << "No matching IndexBuildEntry found with indexBuildUUID: " << indexBuildUUID;
                return Status(ErrorCodes::NoMatchingDocument, ss);
            }

            WriteUnitOfWork wuow(opCtx);
            OpDebug opDebug;
            collection->deleteDocument(opCtx, kUninitializedStmtId, rid, &opDebug);
            wuow.commit();
            return Status::OK();
        });
}

StatusWith<IndexBuildEntry> getIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID) {
    // Read the most up to date data.
    invariant(RecoveryUnit::ReadSource::kNoTimestamp ==
              opCtx->recoveryUnit()->getTimestampReadSource());
    AutoGetCollectionForRead collection(opCtx, NamespaceString::kIndexBuildEntryNamespace);

    // Must not be interruptible. This fail point is used to test the scenario where the index
    // build's OperationContext is interrupted by an abort, which will subsequently remove index
    // build entry from the config db collection.
    hangBeforeGettingIndexBuildEntry.pauseWhileSet(Interruptible::notInterruptible());

    if (!collection) {
        str::stream ss;
        ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
        return Status(ErrorCodes::NamespaceNotFound, ss);
    }

    BSONObj obj;
    // This operation does not perform any writes, but the index building code is sensitive to
    // exceptions and we must protect it from unanticipated write conflicts from reads.
    bool foundObj = writeConflictRetry(
        opCtx, "getIndexBuildEntry", NamespaceString::kIndexBuildEntryNamespace.ns(), [&]() {
            return Helpers::findOne(
                opCtx, collection.getCollection(), BSON("_id" << indexBuildUUID), obj);
        });

    if (!foundObj) {
        str::stream ss;
        ss << "No matching IndexBuildEntry found with indexBuildUUID: " << indexBuildUUID;
        return Status(ErrorCodes::NoMatchingDocument, ss);
    }

    try {
        IDLParserContext ctx("IndexBuildsEntry Parser");
        IndexBuildEntry indexBuildEntry = IndexBuildEntry::parse(ctx, obj);
        return indexBuildEntry;
    } catch (DBException& ex) {
        str::stream ss;
        ss << "Invalid BSON found for matching document with indexBuildUUID: " << indexBuildUUID;
        ss << ": " << obj;
        return ex.toStatus(ss);
    }
}

StatusWith<CommitQuorumOptions> getCommitQuorum(OperationContext* opCtx, UUID indexBuildUUID) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();
    return indexBuildEntry.getCommitQuorum();
}

Status setCommitQuorum_forTest(OperationContext* opCtx,
                               UUID indexBuildUUID,
                               CommitQuorumOptions commitQuorumOptions) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();
    indexBuildEntry.setCommitQuorum(commitQuorumOptions);
    return upsert(opCtx, indexBuildEntry);
}

}  // namespace indexbuildentryhelpers
}  // namespace mongo
