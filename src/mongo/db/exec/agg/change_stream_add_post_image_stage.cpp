// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_add_post_image_stage.h"

#include "mongo/db/exec/agg/change_stream_add_pre_image_stage.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {
namespace exec::agg {
using namespace std::literals::string_view_literals;

namespace {
constexpr auto makePostImageNotFoundErrorMsg =
    &ChangeStreamAddPreImageStage::makePreImageNotFoundErrorMsg;

Value assertFieldHasType(const Document& fullDoc,
                         std::string_view fieldName,
                         BSONType expectedType) {
    auto val = fullDoc[fieldName];
    uassert(40578,
            str::stream() << "failed to look up post image after change: expected \"" << fieldName
                          << "\" field to have type " << typeName(expectedType)
                          << ", instead found type " << typeName(val.getType()) << ": "
                          << val.toString() << ", full object: " << fullDoc.toString(),
            val.getType() == expectedType);
    return val;
}
}  // namespace

ChangeStreamAddPostImageStage::ChangeStreamAddPostImageStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    const FullDocumentModeEnum& fullDocumentMode)
    : Stage(stageName, pExpCtx), _fullDocumentMode(fullDocumentMode) {}

GetNextResult ChangeStreamAddPostImageStage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }
    auto opTypeVal = assertFieldHasType(
        input.getDocument(), DocumentSourceChangeStream::kOperationTypeField, BSONType::string);
    if (opTypeVal.getStringData() != DocumentSourceChangeStream::kUpdateOpType) {
        return input;
    }

    // Create a mutable output document from the input document.
    MutableDocument output(input.releaseDocument());
    const auto postImageDoc = generatePostImage(output.peek());
    uassert(ErrorCodes::NoMatchingDocument,
            str::stream() << "Change stream was configured to require a post-image for all update "
                             "events, but the post-image was not found for event: "
                          << makePostImageNotFoundErrorMsg(output.peek()),
            postImageDoc || _fullDocumentMode != FullDocumentModeEnum::kRequired);

    // Even if no post-image was found, we have to populate the 'fullDocument' field.
    output[DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName] =
        (postImageDoc ? Value(*postImageDoc) : Value(BSONNULL));

    // Do not propagate the update modification and pre-image id information further.
    output.remove(DocumentSourceChangeStreamAddPostImage::kRawOplogUpdateSpecFieldName);
    output.remove(DocumentSourceChangeStreamAddPostImage::kPreImageIdFieldName);
    return output.freeze();
}

boost::optional<Document> ChangeStreamAddPostImageStage::generatePostImage(
    const Document& updateOp) const {
    auto fullDocumentBeforeChange =
        updateOp[DocumentSourceChangeStreamAddPostImage::kFullDocumentBeforeChangeFieldName];

    // If the 'fullDocumentBeforeChange' is present and null, then we already tried and failed to
    // look up a pre-image. We can't compute the post-image without it, so return early.
    if (fullDocumentBeforeChange.getType() == BSONType::null) {
        return boost::none;
    }

    // Otherwise, obtain the pre-image from the information in the input document.
    auto preImage = [&]() -> boost::optional<Document> {
        // Check whether we have already looked up the pre-image document.
        if (!fullDocumentBeforeChange.missing()) {
            return fullDocumentBeforeChange.getDocument();
        }

        // Otherwise, we need to look it up ourselves. Extract the preImageId field.
        auto preImageId = updateOp[DocumentSourceChangeStreamAddPostImage::kPreImageIdFieldName];
        tassert(5869001,
                "Missing both 'fullDocumentBeforeChange' and 'preImageId' fields",
                !preImageId.missing());

        // Use DSCSAddPreImage::lookupPreImage to retrieve the actual pre-image.
        return exec::agg::ChangeStreamAddPreImageStage::lookupPreImage(pExpCtx,
                                                                       preImageId.getDocument());
    }();

    // Return boost::none if pre-image is missing.
    if (!preImage) {
        return boost::none;
    }

    // Raw oplog update spec field must be provided for the update commands.
    auto rawOplogUpdateSpec =
        updateOp[DocumentSourceChangeStreamAddPostImage::kRawOplogUpdateSpecFieldName];
    tassert(5869002,
            "Raw oplog update spec was missing or invalid in change stream",
            rawOplogUpdateSpec.isObject());

    // Setup the UpdateDriver for performing the post-image computation.
    UpdateDriver updateDriver(pExpCtx);
    const auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        rawOplogUpdateSpec.getDocument().toBson(),
        {false /* mustCheckExistenceForInsertOperations */});
    // UpdateDriver only expects to apply a diff in the context of oplog application.
    updateDriver.setFromOplogApplication(true);
    updateDriver.parse(updateMod, {});

    // Compute post-image.
    mutablebson::Document postImage(preImage->toBson());
    uassertStatusOK(updateDriver.update(pExpCtx->getOperationContext(),
                                        std::string_view(),
                                        &postImage,
                                        false /* validateForStorage */,
                                        FieldRefSet(),
                                        false /* isInsert */));
    return Document(postImage.getObject());
}

}  // namespace exec::agg
}  // namespace mongo
