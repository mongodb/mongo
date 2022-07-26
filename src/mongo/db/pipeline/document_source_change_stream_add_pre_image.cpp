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

#include "mongo/util/assert_util.h"

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/util/intrusive_counter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {
REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamAddPreImage,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamAddPreImage::createFromBson,
                                  true);
}  // namespace

constexpr StringData DocumentSourceChangeStreamAddPreImage::kStageName;
constexpr StringData DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName;

boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage>
DocumentSourceChangeStreamAddPreImage::create(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                              const DocumentSourceChangeStreamSpec& spec) {
    auto mode = spec.getFullDocumentBeforeChange();

    return make_intrusive<DocumentSourceChangeStreamAddPreImage>(expCtx, mode);
}

boost::intrusive_ptr<DocumentSourceChangeStreamAddPreImage>
DocumentSourceChangeStreamAddPreImage::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467610,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::Object);
    auto parsedSpec = DocumentSourceChangeStreamAddPreImageSpec::parse(
        IDLParserContext("DocumentSourceChangeStreamAddPreImageSpec"), elem.Obj());
    return make_intrusive<DocumentSourceChangeStreamAddPreImage>(
        expCtx, parsedSpec.getFullDocumentBeforeChange());
}

DocumentSource::GetNextResult DocumentSourceChangeStreamAddPreImage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    // If this is not an update, replace or delete, then just pass along the result.
    const auto kOpTypeField = DocumentSourceChangeStream::kOperationTypeField;
    const auto opType = input.getDocument()[kOpTypeField];
    DocumentSourceChangeStream::checkValueType(opType, kOpTypeField, BSONType::String);
    if (opType.getStringData() != DocumentSourceChangeStream::kUpdateOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kReplaceOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kDeleteOpType) {
        return input;
    }

    // If a pre-image is available, the transform stage will have populated it in the event's
    // 'fullDocumentBeforeChange' field. If this field is missing and the pre-imaging mode is
    // 'required', we throw an exception. Otherwise, we pass along the document unmodified.
    auto preImageId = input.getDocument()[kPreImageIdFieldName];
    if (preImageId.missing()) {
        uassert(51770,
                str::stream()
                    << "Change stream was configured to require a pre-image for all update, delete "
                       "and replace events, but pre-image id was not available for event: "
                    << makePreImageNotFoundErrorMsg(input.getDocument()),
                _fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kRequired);
        return input;
    }
    tassert(5868900,
            "Expected pre-image id field to be a document",
            preImageId.getType() == BSONType::Object);

    // Obtain the pre-image document, if available, given the specified preImageId.
    auto preImageDoc = lookupPreImage(pExpCtx, preImageId.getDocument());
    uassert(
        ErrorCodes::NoMatchingDocument,
        str::stream() << "Change stream was configured to require a pre-image for all update, "
                         "delete and replace events, but the pre-image was not found for event: "
                      << makePreImageNotFoundErrorMsg(input.getDocument()),
        preImageDoc ||
            _fullDocumentBeforeChangeMode != FullDocumentBeforeChangeModeEnum::kRequired);

    // Even if no pre-image was found, we have to populate the 'fullDocumentBeforeChange' field.
    MutableDocument outputDoc(input.releaseDocument());
    outputDoc[kFullDocumentBeforeChangeFieldName] =
        (preImageDoc ? Value(*preImageDoc) : Value(BSONNULL));

    // Do not propagate preImageId field further through the pipeline.
    outputDoc.remove(kPreImageIdFieldName);

    return outputDoc.freeze();
}

boost::optional<Document> DocumentSourceChangeStreamAddPreImage::lookupPreImage(
    boost::intrusive_ptr<ExpressionContext> pExpCtx, const Document& preImageId) {
    // If the pre-image id does not contain the nsUUID field, then it is in legacy format. Look
    // up the pre-image in the oplog.
    if (preImageId[ChangeStreamPreImageId::kNsUUIDFieldName].missing()) {
        return change_stream_legacy::legacyLookupPreImage(pExpCtx, preImageId);
    }

    // Look up the pre-image document on the local node by id.
    auto lookedUpDoc = pExpCtx->mongoProcessInterface->lookupSingleDocumentLocally(
        pExpCtx,
        NamespaceString::kChangeStreamPreImagesNamespace,
        Document{{ChangeStreamPreImage::kIdFieldName, preImageId}});

    // Return boost::none to signify that we failed to find the pre-image.
    if (!lookedUpDoc) {
        return boost::none;
    }

    // Return "preImage" field value from the document.
    auto preImageField = lookedUpDoc->getField(ChangeStreamPreImage::kPreImageFieldName);
    tassert(
        6148000, "Pre-image document must contain the 'preImage' field", !preImageField.nullish());
    return preImageField.getDocument().getOwned();
}

Value DocumentSourceChangeStreamAddPreImage::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return explain
        ? Value(Document{
              {DocumentSourceChangeStream::kStageName,
               Document{{"stage"_sd, "internalAddPreImage"_sd},
                        {"fullDocumentBeforeChange"_sd,
                         FullDocumentBeforeChangeMode_serializer(_fullDocumentBeforeChangeMode)}}}})
        : Value(Document{
              {kStageName,
               DocumentSourceChangeStreamAddPreImageSpec(_fullDocumentBeforeChangeMode).toBSON()}});
}

std::string DocumentSourceChangeStreamAddPreImage::makePreImageNotFoundErrorMsg(
    const Document& event) {
    auto errMsgDoc = Document{{"operationType", event["operationType"]},
                              {"ns", event["ns"]},
                              {"clusterTime", event["clusterTime"]},
                              {"txnNumber", event["txnNumber"]}};
    return errMsgDoc.toString();
}

}  // namespace mongo
