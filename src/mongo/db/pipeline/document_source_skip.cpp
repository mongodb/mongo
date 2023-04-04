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

#include "mongo/db/pipeline/document_source_skip.h"

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/util/str.h"

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                       long long nToSkip)
    : DocumentSource(kStageName, pExpCtx), _nToSkip(nToSkip) {}

REGISTER_DOCUMENT_SOURCE(skip,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSkip::createFromBson,
                         AllowedWithApiStrict::kAlways);

constexpr StringData DocumentSourceSkip::kStageName;

DocumentSource::GetNextResult DocumentSourceSkip::doGetNext() {
    while (_nSkippedSoFar < _nToSkip) {
        // For performance reasons, a streaming stage must not keep references to documents across
        // calls to getNext(). Such stages must retrieve a result from their child and then release
        // it (or return it) before asking for another result. Failing to do so can result in extra
        // work, since the Document/Value library must copy data on write when that data has a
        // refcount above one.
        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }
        ++_nSkippedSoFar;
    }

    return pSource->getNext();
}

Value DocumentSourceSkip::serialize(SerializationOptions opts) const {
    if (opts.redactIdentifiers || opts.replacementForLiteralArgs) {
        MONGO_UNIMPLEMENTED_TASSERT(7484311);
    }

    return Value(DOC(getSourceName() << _nToSkip));
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::optimize() {
    return _nToSkip == 0 ? nullptr : this;
}

Pipeline::SourceContainer::iterator DocumentSourceSkip::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextSkip = dynamic_cast<DocumentSourceSkip*>((*std::next(itr)).get());
    if (nextSkip) {
        // '_nToSkip' can potentially overflow causing it to become negative and skip nothing.
        if (std::numeric_limits<long long>::max() - _nToSkip - nextSkip->getSkip() >= 0) {
            _nToSkip += nextSkip->getSkip();
            container->erase(std::next(itr));
            return itr;
        }
    }
    return std::next(itr);
}

intrusive_ptr<DocumentSourceSkip> DocumentSourceSkip::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx, long long nToSkip) {
    uassert(15956, "Argument to $skip cannot be negative", nToSkip >= 0);
    intrusive_ptr<DocumentSourceSkip> skip(new DocumentSourceSkip(pExpCtx, nToSkip));
    return skip;
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    const auto nToSkip = elem.parseIntegerElementToNonNegativeLong();
    uassert(5107200,
            str::stream() << "invalid argument to $skip stage: " << nToSkip.getStatus().reason(),
            nToSkip.isOK());
    return DocumentSourceSkip::create(pExpCtx, nToSkip.getValue());
}
}  // namespace mongo
