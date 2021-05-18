
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/find_and_modify_result.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logger/redaction.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 * Validates that the request is retry-compatible with the operation that occurred.
 * In the case of nested oplog entry where the correct links are in the top level
 * oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
void validateFindAndModifyRetryability(const FindAndModifyRequest& request,
                                       const repl::OplogEntry& oplogEntry,
                                       const repl::OplogEntry& oplogWithCorrectLinks) {
    auto opType = oplogEntry.getOpType();
    auto ts = oplogEntry.getTimestamp();
    const auto needsRetryImage = oplogWithCorrectLinks.getNeedsRetryImage();

    if (opType == repl::OpTypeEnum::kDelete) {
        uassert(
            40606,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            request.isRemove());
        uassert(40607,
                str::stream() << "No pre-image available for findAndModify retry request:"
                              << redact(request.toBSON()),
                oplogWithCorrectLinks.getPreImageOpTime() || needsRetryImage);
    } else if (opType == repl::OpTypeEnum::kInsert) {
        uassert(
            40608,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            request.isUpsert());
    } else {
        uassert(
            40609,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType())
                          << ", oplogTs: "
                          << ts.toString()
                          << ", oplog: "
                          << redact(oplogEntry.toBSON()),
            opType == repl::OpTypeEnum::kUpdate);

        if (request.shouldReturnNew()) {
            uassert(40611,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document after update returned, but only before "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: "
                                  << redact(oplogEntry.toBSON()),
                    oplogWithCorrectLinks.getPostImageOpTime() || needsRetryImage);
        } else {
            uassert(40612,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document before update returned, but only after "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: "
                                  << redact(oplogEntry.toBSON()),
                    oplogWithCorrectLinks.getPreImageOpTime() || needsRetryImage);
        }
    }
}

/**
 * Extracts either the pre or post image (cannot be both) of the findAndModify operation from the
 * oplog.
 */
BSONObj extractPreOrPostImage(OperationContext* opCtx, const repl::OplogEntry& oplog) {
    invariant(oplog.getPreImageOpTime() || oplog.getPostImageOpTime() ||
              oplog.getNeedsRetryImage());
    DBDirectClient client(opCtx);
    if (oplog.getNeedsRetryImage()) {
        // Extract image from side collection.
        auto sessionIdBson = oplog.getSessionId()->toBSON();
        const auto txnNumber = *oplog.getTxnNumber();
        Timestamp ts = oplog.getTimestamp();
        const auto query = BSON("_id" << sessionIdBson);
        auto imageDoc = client.findOne(NamespaceString::kConfigImagesNamespace.ns(), query);
        if (serverGlobalParams.featureCompatibility.getVersion() <
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
            // We expect to have lost the transaction history since we drop the
            // 'config.image_collection' table on downgrade FCV.
            uassert(ErrorCodes::IncompleteTransactionHistory,
                    str::stream() << "Incomplete transaction history for sessionId: "
                                  << sessionIdBson
                                  << " txnNumber: "
                                  << txnNumber,
                    !imageDoc.isEmpty());
        } else {
            // Other than a real code bug, it is possible to be missing expected transaction
            // history if the server has gone through an FCV downgrade and then a subsequent
            // FCV upgrade before retrying.
            uassert(5637601,
                    str::stream()
                        << "image collection no longer contains the complete write history of this "
                           "transaction, record with sessionId "
                        << sessionIdBson.toString()
                        << " and txnNumber: "
                        << txnNumber
                        << " cannot be found",
                    !imageDoc.isEmpty());
        }

        auto entry =
            repl::ImageEntry::parse(IDLParserErrorContext("ImageEntryForRequest"), imageDoc);
        if (entry.getInvalidated()) {
            // This case is expected when a node could not correctly compute a retry image due
            // to data inconsistency while in initial sync.
            uasserted(ErrorCodes::IncompleteTransactionHistory,
                      str::stream() << "Incomplete transaction history for sessionId: "
                                    << sessionIdBson
                                    << " txnNumber: "
                                    << txnNumber);
        }

        if (entry.getTs() != oplog.getTimestamp() || entry.getTxnNumber() != oplog.getTxnNumber()) {
            // We found a corresponding image document, but the timestamp and transaction number
            // associated with the session record did not match the expected values from the oplog
            // entry.
            // Otherwise, it's unclear what went wrong.
            warning() << "Image lookup for a retryable findAndModify was unable to be verified with"
                         "sessionId "
                      << sessionIdBson << ", txnNumberRequested " << txnNumber
                      << ", timestampRequested " << ts << ", txnNumberFound "
                      << entry.getTxnNumber() << ", timestampFound " << entry.getTs();
            uasserted(
                5637602,
                str::stream()
                    << "image collection no longer contains the complete write history of this "
                       "transaction, record with sessionId: "
                    << sessionIdBson
                    << " cannot be found");
        }
        return imageDoc.getField(repl::ImageEntry::kImageFieldName).Obj().getOwned();
    }

    // Extract image from oplog.
    auto opTime = oplog.getPreImageOpTime() ? oplog.getPreImageOpTime().value()
                                            : oplog.getPostImageOpTime().value();

    auto oplogDoc = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                                   opTime.asQuery(),
                                   nullptr,
                                   QueryOption_OplogReplay);

    uassert(40613,
            str::stream() << "oplog no longer contains the complete write history of this "
                             "transaction, log with opTime "
                          << opTime.toString()
                          << " cannot be found",
            !oplogDoc.isEmpty());

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogDoc));
    return oplogEntry.getObject().getOwned();
}

/**
 * Extracts the findAndModify result by inspecting the oplog entries that where generated by a
 * previous execution of the command. In the case of nested oplog entry where the correct links
 * are in the top level oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
void parseOplogEntryForFindAndModify(OperationContext* opCtx,
                                     const FindAndModifyRequest& request,
                                     const repl::OplogEntry& oplogEntry,
                                     const repl::OplogEntry& oplogWithCorrectLinks,
                                     BSONObjBuilder* builder) {
    validateFindAndModifyRetryability(request, oplogEntry, oplogWithCorrectLinks);

    switch (oplogEntry.getOpType()) {
        case repl::OpTypeEnum::kInsert:
            return find_and_modify::serializeUpsert(
                1,
                request.shouldReturnNew() ? oplogEntry.getObject() : boost::optional<BSONObj>(),
                false,
                oplogEntry.getObject(),
                builder);
        case repl::OpTypeEnum::kUpdate:
            return find_and_modify::serializeUpsert(
                1, extractPreOrPostImage(opCtx, oplogWithCorrectLinks), true, {}, builder);
        case repl::OpTypeEnum::kDelete:
            return find_and_modify::serializeRemove(
                1, extractPreOrPostImage(opCtx, oplogWithCorrectLinks), builder);
        default:
            MONGO_UNREACHABLE;
    }
}

repl::OplogEntry getInnerNestedOplogEntry(const repl::OplogEntry& entry) {
    uassert(40635,
            str::stream() << "expected nested oplog entry with ts: "
                          << entry.getTimestamp().toString()
                          << " to have o2 field: "
                          << redact(entry.toBSON()),
            entry.getObject2());
    return uassertStatusOK(repl::OplogEntry::parse(*entry.getObject2()));
}

}  // namespace

SingleWriteResult parseOplogEntryForUpdate(const repl::OplogEntry& entry) {
    SingleWriteResult res;
    // Upserts are stored as inserts.
    if (entry.getOpType() == repl::OpTypeEnum::kInsert) {
        res.setN(1);
        res.setNModified(0);

        BSONObjBuilder upserted;
        upserted.append(entry.getObject()["_id"]);
        res.setUpsertedId(upserted.obj());
    } else if (entry.getOpType() == repl::OpTypeEnum::kUpdate) {
        res.setN(1);
        res.setNModified(1);
    } else if (entry.getOpType() == repl::OpTypeEnum::kNoop) {
        return parseOplogEntryForUpdate(getInnerNestedOplogEntry(entry));
    } else {
        uasserted(40638,
                  str::stream() << "update retry request is not compatible with previous write in "
                                   "the transaction of type: "
                                << OpType_serializer(entry.getOpType())
                                << ", oplogTs: "
                                << entry.getTimestamp().toString()
                                << ", oplog: "
                                << redact(entry.toBSON()));
    }

    return res;
}

void parseOplogEntryForFindAndModify(OperationContext* opCtx,
                                     const FindAndModifyRequest& request,
                                     const repl::OplogEntry& oplogEntry,
                                     BSONObjBuilder* builder) {
    // Migrated op case.
    if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
        parseOplogEntryForFindAndModify(
            opCtx, request, getInnerNestedOplogEntry(oplogEntry), oplogEntry, builder);
    } else {
        parseOplogEntryForFindAndModify(opCtx, request, oplogEntry, oplogEntry, builder);
    }
}

}  // namespace mongo
