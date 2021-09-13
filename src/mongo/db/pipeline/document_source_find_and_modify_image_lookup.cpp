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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_find_and_modify_image_lookup.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
// Downconverts a 'findAndModify' entry by stripping the 'needsRetryImage' field and appending
// the appropriate 'preImageOpTime' or 'postImageOpTime' field.
Document downConvertFindAndModifyEntry(Document inputDoc,
                                       repl::OpTime imageOpTime,
                                       repl::RetryImageEnum imageType) {
    MutableDocument doc{inputDoc};
    const auto imageOpTimeFieldName = imageType == repl::RetryImageEnum::kPreImage
        ? repl::OplogEntry::kPreImageOpTimeFieldName
        : repl::OplogEntry::kPostImageOpTimeFieldName;
    doc.setField(
        imageOpTimeFieldName,
        Value{Document{{repl::OpTime::kTimestampFieldName.toString(), imageOpTime.getTimestamp()},
                       {repl::OpTime::kTermFieldName.toString(), imageOpTime.getTerm()}}});
    doc.remove(repl::OplogEntryBase::kNeedsRetryImageFieldName);
    return doc.freeze();
}
}  // namespace

using OplogEntry = repl::OplogEntryBase;

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalFindAndModifyImageLookup,
                                  LiteParsedDocumentSourceDefault::parse,
                                  DocumentSourceFindAndModifyImageLookup::createFromBson,
                                  true);

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    return new DocumentSourceFindAndModifyImageLookup(expCtx);
}

boost::intrusive_ptr<DocumentSourceFindAndModifyImageLookup>
DocumentSourceFindAndModifyImageLookup::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5806003,
            str::stream() << "the '" << kStageName << "' spec must be an empty object",
            elem.type() == BSONType::Object && elem.Obj().isEmpty());
    return DocumentSourceFindAndModifyImageLookup::create(expCtx);
}

DocumentSourceFindAndModifyImageLookup::DocumentSourceFindAndModifyImageLookup(
    const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSource(kStageName, expCtx) {}

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
    return Value(Document{{kStageName, Value(Document{})}});
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
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<std::string>{}, {}};
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
    const auto needsRetryImageVal =
        inputDoc.getField(repl::OplogEntryBase::kNeedsRetryImageFieldName);
    if (needsRetryImageVal.missing()) {
        return boost::none;
    }

    const auto inputDocBson = inputDoc.toBson();
    const auto sessionIdBson = inputDocBson.getObjectField(OplogEntry::kSessionIdFieldName);
    auto localImageCollInfo = pExpCtx->mongoProcessInterface->getCollectionOptions(
        pExpCtx->opCtx, NamespaceString::kConfigImagesNamespace);

    // Extract the UUID from the collection information. We should always have a valid uuid
    // here.
    auto imageCollUUID = invariantStatusOK(UUID::parse(localImageCollInfo["uuid"]));
    auto imageDoc = pExpCtx->mongoProcessInterface->lookupSingleDocument(
        pExpCtx,
        NamespaceString::kConfigImagesNamespace,
        imageCollUUID,
        Document{BSON("_id" << sessionIdBson)},
        boost::none /* readConcern not allowed in v5.0 for local lookup */);

    if (!imageDoc) {
        // If no image document with the corresponding 'sessionId' is found, we skip forging the
        // no-op and rely on the retryable write mechanism to catch that no pre- or post- image
        // exists.
        LOGV2_DEBUG(
            580602,
            2,
            "Not forging no-op image oplog entry because no image document found with sessionId",
            "sessionId"_attr = sessionIdBson);
        return boost::none;
    }

    auto image = repl::ImageEntry::parse(IDLParserErrorContext("image entry"), imageDoc->toBson());
    const auto inputOplog = uassertStatusOK(repl::OplogEntry::parse(inputDocBson));
    if (image.getTxnNumber() != inputOplog.getTxnNumber()) {
        // In our snapshot, fetch the current transaction number for a session. If that
        // transaction number doesn't match what's found on the image lookup, it implies that
        // the image is not the correct version for this oplog entry. We will not forge a noop
        // from it.
        LOGV2_DEBUG(
            580603,
            2,
            "Not forging no-op image oplog entry because image document has a different txnNum",
            "sessionId"_attr = sessionIdBson,
            "expectedTxnNum"_attr = inputOplog.getTxnNumber(),
            "actualTxnNum"_attr = image.getTxnNumber());
        return boost::none;
    }

    // Stash the 'findAndModify' document to return after downconverting it.
    repl::OpTime imageOpTime(inputOplog.getTimestamp() - 1, *inputOplog.getTerm());
    const auto docToStash =
        downConvertFindAndModifyEntry(inputDoc,
                                      imageOpTime,
                                      repl::RetryImage_parse(IDLParserErrorContext("retry image"),
                                                             needsRetryImageVal.getStringData()));
    _stashedFindAndModifyDoc = docToStash;

    // Forge a no-op image document to be returned.
    repl::MutableOplogEntry forgedNoop;
    forgedNoop.setSessionId(image.get_id());
    forgedNoop.setTxnNumber(image.getTxnNumber());
    forgedNoop.setObject(image.getImage());
    forgedNoop.setOpType(repl::OpTypeEnum::kNoop);
    forgedNoop.setWallClockTime(inputOplog.getWallClockTime());
    forgedNoop.setNss(inputOplog.getNss());
    forgedNoop.setUuid(*inputOplog.getUuid());

    // Set the opTime to be the findAndModify timestamp - 1. We guarantee that there will be no
    // collisions because we always reserve an extra oplog slot when writing the retryable
    // findAndModify entry on the primary.
    forgedNoop.setOpTime(repl::OpTime(imageOpTime.getTimestamp(), *inputOplog.getTerm()));
    forgedNoop.setStatementIds({0});
    return Document{forgedNoop.toBSON()};
}
}  // namespace mongo
