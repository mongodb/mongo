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

#include "mongo/db/pipeline/document_source_search_vector.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/str.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceSearchVector::DocumentSourceSearchVector(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                         long long k,
                                         const std::string& lookupFieldName,
                                         const std::vector<double>& lookupVector)
    : DocumentSource(kStageName, pExpCtx), _k(k), _lookupFieldName(lookupFieldName), _lookupVector(lookupVector) {}

REGISTER_DOCUMENT_SOURCE(searchVector,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSearchVector::createFromBson,
                         AllowedWithApiStrict::kAlways);

constexpr StringData DocumentSourceSearchVector::kStageName;

Pipeline::SourceContainer::iterator DocumentSourceSearchVector::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }
    return std::next(itr);
}

DocumentSource::GetNextResult DocumentSourceSearchVector::doGetNext() {
    // TODO
    return pSource->getNext();
}

Value DocumentSourceSearchVector::serialize(boost::optional<ExplainOptions::Verbosity> explain) const {
    // TODO
    return Value();
}

intrusive_ptr<DocumentSourceSearchVector> DocumentSourceSearchVector::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx,
    long long k,
    const std::string& lookupFieldName,
    const std::vector<double>& lookupVector) {
    uassert(/*TODO*/ ErrorCodes::TypeMismatch, "k must be greater than 0", k > 0);
    uassert(/*TODO*/ ErrorCodes::TypeMismatch, "the vector should have at least one dimension", lookupVector.size() > 0);
    intrusive_ptr<DocumentSourceSearchVector> source(new DocumentSourceSearchVector(pExpCtx, k, lookupFieldName, lookupVector));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceSearchVector::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::TypeMismatch,
        "$searchVector must be an object",
        elem.type() == BSONType::Object);

    const auto& obj = elem.Obj();
    const auto k = obj["k"].parseIntegerElementToNonNegativeLong();
    uassert(ErrorCodes::TypeMismatch,
        str::stream() << "invalid argument to $searchVector stage: " << k.getStatus().reason(),
        k.isOK());
    const auto fieldName = obj["field"].String();

    uassert(ErrorCodes::TypeMismatch, "vector must be defined",  obj["vector"].type() == BSONType::Array);
    const auto vector = obj["vector"].Array();
    uassert(ErrorCodes::TypeMismatch,
        str::stream() << "invalid argument to $searchVector stage: vector must have at least one dimension",
        !vector.empty());

    std::vector<double> inputVector(vector.size());
    for (const auto& vectorVal : vector) {
        uassert(ErrorCodes::TypeMismatch, "vector must contain doubles", vectorVal.type() == BSONType::NumberDouble);
        inputVector.push_back(vectorVal.Double());
    }

    return DocumentSourceSearchVector::create(pExpCtx, k.getValue(), fieldName, inputVector);
}
}  // namespace mongo
