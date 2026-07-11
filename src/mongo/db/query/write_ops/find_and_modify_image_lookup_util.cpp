// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"

#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/util/time_support.h"

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
                              << idl::serialize(oplogEntry.getOpType()));
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
    auto inTxn = oplogEntry.getCommitTransactionTimestamp().has_value();
    auto opTimestamp =
        inTxn ? *oplogEntry.getCommitTransactionTimestamp() : oplogEntry.getTimestamp();

    repl::ReadConcernArgs snapshotReadConcern(repl::ReadConcernLevel::kSnapshotReadConcern);

    auto atClusterTime = [&] {
        if (oplogEntry.getNeedsRetryImage() == repl::RetryImageEnum::kPostImage) {
            return opTimestamp;
        }

        uassert(12020800,
                str::stream() << "Failed to look up the pre-image document. Expected oplog "
                                 "timestamp increment to start at 1 but found "
                              << opTimestamp,
                opTimestamp.asULL() >= 1);

        // opTimestamp - 1 corresponds to the oplog slot reserved for the forged pre/post noop oplog
        // entry. No data write occurs at this timestamp, so reading at opTimestamp - 1 correctly
        // sees the pre-image before the data write at opTimestamp.
        return opTimestamp - 1;
    }();
    snapshotReadConcern.setArgsAtClusterTimeForSnapshot(atClusterTime);

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

    // The wall clock time for migrated oplog entries may not get overwritten on the recipient, and
    // currently replication lag is calculated based on the oplog wall clock time. For this reason,
    // set the forged noop oplog entry's wall clock time to the current time instead of the
    // findAndModify oplog entry's wall clock time.
    forgedNoop.setWallClockTime(Date_t::now());

    forgedNoop.setNss(oplogEntry.getNss());
    forgedNoop.setUuid(oplogEntry.getUuid());
    forgedNoop.setStatementIds(oplogEntry.getStatementIds());

    // The op time for migrated oplog entries do get overwritten on the recipient anyway when they
    // get written to the oplog. However, we set the forged noop oplog entry's op time to the
    // findAndModify oplog entry's op time - 1 for the following reasons.
    // 1. The oplog fetching in resharding uses the oplog timestamp as the _id for the documents in
    //   the oplog buffer collection and as the resume id. So the op time for each forged noop oplog
    //   entry must be valid and unique. Therefore:
    //   - When writing a retryable findAndModify oplog entry on the primary, we reserve an
    //     additional oplog slot right before it.
    //   - When forging pre/post-image noop oplog entry, we set its timestamp to that of the
    //     findAndModify's oplog entry minus 1.
    // 2. The oplog fetching in chunk migration does not have such requirements since there is no
    //    resumability. However, for consistency we still set the timestamp the same way.
    forgedNoop.setOpTime(repl::OpTime(oplogEntry.getTimestamp() - 1, *oplogEntry.getTerm()));

    return repl::OplogEntry{forgedNoop.toBSON()};
}

}  // namespace mongo
