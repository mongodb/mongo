/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"

#include "mongo/bson/simple_bsonelement_comparator.h"

namespace mongo {

constexpr StringData DocumentSourceLookupChangePostImage::kStageName;
constexpr StringData DocumentSourceLookupChangePostImage::kFullDocumentFieldName;

namespace {
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

DocumentSource::GetNextResult DocumentSourceLookupChangePostImage::doGetNext() {
    auto input = pSource->getNext();
    if (!input.isAdvanced()) {
        return input;
    }
    auto opTypeVal = assertFieldHasType(
        input.getDocument(), DocumentSourceChangeStream::kOperationTypeField, BSONType::String);
    if (opTypeVal.getString() != DocumentSourceChangeStream::kUpdateOpType) {
        return input;
    }

    MutableDocument output(input.releaseDocument());
    output[kFullDocumentFieldName] = lookupPostImage(output.peek());
    return output.freeze();
}

NamespaceString DocumentSourceLookupChangePostImage::assertValidNamespace(
    const Document& inputDoc) const {
    auto namespaceObject =
        assertFieldHasType(inputDoc, DocumentSourceChangeStream::kNamespaceField, BSONType::Object)
            .getDocument();
    auto dbName = assertFieldHasType(namespaceObject, "db"_sd, BSONType::String);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"_sd, BSONType::String);
    NamespaceString nss(dbName.getString(), collectionName.getString());

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

Value DocumentSourceLookupChangePostImage::lookupPostImage(const Document& updateOp) const {
    // Make sure we have a well-formed input.
    auto nss = assertValidNamespace(updateOp);

    auto documentKey = assertFieldHasType(updateOp,
                                          DocumentSourceChangeStream::kDocumentKeyField,
                                          BSONType::Object)
                           .getDocument();

    // Extract the UUID from resume token and do change stream lookups by UUID.
    auto resumeToken =
        ResumeToken::parse(updateOp[DocumentSourceChangeStream::kIdField].getDocument());

    const auto readConcern = pExpCtx->inMongos
        ? boost::optional<BSONObj>(BSON("level"
                                        << "majority"
                                        << "afterClusterTime" << resumeToken.getData().clusterTime))
        : boost::none;


    // Update lookup queries sent from mongoS to shards are allowed to use speculative majority
    // reads.
    const auto allowSpeculativeMajorityRead = pExpCtx->inMongos;
    invariant(resumeToken.getData().uuid);
    auto lookedUpDoc =
        pExpCtx->mongoProcessInterface->lookupSingleDocument(pExpCtx,
                                                             nss,
                                                             *resumeToken.getData().uuid,
                                                             documentKey,
                                                             readConcern,
                                                             allowSpeculativeMajorityRead);

    // Check whether the lookup returned any documents. Even if the lookup itself succeeded, it may
    // not have returned any results if the document was deleted in the time since the update op.
    return (lookedUpDoc ? Value(*lookedUpDoc) : Value(BSONNULL));
}

}  // namespace mongo
