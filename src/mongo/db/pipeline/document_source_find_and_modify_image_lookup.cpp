/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

/**
 * Fetches the pre- or post-image entry for the given 'findAndModify' oplog entry or for the given
 * inner op in the given 'applyOps' oplog entry from the findAndModify image collection, and returns
 * a forged noop oplog entry containing the image. Returns none if no matching image entry is not
 * found.
 */
boost::optional<repl::OplogEntry> forgeNoopImageOplogEntry(
    OperationContext* opCtx,
    const boost::intrusive_ptr<ExpressionContext> pExpCtx,
    const repl::OplogEntry oplogEntry,
    boost::optional<repl::DurableReplOperation> innerOp = boost::none) {
    invariant(!innerOp ||
              (oplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps));
    const auto sessionId = *oplogEntry.getSessionId();

    auto localImageCollInfo = pExpCtx->mongoProcessInterface->getCollectionOptions(
        pExpCtx->opCtx, NamespaceString::kConfigImagesNamespace);

    // Extract the UUID from the collection information. We should always have a valid uuid here.
    auto imageCollUUID = invariantStatusOK(UUID::parse(localImageCollInfo["uuid"]));
    const auto& readConcernBson = repl::ReadConcernArgs::get(opCtx).toBSON();
    auto imageDoc = pExpCtx->mongoProcessInterface->lookupSingleDocument(
        pExpCtx,
        NamespaceString::kConfigImagesNamespace,
        imageCollUUID,
        Document{BSON("_id" << sessionId.toBSON())},
        std::move(readConcernBson));

    if (!imageDoc) {
        // If no image document with the corresponding 'sessionId' is found, we skip forging the
        // no-op and rely on the retryable write mechanism to catch that no pre- or post- image
        // exists.
        LOGV2_DEBUG(580602,
                    2,
                    "Not forging no-op image oplog entry because no image document found with "
                    "sessionId",
                    "sessionId"_attr = sessionId);
        return boost::none;
    }

    auto image = repl::ImageEntry::parse(IDLParserContext("image entry"), imageDoc->toBson());

    if (image.getTxnNumber() != oplogEntry.getTxnNumber()) {
        // In our snapshot, fetch the current transaction number for a session. If that
        // transaction number doesn't match what's found on the image lookup, it implies that
        // the image is not the correct version for this oplog entry. We will not forge a noop
        // from it.
        LOGV2_DEBUG(
            580603,
            2,
            "Not forging no-op image oplog entry because image document has a different txnNum",
            "sessionId"_attr = oplogEntry.getSessionId(),
            "expectedTxnNum"_attr = oplogEntry.getTxnNumber(),
            "actualTxnNum"_attr = image.getTxnNumber());
        return boost::none;
    }

    // Forge a no-op image entry to be returned.
    repl::MutableOplogEntry forgedNoop;
    forgedNoop.setSessionId(sessionId);
    forgedNoop.setTxnNumber(*oplogEntry.getTxnNumber());
    forgedNoop.setObject(image.getImage());
    forgedNoop.setOpType(repl::OpTypeEnum::kNoop);
    forgedNoop.setWallClockTime(oplogEntry.getWallClockTime());
    forgedNoop.setNss(innerOp ? innerOp->getNss() : oplogEntry.getNss());
    forgedNoop.setUuid(innerOp ? innerOp->getUuid() : *oplogEntry.getUuid());
    forgedNoop.setStatementIds(
        innerOp ? repl::variant_util::toVector<StmtId>(innerOp->getStatementIds())
                : oplogEntry.getStatementIds());

    // Set the opTime to be the findAndModify timestamp - 1. We guarantee that there will be no
    // collisions because we always reserve an extra oplog slot when writing the retryable
    // findAndModify entry on the primary.
    forgedNoop.setOpTime(repl::OpTime(oplogEntry.getTimestamp() - 1, *oplogEntry.getTerm()));
    return repl::OplogEntry{forgedNoop.toBSON()};
}

}  // namespace

using OplogEntry = repl::OplogEntryBase;

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalFindAndModifyImageLookup,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceFindAndModifyImageLookup::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTimestamp) {
    return new DocumentSourceFindAndModifyImageLookup(expCtx, includeCommitTimestamp);
}

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5806003,
            str::stream() << "the '" << kStageName << "' spec must be an object",
            elem.type() == BSONType::Object);

    bool includeCommitTimestamp = false;
    for (auto&& subElem : elem.Obj()) {
        if (subElem.fieldNameStringData() == kIncludeCommitTransactionTimestampFieldName) {
            uassert(6387805,
                    str::stream() << "expected a boolean for the "
                                  << kIncludeCommitTransactionTimestampFieldName << " option to "
                                  << kStageName << " stage, got " << typeName(subElem.type()),
                    subElem.type() == Bool);
            includeCommitTimestamp = subElem.Bool();
        } else {
            uasserted(6387800,
                      str::stream() << "unrecognized option to " << kStageName
                                    << " stage: " << subElem.fieldNameStringData());
        }
    }

    return DocumentSourceFindAndModifyImageLookup::create(expCtx, includeCommitTimestamp);
}

DocumentSourceFindAndModifyImageLookup::DocumentSourceFindAndModifyImageLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool includeCommitTimestamp)
    : DocumentSource(kStageName, expCtx),
      _includeCommitTransactionTimestamp(includeCommitTimestamp) {}

StageConstraints DocumentSourceFindAndModifyImageLookup::constraints(
    Pipeline::SplitState pipeState) const {
    return StageConstraints(StreamType::kStreaming,
                            PositionRequirement::kNone,
                            HostTypeRequirement::kAnyShard,
                            DiskUseRequirement::kNoDiskUse,
                            FacetRequirement::kNotAllowed,
                            TransactionRequirement::kNotAllowed,
                            LookupRequirement::kNotAllowed,
                            UnionRequirement::kNotAllowed,
                            ChangeStreamRequirement::kDenylist);
}

Value DocumentSourceFindAndModifyImageLookup::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(
        Document{{kStageName,
                  Value(Document{{kIncludeCommitTransactionTimestampFieldName,
                                  _includeCommitTransactionTimestamp ? Value(true) : Value()}})}});
}

DepsTracker::State DocumentSourceFindAndModifyImageLookup::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(OplogEntry::kSessionIdFieldName.toString());
    deps->fields.insert(OplogEntry::kTxnNumberFieldName.toString());
    deps->fields.insert(OplogEntry::kNeedsRetryImageFieldName.toString());
    deps->fields.insert(OplogEntry::kWallClockTimeFieldName.toString());
    deps->fields.insert(OplogEntry::kNssFieldName.toString());
    deps->fields.insert(OplogEntry::kTimestampFieldName.toString());
    deps->fields.insert(OplogEntry::kTermFieldName.toString());
    deps->fields.insert(OplogEntry::kUuidFieldName.toString());
    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceFindAndModifyImageLookup::getModifiedPaths() const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceFindAndModifyImageLookup::doGetNext() {
    uassert(5806001,
            str::stream() << kStageName << " cannot be executed from mongos",
            !pExpCtx->inMongos);
    if (_stashedFindAndModifyDoc) {
        // Return the stashed findAndModify document. This indicates that the previous document
        // returned was a forged noop image document.
        auto doc = *_stashedFindAndModifyDoc;
        _stashedFindAndModifyDoc = boost::none;
        return doc;
    }

    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }
    auto doc = input.releaseDocument();
    if (auto imageEntry = _forgeNoopImageDoc(doc, pExpCtx->opCtx)) {
        return std::move(*imageEntry);
    }
    return doc;
}

boost::optional<Document> DocumentSourceFindAndModifyImageLookup::_forgeNoopImageDoc(
    Document inputDoc, OperationContext* opCtx) {
    // If '_includeCommitTransactionTimestamp' is true, strip any commit transaction timestamp field
    // from the input doc to avoid hitting an unknown field error when parsing the input doc into an
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
                commitTxnTs.getType() == BSONType::bsonTimestamp);
        MutableDocument mutableInputDoc(inputDoc);
        mutableInputDoc.remove(CommitTransactionOplogObject::kCommitTimestampFieldName);
        return {mutableInputDoc.freeze().toBson(), commitTxnTs.getTimestamp()};
    }();

    const auto inputOplogEntry = uassertStatusOK(repl::OplogEntry::parse(inputOplogBson));
    const auto sessionId = inputOplogEntry.getSessionId();
    const auto txnNumber = inputOplogEntry.getTxnNumber();

    if (!sessionId || !txnNumber) {
        // This oplog entry cannot have a retry image.
        return boost::none;
    }

    if (inputOplogEntry.isCrudOpType() && inputOplogEntry.getNeedsRetryImage()) {
        // This is a CRUD oplog entry for a retryable write and it has a retry image.
        if (const auto forgedNoopOplogEntry =
                forgeNoopImageOplogEntry(opCtx, pExpCtx, inputOplogEntry)) {
            const auto imageType = inputOplogEntry.getNeedsRetryImage();
            const auto imageOpTime = forgedNoopOplogEntry->getOpTime();

            // Downcovert the document for this CRUD oplog entry, and then stash it.
            MutableDocument downConvertedDoc{inputDoc};
            downConvertedDoc.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
            downConvertedDoc.setField(
                imageType == repl::RetryImageEnum::kPreImage
                    ? repl::OplogEntry::kPreImageOpTimeFieldName
                    : repl::OplogEntry::kPostImageOpTimeFieldName,
                Value{Document{
                    {repl::OpTime::kTimestampFieldName.toString(), imageOpTime.getTimestamp()},
                    {repl::OpTime::kTermFieldName.toString(), imageOpTime.getTerm()}}});
            _stashedFindAndModifyDoc = downConvertedDoc.freeze();

            return Document{forgedNoopOplogEntry->getEntry().toBSON()};
        }
        return boost::none;
    } else if (inputOplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps &&
               isInternalSessionForRetryableWrite(*sessionId)) {
        // This is an applyOps oplog entry for a retryable internal transaction. Unpack its
        // operations to see if it has a retry image.
        const auto applyOpsCmdObj = inputOplogEntry.getOperationToApply();
        const auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(applyOpsCmdObj);
        auto operationDocs = applyOpsInfo.getOperations();

        for (size_t i = 0; i < operationDocs.size(); i++) {
            auto op = repl::DurableReplOperation::parse(
                {"DocumentSourceFindAndModifyImageLookup::_forgeNoopImageDoc"}, operationDocs[i]);

            if (const auto imageType = op.getNeedsRetryImage()) {
                // This operation has a retry image.
                if (const auto forgedNoopOplogEntry =
                        forgeNoopImageOplogEntry(opCtx, pExpCtx, inputOplogEntry, op)) {
                    const auto imageOpTime = forgedNoopOplogEntry->getOpTime();

                    // Downcovert the document for this applyOps oplog entry by downcoverting this
                    // operation, and then stash it.
                    op.setNeedsRetryImage(boost::none);
                    if (imageType == repl::RetryImageEnum::kPreImage) {
                        op.setPreImageOpTime(imageOpTime);
                    } else if (imageType == repl::RetryImageEnum::kPostImage) {
                        op.setPostImageOpTime(imageOpTime);
                    } else {
                        MONGO_UNREACHABLE;
                    }
                    operationDocs[i] = op.toBSON();
                    const auto downCovertedApplyOpsCmdObj = applyOpsCmdObj.addFields(
                        BSON(repl::ApplyOpsCommandInfo::kOperationsFieldName << operationDocs));
                    MutableDocument downConvertedDoc(inputDoc);
                    downConvertedDoc.setField(repl::OplogEntry::kObjectFieldName,
                                              Value{downCovertedApplyOpsCmdObj});
                    _stashedFindAndModifyDoc = Document(downConvertedDoc.freeze());

                    MutableDocument forgedNoopDoc{
                        Document{forgedNoopOplogEntry->getEntry().toBSON()}};
                    if (commitTxnTs) {
                        forgedNoopDoc.setField(
                            CommitTransactionOplogObject::kCommitTimestampFieldName,
                            Value{*commitTxnTs});
                    }
                    return forgedNoopDoc.freeze();
                }
                return boost::none;
            }
        }
    }

    return boost::none;
}
}  // namespace mongo
