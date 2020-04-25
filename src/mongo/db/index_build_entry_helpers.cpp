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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_build_entry_helpers.h"

#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/local_oplog_info.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

Status upsert(OperationContext* opCtx, const IndexBuildEntry& indexBuildEntry) {

    return writeConflictRetry(opCtx,
                              "upsertIndexBuildEntry",
                              NamespaceString::kIndexBuildEntryNamespace.ns(),
                              [&]() -> Status {
                                  AutoGetCollection autoCollection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                                  Collection* collection = autoCollection.getCollection();
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Helpers::upsert(opCtx,
                                                  NamespaceString::kIndexBuildEntryNamespace.ns(),
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
                                  AutoGetCollection autoCollection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
                                  Collection* collection = autoCollection.getCollection();
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Helpers::upsert(opCtx,
                                                  NamespaceString::kIndexBuildEntryNamespace.ns(),
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
    writeConflictRetry(opCtx,
                       "createIndexBuildCollection",
                       NamespaceString::kIndexBuildEntryNamespace.ns(),
                       [&]() -> void {
                           AutoGetOrCreateDb autoDb(
                               opCtx, NamespaceString::kIndexBuildEntryNamespace.db(), MODE_X);
                           Database* db = autoDb.getDb();

                           // Ensure the database exists.
                           invariant(db);

                           // Create the collection if it doesn't exist.
                           if (!CollectionCatalog::get(opCtx).lookupCollectionByNamespace(
                                   opCtx, NamespaceString::kIndexBuildEntryNamespace)) {
                               WriteUnitOfWork wuow(opCtx);
                               CollectionOptions defaultCollectionOptions;
                               Collection* collection =
                                   db->createCollection(opCtx,
                                                        NamespaceString::kIndexBuildEntryNamespace,
                                                        defaultCollectionOptions);

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
    return upsert(opCtx, filter, updateMod);
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
            AutoGetCollection autoCollection(
                opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
            Collection* collection = autoCollection.getCollection();
            if (!collection) {
                str::stream ss;
                ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
                return Status(ErrorCodes::NamespaceNotFound, ss);
            }

            WriteUnitOfWork wuow(opCtx);

            Status status = Status::OK();
            if (supportsDocLocking()) {
                // Reserve a slot in the oplog. This must only be done for document level locking
                // storage engines, which are allowed to insert oplog documents out-of-order into
                // the oplog.
                auto oplogInfo = repl::LocalOplogInfo::get(opCtx);
                auto oplogSlot = oplogInfo->getNextOpTimes(opCtx, 1U)[0];
                status = collection->insertDocument(
                    opCtx,
                    InsertStatement(kUninitializedStmtId, indexBuildEntry.toBSON(), oplogSlot),
                    nullptr);
            } else {
                status = collection->insertDocument(
                    opCtx, InsertStatement(indexBuildEntry.toBSON()), nullptr);
            }
            if (!status.isOK()) {
                return status;
            }
            wuow.commit();
            return Status::OK();
        });
}

Status removeIndexBuildEntry(OperationContext* opCtx, UUID indexBuildUUID) {
    return writeConflictRetry(
        opCtx,
        "removeIndexBuildEntry",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() -> Status {
            AutoGetCollection autoCollection(
                opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_IX);
            Collection* collection = autoCollection.getCollection();
            if (!collection) {
                str::stream ss;
                ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
                return Status(ErrorCodes::NamespaceNotFound, ss);
            }

            RecordId rid = Helpers::findOne(
                opCtx, collection, BSON("_id" << indexBuildUUID), /*requireIndex=*/true);
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
    ReadSourceScope readSourceScope(opCtx, RecoveryUnit::ReadSource::kNoTimestamp);
    AutoGetCollectionForRead autoCollection(opCtx, NamespaceString::kIndexBuildEntryNamespace);
    Collection* collection = autoCollection.getCollection();
    if (!collection) {
        str::stream ss;
        ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
        return Status(ErrorCodes::NamespaceNotFound, ss);
    }

    BSONObj obj;
    bool foundObj = Helpers::findOne(
        opCtx, collection, BSON("_id" << indexBuildUUID), obj, /*requireIndex=*/true);
    if (!foundObj) {
        str::stream ss;
        ss << "No matching IndexBuildEntry found with indexBuildUUID: " << indexBuildUUID;
        return Status(ErrorCodes::NoMatchingDocument, ss);
    }

    try {
        IDLParserErrorContext ctx("IndexBuildsEntry Parser");
        IndexBuildEntry indexBuildEntry = IndexBuildEntry::parse(ctx, obj);
        return indexBuildEntry;
    } catch (...) {
        str::stream ss;
        ss << "Invalid BSON found for matching document with indexBuildUUID: " << indexBuildUUID;
        return Status(ErrorCodes::InvalidBSON, ss);
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
