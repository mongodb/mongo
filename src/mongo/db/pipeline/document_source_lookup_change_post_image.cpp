/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
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
                          << "\" field to have type "
                          << typeName(expectedType)
                          << ", instead found type "
                          << typeName(val.getType())
                          << ": "
                          << val.toString()
                          << ", full object: "
                          << fullDoc.toString(),
            val.getType() == expectedType);
    return val;
}
}  // namespace

DocumentSource::GetNextResult DocumentSourceLookupChangePostImage::getNext() {
    pExpCtx->checkForInterrupt();

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

NamespaceString DocumentSourceLookupChangePostImage::assertNamespaceMatches(
    const Document& inputDoc) const {
    auto namespaceObject =
        assertFieldHasType(inputDoc, DocumentSourceChangeStream::kNamespaceField, BSONType::Object)
            .getDocument();
    auto dbName = assertFieldHasType(namespaceObject, "db"_sd, BSONType::String);
    auto collectionName = assertFieldHasType(namespaceObject, "coll"_sd, BSONType::String);
    NamespaceString nss(dbName.getString(), collectionName.getString());
    uassert(40579,
            str::stream() << "unexpected namespace during post image lookup: " << nss.ns()
                          << ", expected "
                          << pExpCtx->ns.ns(),
            nss == pExpCtx->ns);
    return nss;
}

Value DocumentSourceLookupChangePostImage::lookupPostImage(const Document& updateOp) const {
    // Make sure we have a well-formed input.
    auto nss = assertNamespaceMatches(updateOp);

    auto documentKey = assertFieldHasType(
        updateOp, DocumentSourceChangeStream::kDocumentKeyField, BSONType::Object);
    auto matchSpec = BSON("$match" << documentKey);

    // TODO SERVER-29134 we need to extract the namespace from the document and set them on the new
    // ExpressionContext if we're getting notifications from an entire database.
    auto foreignExpCtx = pExpCtx->copyWith(nss);
    auto pipeline = uassertStatusOK(_mongod->makePipeline({matchSpec}, foreignExpCtx));

    if (auto first = pipeline->getNext()) {
        auto lookedUpDocument = Value(*first);
        if (auto next = pipeline->getNext()) {
            uasserted(40580,
                      str::stream() << "found more than document with documentKey "
                                    << documentKey.toString()
                                    << " while looking up post image after change: ["
                                    << lookedUpDocument.toString()
                                    << ", "
                                    << next->toString()
                                    << "]");
        }
        return lookedUpDocument;
    }
    // We couldn't find it with the documentKey, it may have been deleted.
    return Value(BSONNULL);
}

}  // namespace mongo
