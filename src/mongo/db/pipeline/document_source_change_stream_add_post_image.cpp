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

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/change_stream_helpers_legacy.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/update/update_driver.h"

namespace mongo {

constexpr StringData DocumentSourceChangeStreamAddPostImage::kStageName;
constexpr StringData DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName;

namespace {
REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamAddPostImage,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamAddPostImage::createFromBson,
                                  true);

constexpr auto makePostImageNotFoundErrorMsg =
    &DocumentSourceChangeStreamAddPreImage::makePreImageNotFoundErrorMsg;

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

boost::intrusive_ptr<DocumentSourceChangeStreamAddPostImage>
DocumentSourceChangeStreamAddPostImage::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467608,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::Object);
    auto parsedSpec = DocumentSourceChangeStreamAddPostImageSpec::parse(
        IDLParserContext("DocumentSourceChangeStreamAddPostImageSpec"), elem.Obj());
    return new DocumentSourceChangeStreamAddPostImage(expCtx, parsedSpec.getFullDocument());
}

DocumentSource::GetNextResult DocumentSourceChangeStreamAddPostImage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }
    auto opTypeVal = assertFieldHasType(
        input.getDocument(), DocumentSourceChangeStream::kOperationTypeField, BSONType::String);
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
    output[kFullDocumentFieldName] = (postImageDoc ? Value(*postImageDoc) : Value(BSONNULL));

    // Do not propagate the update modification and pre-image id information further.
    output.remove(kRawOplogUpdateSpecFieldName);
    output.remove(kPreImageIdFieldName);
    return output.freeze();
}

NamespaceString DocumentSourceChangeStreamAddPostImage::assertValidNamespace(
    const Document& inputDoc) const {
    auto namespaceObject =
        assertFieldHasType(inputDoc, DocumentSourceChangeStream::kNamespaceField, BSONType::Object)
            .getDocument();
    auto dbName = assertFieldHasType(namespaceObject, "db"_sd, BSONType::String);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"_sd, BSONType::String);
    NamespaceString nss(pExpCtx->ns.tenantId(), dbName.getString(), collectionName.getString());

    // Change streams on an entire database only need to verify that the database names match. If
    // the database is 'admin', then this is a cluster-wide $changeStream and we are permitted to
    // lookup into any namespace.
    uassert(40579,
            str::stream() << "unexpected namespace during post image lookup: " << nss.ns()
                          << ", expected " << pExpCtx->ns.ns(),
            nss == pExpCtx->ns ||
                (pExpCtx->isClusterAggregation() || pExpCtx->isDBAggregation(nss.db())));

    return nss;
}

boost::optional<Document> DocumentSourceChangeStreamAddPostImage::generatePostImage(
    const Document& updateOp) const {
    // If the 'fullDocumentBeforeChange' is present and null, then we already tried and failed to
    // look up a pre-image. We can't compute the post-image without it, so return early.
    if (updateOp[kFullDocumentBeforeChangeFieldName].getType() == BSONType::jstNULL) {
        return boost::none;
    }

    // Otherwise, obtain the pre-image from the information in the input document.
    auto preImage = [&]() -> boost::optional<Document> {
        // Check whether we have already looked up the pre-image document.
        if (!updateOp[kFullDocumentBeforeChangeFieldName].missing()) {
            return updateOp[kFullDocumentBeforeChangeFieldName].getDocument();
        }

        // Otherwise, we need to look it up ourselves. Extract the preImageId field.
        auto preImageId = updateOp[kPreImageIdFieldName];
        tassert(5869001,
                "Missing both 'fullDocumentBeforeChange' and 'preImageId' fields",
                !preImageId.missing());

        // Use DSCSAddPreImage::lookupPreImage to retrieve the actual pre-image.
        return DocumentSourceChangeStreamAddPreImage::lookupPreImage(pExpCtx,
                                                                     preImageId.getDocument());
    }();

    // Return boost::none if pre-image is missing.
    if (!preImage) {
        return boost::none;
    }

    // Raw oplog update spec field must be provided for the update commands.
    tassert(5869002,
            "Raw oplog update spec was missing or invalid in change stream",
            updateOp[kRawOplogUpdateSpecFieldName].isObject());

    // Setup the UpdateDriver for performing the post-image computation.
    UpdateDriver updateDriver(pExpCtx);
    const auto rawOplogUpdateSpec = updateOp[kRawOplogUpdateSpecFieldName].getDocument().toBson();
    const auto updateMod = write_ops::UpdateModification::parseFromOplogEntry(
        rawOplogUpdateSpec, {false /* mustCheckExistenceForInsertOperations */});
    // UpdateDriver only expects to apply a diff in the context of oplog application.
    updateDriver.setFromOplogApplication(true);
    updateDriver.parse(updateMod, {});

    // Compute post-image.
    mutablebson::Document postImage(preImage->toBson());
    uassertStatusOK(updateDriver.update(pExpCtx->opCtx,
                                        StringData(),
                                        &postImage,
                                        false /* validateForStorage */,
                                        FieldRefSet(),
                                        false /* isInsert */));
    return Document(postImage.getObject());
}

boost::optional<Document> DocumentSourceChangeStreamAddPostImage::lookupLatestPostImage(
    const Document& updateOp) const {
    // Make sure we have a well-formed input.
    auto nss = assertValidNamespace(updateOp);

    auto documentKey = assertFieldHasType(updateOp,
                                          DocumentSourceChangeStream::kDocumentKeyField,
                                          BSONType::Object)
                           .getDocument();

    // Extract the resume token data from the input event.
    auto resumeTokenData =
        ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument()).getData();

    auto readConcern = BSON("level"
                            << "majority"
                            << "afterClusterTime" << resumeTokenData.clusterTime);

    // Update lookup queries sent from mongoS to shards are allowed to use speculative majority
    // reads. Even if the lookup itself succeeded, it may not have returned any results if the
    // document was deleted in the time since the update op.
    invariant(resumeTokenData.uuid);
    return pExpCtx->mongoProcessInterface->lookupSingleDocument(
        pExpCtx, nss, *resumeTokenData.uuid, documentKey, std::move(readConcern));
}

Value DocumentSourceChangeStreamAddPostImage::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return explain
        ? Value(Document{
              {DocumentSourceChangeStream::kStageName,
               Document{{"stage"_sd, kStageName},
                        {kFullDocumentFieldName, FullDocumentMode_serializer(_fullDocumentMode)}}}})
        : Value(Document{{kStageName,
                          DocumentSourceChangeStreamAddPostImageSpec(_fullDocumentMode).toBSON()}});
}

}  // namespace mongo
