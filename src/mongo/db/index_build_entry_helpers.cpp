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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_build_entry_helpers.h"

#include <memory>
#include <string>
#include <vector>

#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog/database_impl.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_request.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace {

Status upsert(OperationContext* opCtx, IndexBuildEntry indexBuildEntry) {
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

}  // namespace

namespace indexbuildentryhelpers {

void ensureIndexBuildEntriesNamespaceExists(OperationContext* opCtx) {
    writeConflictRetry(
        opCtx,
        "createIndexBuildCollection",
        NamespaceString::kIndexBuildEntryNamespace.ns(),
        [&]() -> void {
            AutoGetOrCreateDb autoDb(
                opCtx, NamespaceString::kIndexBuildEntryNamespace.db(), MODE_X);
            Database* db = autoDb.getDb();

            // Ensure the database exists.
            invariant(db);

            // Create the collection if it doesn't exist.
            if (!db->getCollection(opCtx, NamespaceString::kIndexBuildEntryNamespace)) {
                WriteUnitOfWork wuow(opCtx);
                CollectionOptions options;
                Collection* collection = db->createCollection(
                    opCtx, NamespaceString::kIndexBuildEntryNamespace, options);

                // Ensure the collection exists.
                invariant(collection);
                wuow.commit();
            }
        });
}

Status addIndexBuildEntry(OperationContext* opCtx, IndexBuildEntry indexBuildEntry) {
    return writeConflictRetry(opCtx,
                              "addIndexBuildEntry",
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
                                  Status status = collection->insertDocument(
                                      opCtx, InsertStatement(indexBuildEntry.toBSON()), nullptr);
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

StatusWith<std::vector<IndexBuildEntry>> getIndexBuildEntries(OperationContext* opCtx,
                                                              UUID collectionUUID) {
    AutoGetCollectionForRead autoCollection(opCtx, NamespaceString::kIndexBuildEntryNamespace);
    Collection* collection = autoCollection.getCollection();
    if (!collection) {
        str::stream ss;
        ss << "Collection not found: " << NamespaceString::kIndexBuildEntryNamespace.ns();
        return Status(ErrorCodes::NamespaceNotFound, ss);
    }

    BSONObj collectionQuery = BSON("collectionUUID" << collectionUUID);
    std::vector<IndexBuildEntry> indexBuildEntries;

    auto qr = std::make_unique<QueryRequest>(collection->ns());
    qr->setFilter(collectionQuery);

    const ExtensionsCallbackReal extensionsCallback(opCtx, &collection->ns());
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto statusWithCQ =
        CanonicalQuery::canonicalize(opCtx,
                                     std::move(qr),
                                     expCtx,
                                     extensionsCallback,
                                     MatchExpressionParser::kAllowAllSpecialFeatures);

    if (!statusWithCQ.isOK()) {
        return statusWithCQ.getStatus();
    }

    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    auto statusWithExecutor = getExecutor(
        opCtx, collection, std::move(cq), PlanExecutor::NO_YIELD, QueryPlannerParams::DEFAULT);
    if (!statusWithExecutor.isOK()) {
        return statusWithExecutor.getStatus();
    }

    auto exec = std::move(statusWithExecutor.getValue());
    PlanExecutor::ExecState state;
    BSONObj obj;
    RecordId loc;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, &loc))) {
        try {
            IDLParserErrorContext ctx("IndexBuildsEntry Parser");
            IndexBuildEntry indexBuildEntry = IndexBuildEntry::parse(ctx, obj);
            indexBuildEntries.push_back(indexBuildEntry);
        } catch (...) {
            str::stream ss;
            ss << "Invalid BSON found for RecordId " << loc << " in collection "
               << collection->ns();
            return Status(ErrorCodes::InvalidBSON, ss);
        }
    }

    return indexBuildEntries;
}

StatusWith<CommitQuorumOptions> getCommitQuorum(OperationContext* opCtx, UUID indexBuildUUID) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();
    return indexBuildEntry.getCommitQuorum();
}

Status setCommitQuorum(OperationContext* opCtx,
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

Status addCommitReadyMember(OperationContext* opCtx, UUID indexBuildUUID, HostAndPort hostAndPort) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();

    std::vector<HostAndPort> newCommitReadyMembers;
    if (indexBuildEntry.getCommitReadyMembers()) {
        newCommitReadyMembers = indexBuildEntry.getCommitReadyMembers().get();
    }

    if (std::find(newCommitReadyMembers.begin(), newCommitReadyMembers.end(), hostAndPort) ==
        newCommitReadyMembers.end()) {
        newCommitReadyMembers.push_back(hostAndPort);
        indexBuildEntry.setCommitReadyMembers(newCommitReadyMembers);
        return upsert(opCtx, indexBuildEntry);
    }

    return Status::OK();
}

Status removeCommitReadyMember(OperationContext* opCtx,
                               UUID indexBuildUUID,
                               HostAndPort hostAndPort) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();

    std::vector<HostAndPort> newCommitReadyMembers;
    if (indexBuildEntry.getCommitReadyMembers()) {
        newCommitReadyMembers = indexBuildEntry.getCommitReadyMembers().get();
    }

    if (std::find(newCommitReadyMembers.begin(), newCommitReadyMembers.end(), hostAndPort) !=
        newCommitReadyMembers.end()) {
        newCommitReadyMembers.erase(
            std::remove(newCommitReadyMembers.begin(), newCommitReadyMembers.end(), hostAndPort));
        indexBuildEntry.setCommitReadyMembers(newCommitReadyMembers);
        return upsert(opCtx, indexBuildEntry);
    }

    return Status::OK();
}

StatusWith<std::vector<HostAndPort>> getCommitReadyMembers(OperationContext* opCtx,
                                                           UUID indexBuildUUID) {
    StatusWith<IndexBuildEntry> status = getIndexBuildEntry(opCtx, indexBuildUUID);
    if (!status.isOK()) {
        return status.getStatus();
    }

    IndexBuildEntry indexBuildEntry = status.getValue();
    if (indexBuildEntry.getCommitReadyMembers()) {
        return indexBuildEntry.getCommitReadyMembers().get();
    }

    return std::vector<HostAndPort>();
}

Status clearAllIndexBuildEntries(OperationContext* opCtx) {
    return writeConflictRetry(opCtx,
                              "truncateIndexBuildEntries",
                              NamespaceString::kIndexBuildEntryNamespace.ns(),
                              [&]() -> Status {
                                  AutoGetCollection autoCollection(
                                      opCtx, NamespaceString::kIndexBuildEntryNamespace, MODE_X);
                                  Collection* collection = autoCollection.getCollection();
                                  if (!collection) {
                                      str::stream ss;
                                      ss << "Collection not found: "
                                         << NamespaceString::kIndexBuildEntryNamespace.ns();
                                      return Status(ErrorCodes::NamespaceNotFound, ss);
                                  }

                                  WriteUnitOfWork wuow(opCtx);
                                  Status status = collection->truncate(opCtx);
                                  if (!status.isOK()) {
                                      return status;
                                  }
                                  wuow.commit();
                                  return Status::OK();
                              });
}

}  // namespace indexbuildentryhelpers
}  // namespace mongo
