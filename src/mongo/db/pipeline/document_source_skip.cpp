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

#include "mongo/db/pipeline/document_source_skip.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <iterator>
#include <limits>
#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                       long long nToSkip)
    : DocumentSource(kStageName, pExpCtx), _nToSkip(nToSkip) {}

REGISTER_DOCUMENT_SOURCE(skip,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceSkip::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(skip, DocumentSourceSkip::id)

constexpr StringData DocumentSourceSkip::kStageName;

Value DocumentSourceSkip::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << opts.serializeLiteral(_nToSkip)));
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::optimize() {
    return _nToSkip == 0 ? nullptr : this;
}

DocumentSourceContainer::iterator DocumentSourceSkip::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
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
