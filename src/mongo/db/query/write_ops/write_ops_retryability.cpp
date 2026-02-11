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


#include "mongo/db/query/write_ops/write_ops_retryability.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <mutex>
#include <utility>

#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Returns an error message that the pre- or post-image for the given findAndModify oplog entry
 * is no longer available. Expects the given oplog entry to have the 'needsRetryImage' field.
 */
std::string makePreOrPostImageNotFoundErrorMessage(const repl::OplogEntry& oplog) {
    return fmt::format(
        "Retrying a findAndModify operation with session id {} and txn number {} after it "
        "has been executed but the {} is no longer available",
        oplog.getSessionId()->toBSON().toString(),
        *oplog.getTxnNumber(),
        repl::RetryImage_serializer(*oplog.getNeedsRetryImage()));
}

/**
 * Returns an error message that the pre- or post-image for the given findAndModify oplog entry
 * and request is no longer available.
 */
std::string makePreOrPostImageNotFoundErrorMessage(
    const write_ops::FindAndModifyCommandRequest& request,
    const repl::OplogEntry& oplog,
    const repl::OplogEntry& oplogWithCorrectLinks,
    repl::RetryImageEnum imageType) {
    return fmt::format(
        "Retrying a findAndModify operation with session id {} and txn number {} after it "
        "has been executed but the {} is no longer available: ",
        oplogWithCorrectLinks.getSessionId()->toBSON().toString(),
        *oplogWithCorrectLinks.getTxnNumber(),
        repl::RetryImage_serializer(imageType),
        redact(request.toBSON()).toString());
}

/**
 * Validates that the request is retry-compatible with the operation that occurred.
 * In the case of nested oplog entry where the correct links are in the top level
 * oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
void validateFindAndModifyRetryability(const write_ops::FindAndModifyCommandRequest& request,
                                       const repl::OplogEntry& oplogEntry,
                                       const repl::OplogEntry& oplogWithCorrectLinks) {
    auto opType = oplogEntry.getOpType();
    auto ts = oplogEntry.getTimestamp();
    const auto needsRetryImage = oplogEntry.getNeedsRetryImage();

    uassert(11731000,
            "Expected oplog entry for a retryable findAndModify operation to have a session id",
            oplogWithCorrectLinks.getSessionId());
    uassert(11731001,
            "Expected oplog entry for a retryable findAndModify operation to have a txn number",
            oplogWithCorrectLinks.getTxnNumber());

    if (opType == repl::OpTypeEnum::kDelete) {
        uassert(
            40606,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType()) << ", oplogTs: "
                          << ts.toString() << ", oplog: " << redact(oplogEntry.toBSONForLogging()),
            request.getRemove().value_or(false));

        // Throw an error if the pre-image was not stored.
        uassert(ErrorCodes::IncompleteTransactionHistory,
                makePreOrPostImageNotFoundErrorMessage(
                    request, oplogEntry, oplogWithCorrectLinks, repl::RetryImageEnum::kPreImage),
                oplogWithCorrectLinks.getPreImageOpTime() ||
                    (needsRetryImage == repl::RetryImageEnum::kPreImage));
    } else if (opType == repl::OpTypeEnum::kInsert) {
        uassert(
            40608,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType()) << ", oplogTs: "
                          << ts.toString() << ", oplog: " << redact(oplogEntry.toBSONForLogging()),
            request.getUpsert().value_or(false));
    } else {
        uassert(
            40609,
            str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                          << " is not compatible with previous write in the transaction of type: "
                          << OpType_serializer(oplogEntry.getOpType()) << ", oplogTs: "
                          << ts.toString() << ", oplog: " << redact(oplogEntry.toBSONForLogging()),
            opType == repl::OpTypeEnum::kUpdate);

        if (request.getNew().value_or(false)) {
            // Throw an error if the pre-image was stored.
            uassert(40611,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document after update returned, but only before "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: " << redact(oplogEntry.toBSONForLogging()),
                    !oplogWithCorrectLinks.getPreImageOpTime() &&
                        (needsRetryImage != repl::RetryImageEnum::kPreImage));
            // Throw an error if the post-image was not stored.
            uassert(
                ErrorCodes::IncompleteTransactionHistory,
                makePreOrPostImageNotFoundErrorMessage(
                    request, oplogEntry, oplogWithCorrectLinks, repl::RetryImageEnum::kPostImage),
                oplogWithCorrectLinks.getPostImageOpTime() ||
                    (needsRetryImage == repl::RetryImageEnum::kPostImage));
        } else {
            // Throw an error if the post-image was stored.
            uassert(40612,
                    str::stream() << "findAndModify retry request: " << redact(request.toBSON())
                                  << " wants the document before update returned, but only after "
                                     "update document is stored, oplogTs: "
                                  << ts.toString()
                                  << ", oplog: " << redact(oplogEntry.toBSONForLogging()),
                    !oplogWithCorrectLinks.getPostImageOpTime() &&
                        (needsRetryImage != repl::RetryImageEnum::kPostImage));
            // Throw an error if the pre-image was not stored.
            uassert(
                ErrorCodes::IncompleteTransactionHistory,
                makePreOrPostImageNotFoundErrorMessage(
                    request, oplogEntry, oplogWithCorrectLinks, repl::RetryImageEnum::kPreImage),
                oplogWithCorrectLinks.getPreImageOpTime() ||
                    (needsRetryImage == repl::RetryImageEnum::kPreImage));
        }
    }
}

/**
 * Fetches the pre- or post-image for the given findAndModify operation from the image collection.
 */
BSONObj fetchPreOrPostImageFromImageCollection(OperationContext* opCtx,
                                               const repl::OplogEntry& oplog) {
    invariant(oplog.getNeedsRetryImage());

    LogicalSessionId sessionId = oplog.getSessionId().value();
    TxnNumber txnNumber = oplog.getTxnNumber().value();
    Timestamp ts = oplog.getTimestamp();

    auto curOp = CurOp::get(opCtx);
    const auto existingNS = curOp->getNSS();

    DBDirectClient client(opCtx);
    BSONObj imageDoc =
        client.findOne(NamespaceString::kConfigImagesNamespace, BSON("_id" << sessionId.toBSON()));

    {
        stdx::lock_guard<Client> clientLock(*opCtx->getClient());
        curOp->setNS(clientLock, existingNS);
    }

    if (imageDoc.isEmpty()) {
        LOGV2_WARNING(5676402,
                      "Image lookup for a retryable findAndModify was not found",
                      "sessionId"_attr = sessionId,
                      "txnNumber"_attr = txnNumber,
                      "timestamp"_attr = ts);
        uasserted(5637601,
                  str::stream()
                      << "image collection no longer contains the complete write history of this "
                         "transaction, record with sessionId: "
                      << sessionId.toBSON() << " cannot be found");
    }

    auto entry = repl::ImageEntry::parse(imageDoc, IDLParserContext("ImageEntryForRequest"));
    if (entry.getInvalidated()) {
        // This case is expected when a node could not correctly compute a retry image due
        // to data inconsistency while in initial sync.
        uasserted(ErrorCodes::IncompleteTransactionHistory,
                  str::stream() << "Incomplete transaction history for sessionId: "
                                << sessionId.toBSON() << " txnNumber: " << txnNumber);
    }

    if (entry.getTs() != oplog.getTimestamp() || entry.getTxnNumber() != oplog.getTxnNumber()) {
        // We found a corresponding image document, but the timestamp and transaction number
        // associated with the session record did not match the expected values from the oplog
        // entry.

        // Otherwise, it's unclear what went wrong.
        LOGV2_WARNING(5676403,
                      "Image lookup for a retryable findAndModify was unable to be verified",
                      "sessionId"_attr = sessionId,
                      "txnNumberRequested"_attr = txnNumber,
                      "timestampRequested"_attr = ts,
                      "txnNumberFound"_attr = entry.getTxnNumber(),
                      "timestampFound"_attr = entry.getTs());
        uasserted(5637602,
                  str::stream()
                      << "image collection no longer contains the complete write history of this "
                         "transaction, record with sessionId: "
                      << sessionId.toBSON() << " cannot be found");
    }

    return entry.getImage();
}

/**
 * Fetches the pre- or post-image for the given findAndModify operation from the oplog.
 */
BSONObj fetchPreOrPostImageFromOplog(OperationContext* opCtx, const repl::OplogEntry& oplog) {
    invariant(oplog.getPreImageOpTime() || oplog.getPostImageOpTime());

    auto opTime = oplog.getPreImageOpTime() ? oplog.getPreImageOpTime().value()
                                            : oplog.getPostImageOpTime().value();

    DBDirectClient client(opCtx);
    auto oplogDoc = client.findOne(NamespaceString::kRsOplogNamespace, opTime.asQuery());

    uassert(ErrorCodes::IncompleteTransactionHistory,
            str::stream() << "The oplog no longer contains the "
                          << (oplog.getPreImageOpTime() ? "pre-image" : "post-image")
                          << " for this findAndModify "
                             "operation, The oplog entry with opTime "
                          << opTime.toString() << " cannot be found",
            !oplogDoc.isEmpty());

    auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogDoc));
    return oplogEntry.getObject().getOwned();
}

/**
 * Fetches the pre- or post-image for the given findAndModify operation.
 */
BSONObj extractPreOrPostImage(OperationContext* opCtx, const repl::OplogEntry& oplog) {
    tassert(11052024,
            "Expected OplogEntry with pre or post image",
            oplog.getPreImageOpTime() || oplog.getPostImageOpTime() || oplog.getNeedsRetryImage());
    if (oplog.getNeedsRetryImage()) {
        auto& rss = rss::ReplicatedStorageService::get(opCtx);
        if (rss.getPersistenceProvider().supportsFindAndModifyImageCollection()) {
            return fetchPreOrPostImageFromImageCollection(opCtx, oplog);
        }

        auto image = fetchPreOrPostImageFromSnapshot(opCtx, oplog);
        uassert(ErrorCodes::IncompleteTransactionHistory,
                makePreOrPostImageNotFoundErrorMessage(oplog),
                image);
        return *image;
    }
    return fetchPreOrPostImageFromOplog(opCtx, oplog);
}

/**
 * Extracts the findAndModify result by inspecting the oplog entries that were generated by a
 * previous execution of the command. In the case of nested oplog entry where the correct links
 * are in the top level oplog, oplogWithCorrectLinks can be used to specify the outer oplog.
 */
write_ops::FindAndModifyCommandReply parseOplogEntryForFindAndModify(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& request,
    const repl::OplogEntry& oplogEntry,
    const repl::OplogEntry& oplogWithCorrectLinks) {
    validateFindAndModifyRetryability(request, oplogEntry, oplogWithCorrectLinks);

    write_ops::FindAndModifyCommandReply result;
    write_ops::FindAndModifyLastError lastError;
    lastError.setNumDocs(1);

    switch (oplogEntry.getOpType()) {
        case repl::OpTypeEnum::kDelete:
            result.setValue(extractPreOrPostImage(opCtx, oplogWithCorrectLinks));
            break;
        case repl::OpTypeEnum::kUpdate:
            lastError.setUpdatedExisting(true);
            result.setValue(extractPreOrPostImage(opCtx, oplogWithCorrectLinks));
            break;
        case repl::OpTypeEnum::kInsert: {
            lastError.setUpdatedExisting(false);

            auto owned = oplogEntry.getObject().getOwned();
            auto id = owned.getField("_id");
            if (id) {
                lastError.setUpserted(IDLAnyTypeOwned(id, owned));
            }

            if (request.getNew().value_or(false)) {
                result.setValue(oplogEntry.getObject().getOwned());
            }
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
    result.setLastErrorObject(std::move(lastError));
    return result;
}

repl::OplogEntry getInnerNestedOplogEntry(const repl::OplogEntry& entry) {
    uassert(40635,
            str::stream() << "expected nested oplog entry with ts: "
                          << entry.getTimestamp().toString()
                          << " to have o2 field: " << redact(entry.toBSONForLogging()),
            entry.getObject2());
    return uassertStatusOK(repl::OplogEntry::parse(*entry.getObject2()));
}

}  // namespace

static constexpr StringData kWouldChangeOwningShardRetryContext =
    "Operation was converted into a distributed transaction because the modified document would "
    "move shards and succeeded but transaction history was not generated so the original reply "
    "cannot be recreated."_sd;

SingleWriteResult parseOplogEntryForUpdate(const repl::OplogEntry& entry,
                                           const boost::optional<BSONElement>& upsertedId) {
    SingleWriteResult res;
    // Upserts are stored as inserts.
    if (entry.getOpType() == repl::OpTypeEnum::kInsert) {
        res.setN(1);
        res.setNModified(0);

        BSONObjBuilder upserted;
        upserted.append(upsertedId ? *upsertedId : entry.getObject()["_id"]);
        res.setUpsertedId(upserted.obj());
    } else if (entry.getOpType() == repl::OpTypeEnum::kUpdate ||
               entry.getOpType() == repl::OpTypeEnum::kDelete) {
        // Time-series updates could generate an oplog of type "kDelete". It also implies one
        // user-level measurement is modified.
        res.setN(1);
        res.setNModified(1);
    } else if (entry.getOpType() == repl::OpTypeEnum::kNoop) {
        if (isWouldChangeOwningShardSentinelOplogEntry(entry)) {
            uasserted(ErrorCodes::IncompleteTransactionHistory,
                      kWouldChangeOwningShardRetryContext);
        }
        return parseOplogEntryForUpdate(getInnerNestedOplogEntry(entry));
    } else {
        uasserted(40638,
                  str::stream() << "update retry request is not compatible with previous write in "
                                   "the transaction of type: "
                                << OpType_serializer(entry.getOpType())
                                << ", oplogTs: " << entry.getTimestamp().toString()
                                << ", oplog: " << redact(entry.toBSONForLogging()));
    }

    return res;
}

write_ops::FindAndModifyCommandReply parseOplogEntryForFindAndModify(
    OperationContext* opCtx,
    const write_ops::FindAndModifyCommandRequest& request,
    const repl::OplogEntry& oplogEntry) {

    // Migrated op and WouldChangeOwningShard sentinel case.
    if (oplogEntry.getOpType() == repl::OpTypeEnum::kNoop) {
        if (isWouldChangeOwningShardSentinelOplogEntry(oplogEntry)) {
            uasserted(ErrorCodes::IncompleteTransactionHistory,
                      kWouldChangeOwningShardRetryContext);
        }

        return parseOplogEntryForFindAndModify(
            opCtx, request, getInnerNestedOplogEntry(oplogEntry), oplogEntry);
    }

    return parseOplogEntryForFindAndModify(opCtx, request, oplogEntry, oplogEntry);
}

}  // namespace mongo
