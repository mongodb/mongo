// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_update_lookup_stage.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

namespace mongo::exec::agg {
namespace {
using namespace std::literals::string_view_literals;
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
    auto dbName = assertFieldHasType(namespaceObject, "db"sv, BSONType::string);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"sv, BSONType::string);
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
 * required for the internal 'matchCollectionUUID' flag. When the flag is set, the event transform
 * mirrors the resume token's UUID in the event's 'collectionUUID' field, so we read it directly
 * instead of decoding the resume token keystring.
 */
boost::optional<UUID> getCollectionUUID(const Document& updateOp, bool shouldMatchCollectionUUID) {
    if (!shouldMatchCollectionUUID) {
        return boost::none;
    }
    auto uuidVal = updateOp[DocumentSourceChangeStream::kCollectionUuidField];
    tassert(12888200,
            str::stream() << "expected the event transform to emit the '"
                          << DocumentSourceChangeStream::kCollectionUuidField
                          << "' field on update events when matching the collection UUID, full "
                             "object: "
                          << updateOp.toString(),
            uuidVal.getType() == BSONType::binData);
    auto uuid = uuidVal.getUuid();
    dassert(ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument())
                .getData()
                .uuid == uuid);
    return uuid;
}
}  // namespace

ChangeStreamUpdateLookupStage::ChangeStreamUpdateLookupStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    std::unique_ptr<SingleDocumentLookupExecutor> lookupExecutor,
    Limits limits)
    : BatchedEnrichmentStage(stageName, pExpCtx, limits),
      _lookupExecutor(std::move(lookupExecutor)),
      _changeStream(ChangeStream::buildFromExpressionContext(pExpCtx)) {
    tassert(12841000, "SingleDocumentLookupExecutor must be provided", _lookupExecutor);
}

void ChangeStreamUpdateLookupStage::beginBatch() {
    _batch.emplace(*_lookupExecutor);
}

void ChangeStreamUpdateLookupStage::closeBatch() noexcept {
    _batch.reset();
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

Document ChangeStreamUpdateLookupStage::enrich(Document event) {
    auto opTypeVal = assertFieldHasType(
        event, DocumentSourceChangeStream::kOperationTypeField, BSONType::string);
    if (opTypeVal.getStringData() != DocumentSourceChangeStream::kUpdateOpType) {
        return event;
    }

    // Create a mutable output document from the input document.
    MutableDocument output(std::move(event));
    const auto postImageDoc = performPostImageLookup(output.peek());

    // Even if no post-image was found, we have to populate the 'fullDocument' field.
    output[DocumentSourceChangeStreamAddPostImage::kFullDocumentFieldName] =
        (postImageDoc ? Value(*postImageDoc) : Value(BSONNULL));

    // Remove the 'collectionUUID' field unless it is a user-visible field of this stream's events.
    if (!change_stream::shouldEmitCollectionUUIDForChangeEvent(*pExpCtx->getChangeStreamSpec())) {
        output.remove(DocumentSourceChangeStream::kCollectionUuidField);
    }
    return output.freeze();
}

}  // namespace mongo::exec::agg
