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

#include "mongo/db/exec/agg/change_stream_update_lookup_stage.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo::exec::agg {
namespace {
Value assertFieldHasType(const Document& fullDoc,
                         std::string_view fieldName,
                         BSONType expectedType) {
    auto val = fullDoc[fieldName];
    uassert(12840700,
            str::stream() << "failed to look up post image after change: expected \"" << fieldName
                          << "\" field to have type " << typeName(expectedType)
                          << ", instead found type " << typeName(val.getType()) << ": "
                          << val.toString() << ", full object: " << fullDoc.toString(),
            val.getType() == expectedType);
    return val;
}

NamespaceString parseNss(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const Document& inputDoc) {
    auto namespaceObject =
        assertFieldHasType(inputDoc, DocumentSourceChangeStream::kNamespaceField, BSONType::object)
            .getDocument();
    auto dbName = assertFieldHasType(namespaceObject, "db"_sd, BSONType::string);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"_sd, BSONType::string);
    NamespaceString nss(NamespaceStringUtil::deserialize(expCtx->getNamespaceString().tenantId(),
                                                         dbName.getStringData(),
                                                         collectionName.getStringData(),
                                                         expCtx->getSerializationContext()));

    // Change streams on an entire database only need to verify that the database names match. If
    // the database is 'admin', then this is a cluster-wide $changeStream and we are permitted to
    // lookup into any namespace.
    uassert(12840701,
            str::stream() << "unexpected namespace during post image lookup: "
                          << nss.toStringForErrorMsg() << ", expected "
                          << expCtx->getNamespaceString().toStringForErrorMsg(),
            nss == expCtx->getNamespaceString() || expCtx->isClusterAggregation() ||
                expCtx->isDBAggregation(nss));

    return nss;
}

/**
 * Returns the namespace to look the post-image up in. For a collection-level change stream the
 * target namespace is fixed at the stream's namespace, so we skip the per-event parse (the only
 * value parseNss could return is that namespace, which it asserts). Database- and cluster-wide
 * streams have a per-event namespace that must be parsed from the event.
 */
NamespaceString getLookupNss(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                             const ChangeStream& changeStream,
                             const Document& updateOp) {
    if (changeStream.getChangeStreamType() == ChangeStreamType::kCollection) {
        dassert(parseNss(expCtx, updateOp) == *changeStream.getNamespace());
        return *changeStream.getNamespace();
    }
    return parseNss(expCtx, updateOp);
}

/**
 * Returns the clusterTime of the event for the lookup's readConcern. Every change event mirrors its
 * resume token's clusterTime in a top-level 'clusterTime' field (see
 * change_stream_event_transform), so we read it directly instead of decoding the resume token
 * keystring -- the dominant per-event lookup cost on the flamegraph.
 */
Timestamp getClusterTime(const Document& updateOp) {
    auto clusterTime = assertFieldHasType(updateOp,
                                          DocumentSourceChangeStream::kClusterTimeField,
                                          BSONType::timestamp)
                           .getTimestamp();
    dassert(ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument())
                .getData()
                .clusterTime == clusterTime);
    return clusterTime;
}

/**
 * Returns the collection UUID to match against the lookup target, or boost::none. The UUID is only
 * required for the internal 'matchCollectionUUID' flag and is only obtainable from the resume
 * token, so we decode the token lazily -- only when the flag is set.
 */
boost::optional<UUID> getCollectionUUID(const Document& updateOp, bool shouldMatchCollectionUUID) {
    if (!shouldMatchCollectionUUID) {
        return boost::none;
    }
    return ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument())
        .getData()
        .uuid;
}
}  // namespace

ChangeStreamUpdateLookupStage::ChangeStreamUpdateLookupStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor)
    : Stage(stageName, pExpCtx),
      _lookupExecutor(std::move(lookupExecutor)),
      _changeStream(ChangeStream::buildFromExpressionContext(pExpCtx)) {
    tassert(12841000, "SingleDocumentLookupExecutor must be provided", _lookupExecutor);
}

boost::optional<Document> ChangeStreamUpdateLookupStage::performPostImageLookup(
    const Document& updateOp) {
    auto nss = getLookupNss(pExpCtx, _changeStream, updateOp);
    auto collectionUUID = getCollectionUUID(
        updateOp, pExpCtx->getChangeStreamSpec()->getMatchCollectionUUIDForUpdateLookup());
    auto documentKey = assertFieldHasType(updateOp,
                                          DocumentSourceChangeStream::kDocumentKeyField,
                                          BSONType::object)
                           .getDocument();

    try {
        auto result = _lookupExecutor->performLookup(
            pExpCtx, nss, std::move(collectionUUID), documentKey, getClusterTime(updateOp));
        tassert(12841001,
                "lookup executor chain must terminate in a handled result",
                result.status !=
                    SingleDocumentLookupExecutor::LookupResult::HandledStatus::kNotHandled);
        return result.document;
    } catch (const ExceptionFor<ErrorCodes::TooManyMatchingDocuments>& ex) {
        uasserted(ErrorCodes::ChangeStreamFatalError, ex.what());
    }
}

GetNextResult ChangeStreamUpdateLookupStage::doGetNext() {
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
    const auto postImageDoc = performPostImageLookup(output.peek());

    // Even if no post-image was found, we have to populate the 'fullDocument' field.
    output[DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName] =
        (postImageDoc ? Value(*postImageDoc) : Value(BSONNULL));

    return output.freeze();
}

}  // namespace mongo::exec::agg
