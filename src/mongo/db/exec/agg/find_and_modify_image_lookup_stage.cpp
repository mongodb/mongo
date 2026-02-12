/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/exec/agg/find_and_modify_image_lookup_stage.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/query/write_ops/find_and_modify_image_lookup_util.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/scoped_read_concern.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

using OplogEntry = repl::OplogEntryBase;

MONGO_FAIL_POINT_DEFINE(failFindAndModifyImageLookupStageFindOneWithSnapshotTooOld);

/**
 * Perform a local findOne using the given read concern to find the document that matches in the
 * given filter in the given collection. Returns none if no document is found.
 */
boost::optional<BSONObj> findOneLocally(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                        const NamespaceString& nss,
                                        const BSONObj& filter,
                                        const boost::optional<repl::ReadConcernArgs>& readConcern) {
    if (MONGO_unlikely(failFindAndModifyImageLookupStageFindOneWithSnapshotTooOld.shouldFail(
            [&](const BSONObj& data) {
                return data.getStringField("nss") == nss.toString_forTest();
            }))) {
        tassert(11731903,
                "Expected the findOne to have readConcern 'snapshot'",
                readConcern->getLevel() == repl::ReadConcernLevelEnum::kSnapshotReadConcern);
        uasserted(ErrorCodes::SnapshotTooOld, "Failing findOne during findAndModify image lookup");
    }

    const bool isRawData = isRawDataOperation(pExpCtx->getOperationContext());
    ON_BLOCK_EXIT([&] { isRawDataOperation(pExpCtx->getOperationContext()) = isRawData; });
    if (gFeatureFlagAllBinariesSupportRawDataOperations.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(pExpCtx->getOperationContext()),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        // This must be set for the request to work against a timeseries collection.
        isRawDataOperation(pExpCtx->getOperationContext()) = true;
    }

    boost::optional<ScopedReadConcern> scopedReadConcern;
    if (readConcern) {
        scopedReadConcern.emplace(pExpCtx->getOperationContext(), *readConcern);

        auto status = mongo::waitForReadConcern(pExpCtx->getOperationContext(),
                                                *readConcern,
                                                nss.dbName(),
                                                true /* allowAfterClusterTime */);
        if (!status.isOK()) {
            LOGV2_WARNING(11731902,
                          "Failed to wait for read concern before doing a local find",
                          "nss"_attr = nss,
                          "readConcern"_attr = readConcern,
                          "status"_attr = status);
            return boost::none;
        }
    }

    auto doc = pExpCtx->getMongoProcessInterface()->lookupSingleDocumentLocally(
        pExpCtx, nss, Document{filter});
    return doc ? boost::make_optional(doc->toBson()) : boost::none;
}

}  // namespace

boost::intrusive_ptr<exec::agg::Stage> findAndModifyImageLookupStageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceFindAndModifyImageLookup) {
    auto* ptr = dynamic_cast<DocumentSourceFindAndModifyImageLookup*>(
        documentSourceFindAndModifyImageLookup.get());
    tassert(10980000, "expected 'DocumentSourceFindAndModifyImageLookup' type", ptr);
    return make_intrusive<exec::agg::FindAndModifyImageLookupStage>(
        ptr->kStageName, ptr->getExpCtx(), ptr->getIncludeCommitTransactionTimestamp());
}

namespace exec::agg {
REGISTER_AGG_STAGE_MAPPING(findAndModifyImageLookupStage,
                           DocumentSourceFindAndModifyImageLookup::id,
                           findAndModifyImageLookupStageToStageFn);

FindAndModifyImageLookupStage::FindAndModifyImageLookupStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    bool includeCommitTransactionTimestamp)
    : Stage(stageName, pExpCtx),
      _includeCommitTransactionTimestamp(includeCommitTransactionTimestamp) {}

GetNextResult FindAndModifyImageLookupStage::doGetNext() {
    uassert(5806001,
            str::stream() << DocumentSourceFindAndModifyImageLookup::kStageName
                          << " cannot be executed from router",
            !pExpCtx->getInRouter());
    if (_stashedDownconvertedDoc) {
        // Return the stashed downconverted document. This indicates that the previous document
        // returned was a forged noop image document.
        auto doc = *_stashedDownconvertedDoc;
        _stashedDownconvertedDoc = boost::none;
        return doc;
    }

    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    auto inputDoc = input.releaseDocument();
    return downConvertIfNeedsRetryImage(std::move(inputDoc));
}

Document FindAndModifyImageLookupStage::downConvertIfNeedsRetryImage(Document inputDoc) {
    // If '_includeCommitTransactionTimestamp' is true, strip any commit transaction timestamp field
    // from the inputDoc to avoid hitting an unknown field error when parsing the inputDoc into an
    // oplog entry below. Store the commit timestamp so it can be attached to the forged image doc
    // later, if there is one.
    const auto [inputOplogBson,
                commitTxnTs] = [&]() -> std::pair<BSONObj, boost::optional<Timestamp>> {
        if (!_includeCommitTransactionTimestamp) {
            return {inputDoc.toBson(), boost::none};
        }

        const auto commitTxnTs =
            inputDoc.getField(CommitTransactionOplogObject::kCommitTimestampFieldName);
        if (commitTxnTs.missing()) {
            return {inputDoc.toBson(), boost::none};
        }
        tassert(6387806,
                str::stream() << "'" << CommitTransactionOplogObject::kCommitTimestampFieldName
                              << "' field is not a BSON Timestamp",
                commitTxnTs.getType() == BSONType::timestamp);
        MutableDocument mutableInputDoc{inputDoc};
        mutableInputDoc.remove(CommitTransactionOplogObject::kCommitTimestampFieldName);
        return {mutableInputDoc.freeze().toBson(), commitTxnTs.getTimestamp()};
    }();

    const auto inputOplogEntry = uassertStatusOK(repl::OplogEntry::parse(inputOplogBson));
    const auto sessionId = inputOplogEntry.getSessionId();
    const auto txnNumber = inputOplogEntry.getTxnNumber();

    if (!sessionId || !txnNumber) {
        // This oplog entry cannot have a retry image.
        return inputDoc;
    }

    auto findOneLocallyFunc = [&](const NamespaceString& nss,
                                  const BSONObj& filter,
                                  const boost::optional<repl::ReadConcernArgs>& readConcern) {
        return findOneLocally(pExpCtx, nss, filter, readConcern);
    };

    if (inputOplogEntry.isCrudOpType() && inputOplogEntry.getNeedsRetryImage()) {
        // Strip the needsRetryImage field if set.
        MutableDocument downConvertedDoc{inputDoc};
        downConvertedDoc.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);

        if (const auto forgedNoopOplogEntry = forgeNoopImageOplogEntry(
                pExpCtx->getOperationContext(), inputOplogEntry, findOneLocallyFunc)) {
            const auto imageType = inputOplogEntry.getNeedsRetryImage();
            const auto imageOpTime = forgedNoopOplogEntry->getOpTime();
            downConvertedDoc.setField(
                imageType == repl::RetryImageEnum::kPreImage
                    ? repl::OplogEntry::kPreImageOpTimeFieldName
                    : repl::OplogEntry::kPostImageOpTimeFieldName,
                Value{Document{
                    {std::string{repl::OpTime::kTimestampFieldName}, imageOpTime.getTimestamp()},
                    {std::string{repl::OpTime::kTermFieldName}, imageOpTime.getTerm()}}});
            _stashedDownconvertedDoc = downConvertedDoc.freeze();
            return Document{forgedNoopOplogEntry->getEntry().toBSON()};
        }

        return downConvertedDoc.freeze();
    }

    if (inputOplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps &&
        isInternalSessionForRetryableWrite(*sessionId)) {
        // This is an applyOps oplog entry for a retryable internal transaction. Unpack its
        // operations to see if it has a retry image. Only one findAndModify operation is
        // allowed in a given retryable transaction.
        const auto applyOpsCmdObj = inputOplogEntry.getOperationToApply();
        const auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(applyOpsCmdObj);
        auto operationDocs = applyOpsInfo.getOperations();

        for (size_t i = 0; i < operationDocs.size(); i++) {
            auto op = repl::DurableReplOperation::parse(
                operationDocs[i],
                IDLParserContext{"FindAndModifyImageLookupStage::downConvertIfNeedsRetryImage"});

            const auto imageType = op.getNeedsRetryImage();
            if (!imageType) {
                continue;
            }

            auto mutableOp = uassertStatusOK(
                repl::MutableOplogEntry::parse(inputOplogEntry.getEntry().toBSON()));
            mutableOp.setMultiOpType(boost::none);
            mutableOp.setDurableReplOperation(op);
            auto findAndModifyOplogEntry = repl::OplogEntry(mutableOp.toBSON());

            const auto forgedNoopOplogEntry = [&]() -> boost::optional<repl::OplogEntry> {
                auto& rss = rss::ReplicatedStorageService::get(pExpCtx->getOperationContext());
                if (!rss.getPersistenceProvider().supportsFindAndModifyImageCollection()) {
                    // The commitTimestamp is only needed when fetching the image from the snapshot.
                    // Despite the name, 'commitTxnTs' is actually the timestamp of the last
                    // oplog entry in the oplog chain for the transaction.
                    // - For unprepared transaction, it is the timestamp for the terminal oplog
                    //   entry which is also the commit timestamp for the transaction.
                    // - For prepared transaction, it is the timestamp for the commitTransaction
                    //   oplog entry which is not the same as the commit timestamp for the
                    //   transaction.
                    auto lastOplogEntryDoc = findOneLocally(pExpCtx,
                                                            NamespaceString::kRsOplogNamespace,
                                                            BSON("ts" << *commitTxnTs),
                                                            boost::none /* readConcern */);

                    if (!lastOplogEntryDoc) {
                        // The commit oplog entry is no longer available.
                        return boost::none;
                    }

                    auto lastOplogEntry =
                        uassertStatusOK(repl::OplogEntry::parse(*lastOplogEntryDoc));
                    auto commitTimestamp =
                        uassertStatusOK(lastOplogEntry.extractCommitTransactionTimestamp());
                    findAndModifyOplogEntry.setCommitTransactionTimestamp(commitTimestamp);
                }
                return forgeNoopImageOplogEntry(
                    pExpCtx->getOperationContext(), findAndModifyOplogEntry, findOneLocallyFunc);
            }();

            // Downcovert the document for this applyOps oplog entry by downcoverting this
            // operation.
            op.setNeedsRetryImage(boost::none);
            if (forgedNoopOplogEntry) {
                if (imageType == repl::RetryImageEnum::kPreImage) {
                    op.setPreImageOpTime(forgedNoopOplogEntry->getOpTime());
                } else if (imageType == repl::RetryImageEnum::kPostImage) {
                    op.setPostImageOpTime(forgedNoopOplogEntry->getOpTime());
                } else {
                    MONGO_UNREACHABLE;
                }
            }

            operationDocs[i] = op.toBSON();

            const auto downCovertedApplyOpsCmdObj = applyOpsCmdObj.addFields(
                BSON(repl::ApplyOpsCommandInfo::kOperationsFieldName << operationDocs));

            MutableDocument downConvertedDoc(inputDoc);
            downConvertedDoc.setField(repl::OplogEntry::kObjectFieldName,
                                      Value{downCovertedApplyOpsCmdObj});

            if (!forgedNoopOplogEntry) {
                return downConvertedDoc.freeze();
            }

            _stashedDownconvertedDoc = downConvertedDoc.freeze();

            MutableDocument forgedNoopDoc{Document(forgedNoopOplogEntry->getEntry().toBSON())};
            if (commitTxnTs) {
                forgedNoopDoc.setField(CommitTransactionOplogObject::kCommitTimestampFieldName,
                                       Value{*commitTxnTs});
            }

            return forgedNoopDoc.freeze();
        }
    }

    return inputDoc;
}
}  // namespace exec::agg
}  // namespace mongo
