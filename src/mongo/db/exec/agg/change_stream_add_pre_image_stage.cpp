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

#include "mongo/db/exec/agg/change_stream_add_pre_image_stage.h"

#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPreImageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamAddPreImageDS =
        dynamic_cast<DocumentSourceChangeStreamAddPreImage*>(documentSource.get());

    tassert(10561300,
            "expected 'DocumentSourceChangeStreamAddPreImage' type",
            changeStreamAddPreImageDS);

    return make_intrusive<exec::agg::ChangeStreamAddPreImageStage>(
        changeStreamAddPreImageDS->kStageName,
        changeStreamAddPreImageDS->getExpCtx(),
        changeStreamAddPreImageDS->_fullDocumentBeforeChangeMode);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamAddPreImage,
                           DocumentSourceChangeStreamAddPreImage::id,
                           documentSourceChangeStreamAddPreImageToStageFn)

ChangeStreamAddPreImageStage::ChangeStreamAddPreImageStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const FullDocumentBeforeChangeModeEnum& fullDocumentBeforeChangeMode)
    : Stage(stageName, pExpCtx), _fullDocumentBeforeChangeMode(fullDocumentBeforeChangeMode) {}

GetNextResult ChangeStreamAddPreImageStage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }

    // If this is not an update, replace or delete, then just pass along the result.
    const auto kOpTypeField = DocumentSourceChangeStream::kOperationTypeField;
    const auto opType = input.getDocument()[kOpTypeField];
    DocumentSourceChangeStream::checkValueType(opType, kOpTypeField, BSONType::string);
    if (opType.getStringData() != DocumentSourceChangeStream::kUpdateOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kReplaceOpType &&
        opType.getStringData() != DocumentSourceChangeStream::kDeleteOpType) {
        return input;
    }

    auto preImageId =
        input.getDocument()[DocumentSourceChangeStreamAddPreImage::kPreImageIdFieldName];
    tassert(6091900, "Pre-image id field is missing", !preImageId.missing());
    tassert(5868900,
            "Expected pre-image id field to be a document",
            preImageId.getType() == BSONType::object);

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
    outputDoc[DocumentSourceChangeStreamAddPreImage::kFullDocumentBeforeChangeFieldName] =
        (preImageDoc ? Value(*preImageDoc) : Value(BSONNULL));

    // Do not propagate preImageId field further through the pipeline.
    outputDoc.remove(DocumentSourceChangeStreamAddPreImage::kPreImageIdFieldName);

    return outputDoc.freeze();
}

std::string ChangeStreamAddPreImageStage::makePreImageNotFoundErrorMsg(const Document& event) {
    auto errMsgDoc = Document{{"operationType", event["operationType"]},
                              {"ns", event["ns"]},
                              {"clusterTime", event["clusterTime"]},
                              {"txnNumber", event["txnNumber"]}};
    return errMsgDoc.toString();
}

boost::optional<Document> ChangeStreamAddPreImageStage::lookupPreImage(
    boost::intrusive_ptr<ExpressionContext> pExpCtx, const Document& preImageId) {
    // Look up the pre-image document on the local node by id.
    const auto tenantId = change_stream_serverless_helpers::resolveTenantId(
        VersionContext::getDecoration(pExpCtx->getOperationContext()),
        pExpCtx->getNamespaceString().tenantId());

    // Pre-images are only supported in single-tenant environments.
    tassert(10915400,
            "Failed to lookup pre-image given change stream pre-images aren't supported in "
            "multi-tenant environments",
            tenantId == boost::none);

    auto lookedUpDoc = pExpCtx->getMongoProcessInterface()->lookupSingleDocumentLocally(
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

}  // namespace exec::agg
}  // namespace mongo
