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

#include "mongo/db/exec/agg/change_stream_add_post_image_stage.h"

#include "mongo/db/exec/agg/change_stream_add_pre_image_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamAddPostImageToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamAddPostImageDS =
        dynamic_cast<DocumentSourceChangeStreamAddPostImage*>(documentSource.get());

    tassert(10561301,
            "expected 'DocumentSourceChangeStreamAddPostImage' type",
            changeStreamAddPostImageDS);

    return make_intrusive<exec::agg::ChangeStreamAddPostImageStage>(
        changeStreamAddPostImageDS->kStageName,
        changeStreamAddPostImageDS->getExpCtx(),
        changeStreamAddPostImageDS->_fullDocumentMode);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamAddPostImage,
                           DocumentSourceChangeStreamAddPostImage::id,
                           documentSourceChangeStreamAddPostImageToStageFn)

namespace {
constexpr auto makePostImageNotFoundErrorMsg =
    &ChangeStreamAddPreImageStage::makePreImageNotFoundErrorMsg;

Value assertFieldHasType(const Document& fullDoc, StringData fieldName, BSONType expectedType) {
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
    StringData stageName,
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
    if (opTypeVal.getString() != DocumentSourceChangeStream::kUpdateOpType) {
        return input;
    }

    // Create a mutable output document from the input document.
    MutableDocument output(input.releaseDocument());
    const auto postImageDoc = (_fullDocumentMode == FullDocumentModeEnum::kUpdateLookup
                                   ? lookupLatestPostImage(output.peek())
                                   : generatePostImage(output.peek()));
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

boost::optional<Document> ChangeStreamAddPostImageStage::lookupLatestPostImage(
    const Document& updateOp) const {
    // Make sure we have a well-formed input.
    auto nss = assertValidNamespace(updateOp);

    auto documentKey = assertFieldHasType(updateOp,
                                          DocumentSourceChangeStream::kDocumentKeyField,
                                          BSONType::object)
                           .getDocument();

    // Extract the resume token data from the input event.
    auto resumeTokenData =
        ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument()).getData();

    auto readConcern = BSON("level" << "majority"
                                    << "afterClusterTime" << resumeTokenData.clusterTime);

    // Update lookup queries sent from mongoS to shards are allowed to use speculative majority
    // reads. Even if the lookup itself succeeded, it may not have returned any results if the
    // document was deleted in the time since the update op.
    tassert(9797601, "UUID should be present in the resume token", resumeTokenData.uuid);
    try {
        // In case we are running $changeStreams and are performing updateLookup, we do not pass
        // 'collectionUUID' to avoid any UUID validation on the target collection. UUID of the
        // target collection should only be checked if 'matchCollectionUUIDForUpdateLookup' flag has
        // been passed.
        auto collectionUUID =
            pExpCtx->getChangeStreamSpec()->getMatchCollectionUUIDForUpdateLookup()
            ? boost::optional<UUID>(*resumeTokenData.uuid)
            : boost::none;
        return pExpCtx->getMongoProcessInterface()->lookupSingleDocument(
            pExpCtx, nss, std::move(collectionUUID), documentKey, std::move(readConcern));
    } catch (const ExceptionFor<ErrorCodes::TooManyMatchingDocuments>& ex) {
        uasserted(ErrorCodes::ChangeStreamFatalError, ex.what());
    }
}

NamespaceString ChangeStreamAddPostImageStage::assertValidNamespace(
    const Document& inputDoc) const {
    auto namespaceObject =
        assertFieldHasType(inputDoc, DocumentSourceChangeStream::kNamespaceField, BSONType::object)
            .getDocument();
    auto dbName = assertFieldHasType(namespaceObject, "db"_sd, BSONType::string);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"_sd, BSONType::string);
    NamespaceString nss(NamespaceStringUtil::deserialize(pExpCtx->getNamespaceString().tenantId(),
                                                         dbName.getString(),
                                                         collectionName.getString(),
                                                         pExpCtx->getSerializationContext()));

    // Change streams on an entire database only need to verify that the database names match. If
    // the database is 'admin', then this is a cluster-wide $changeStream and we are permitted to
    // lookup into any namespace.
    uassert(40579,
            str::stream() << "unexpected namespace during post image lookup: "
                          << nss.toStringForErrorMsg() << ", expected "
                          << pExpCtx->getNamespaceString().toStringForErrorMsg(),
            nss == pExpCtx->getNamespaceString() ||
                (pExpCtx->isClusterAggregation() || pExpCtx->isDBAggregation(nss)));

    return nss;
}

boost::optional<Document> ChangeStreamAddPostImageStage::generatePostImage(
    const Document& updateOp) const {
    // If the 'fullDocumentBeforeChange' is present and null, then we already tried and failed to
    // look up a pre-image. We can't compute the post-image without it, so return early.
    if (updateOp[DocumentSourceChangeStreamAddPostImage::kFullDocumentBeforeChangeFieldName]
            .getType() == BSONType::null) {
        return boost::none;
    }

    // Otherwise, obtain the pre-image from the information in the input document.
    auto preImage = [&]() -> boost::optional<Document> {
        // Check whether we have already looked up the pre-image document.
        if (!updateOp[DocumentSourceChangeStreamAddPostImage::kFullDocumentBeforeChangeFieldName]
                 .missing()) {
            return updateOp
                [DocumentSourceChangeStreamAddPostImage::kFullDocumentBeforeChangeFieldName]
                    .getDocument();
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
    tassert(
        5869002,
        "Raw oplog update spec was missing or invalid in change stream",
        updateOp[DocumentSourceChangeStreamAddPostImage::kRawOplogUpdateSpecFieldName].isObject());

    // Setup the UpdateDriver for performing the post-image computation.
    UpdateDriver updateDriver(pExpCtx);
    const auto rawOplogUpdateSpec =
        updateOp[DocumentSourceChangeStreamAddPostImage::kRawOplogUpdateSpecFieldName]
            .getDocument()
            .toBson();
    const auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        rawOplogUpdateSpec, {false /* mustCheckExistenceForInsertOperations */});
    // UpdateDriver only expects to apply a diff in the context of oplog application.
    updateDriver.setFromOplogApplication(true);
    updateDriver.parse(updateMod, {});

    // Compute post-image.
    mutablebson::Document postImage(preImage->toBson());
    uassertStatusOK(updateDriver.update(pExpCtx->getOperationContext(),
                                        StringData(),
                                        &postImage,
                                        false /* validateForStorage */,
                                        FieldRefSet(),
                                        false /* isInsert */));
    return Document(postImage.getObject());
}

}  // namespace exec::agg
}  // namespace mongo
