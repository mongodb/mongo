/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/rss/replicated_storage_service.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace {

/**
 * Extracts the _id filter for the findAndModify operation with the given oplog entry.
 */
BSONObj extractFindAndModifyIdFilter(const repl::OplogEntry& oplogEntry) {
    auto idField = [&] {
        switch (oplogEntry.getOpType()) {
            case repl::OpTypeEnum::kUpdate:
                return oplogEntry.getObject2().get_value_or({})["_id"];
            case repl::OpTypeEnum::kDelete:
                return oplogEntry.getObject()["_id"];
            default:
                uasserted(11730900,
                          str::stream()
                              << "Found a findAndModify oplog entry with an unexpected op type "
                              << OpType_serializer(oplogEntry.getOpType()));
        }
    }();
    uassert(11730901,
            str::stream() << "Expected a findAndModify oplog entry to have an '_id' field "
                          << redact(oplogEntry.toBSONForLogging()),
            !idField.eoo());
    return idField.wrap();
}

/**
 * Fetches the pre- or post-image for the given findAndModify operation from the image collection.
 */
boost::optional<BSONObj> fetchPreOrPostImageFromImageCollection(
    const repl::OplogEntry oplogEntry, FindOneLocallyFunc findOneLocallyFunc) {
    auto imageDoc = findOneLocallyFunc(NamespaceString::kConfigImagesNamespace,
                                       BSON("_id" << oplogEntry.getSessionId()->toBSON()),
                                       boost::none /* readConcern */);

    if (!imageDoc) {
        return boost::none;
    }

    auto image = repl::ImageEntry::parse(*imageDoc, IDLParserContext("image entry"));
    if (image.getTxnNumber() != oplogEntry.getTxnNumber()) {
        // In our snapshot, fetch the current transaction number for a session. If that transaction
        // number doesn't match what's found on the image lookup, it implies that the image is not
        // the correct version for this oplog entry. We will not forge a noop from it.
        LOGV2_DEBUG(
            580603,
            2,
            "Not forging no-op image oplog entry because image document has a different txnNum",
            "sessionId"_attr = oplogEntry.getSessionId(),
            "expectedTxnNum"_attr = oplogEntry.getTxnNumber(),
            "actualTxnNum"_attr = image.getTxnNumber());
        return boost::none;
    }
    return image.getImage();
}

/**
 * Fetches the pre- or post-image for the given findAndModify operation.
 */
boost::optional<BSONObj> fetchPreOrPostImage(OperationContext* opCtx,
                                             const repl::OplogEntry& oplogEntry,
                                             FindOneLocallyFunc findOneLocallyFunc) {
    auto& rss = rss::ReplicatedStorageService::get(opCtx);
    if (rss.getPersistenceProvider().supportsFindAndModifyImageCollection()) {
        return fetchPreOrPostImageFromImageCollection(oplogEntry, findOneLocallyFunc);
    }
    return fetchPreOrPostImageFromSnapshot(oplogEntry, findOneLocallyFunc);
}

}  // namespace

boost::optional<BSONObj> fetchPreOrPostImageFromSnapshot(const repl::OplogEntry& oplogEntry,
                                                         FindOneLocallyFunc findOneLocallyFunc) {
    invariant(oplogEntry.getNeedsRetryImage());

    auto idFilter = extractFindAndModifyIdFilter(oplogEntry);
    auto opTimestamp = oplogEntry.getCommitTransactionTimestamp()
        ? *oplogEntry.getCommitTransactionTimestamp()
        : oplogEntry.getTimestamp();

    repl::ReadConcernArgs snapshotReadConcern(repl::ReadConcernLevel::kSnapshotReadConcern);
    snapshotReadConcern.setArgsAtClusterTimeForSnapshot(
        oplogEntry.getNeedsRetryImage() == repl::RetryImageEnum::kPostImage ? opTimestamp
                                                                            : opTimestamp - 1);

    try {
        auto doc = findOneLocallyFunc(oplogEntry.getNss(), idFilter, snapshotReadConcern);
        tassert(11730902,
                str::stream() << "Could not find the document that the findAndModify operation "
                                 "wrote to in the snapshot for "
                              << oplogEntry.getTimestamp(),
                doc);
        return doc;
    } catch (const ExceptionFor<ErrorCategory::SnapshotError>&) {
        return boost::none;
    }
}

boost::optional<repl::OplogEntry> forgeNoopImageOplogEntry(OperationContext* opCtx,
                                                           const repl::OplogEntry& oplogEntry,
                                                           FindOneLocallyFunc findOneLocallyFunc) {
    invariant(oplogEntry.getNeedsRetryImage());

    auto image = fetchPreOrPostImage(opCtx, oplogEntry, findOneLocallyFunc);

    if (!image) {
        return boost::none;
    }

    repl::MutableOplogEntry forgedNoop;
    forgedNoop.setSessionId(*oplogEntry.getSessionId());
    forgedNoop.setTxnNumber(*oplogEntry.getTxnNumber());
    forgedNoop.setObject(*image);
    forgedNoop.setOpType(repl::OpTypeEnum::kNoop);
    forgedNoop.setWallClockTime(oplogEntry.getWallClockTime());
    forgedNoop.setNss(oplogEntry.getNss());
    forgedNoop.setUuid(oplogEntry.getUuid());
    forgedNoop.setStatementIds(oplogEntry.getStatementIds());

    // Set the opTime to be the findAndModify timestamp - 1. We guarantee that there will be no
    // collisions because we always reserve an extra oplog slot when writing the retryable
    // findAndModify entry on the primary.
    forgedNoop.setOpTime(repl::OpTime(oplogEntry.getTimestamp() - 1, *oplogEntry.getTerm()));
    return repl::OplogEntry{forgedNoop.toBSON()};
}

}  // namespace mongo
