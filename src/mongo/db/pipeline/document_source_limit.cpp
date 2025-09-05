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

#include "mongo/db/pipeline/document_source_limit.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceLimit::DocumentSourceLimit(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                         long long limit)
    : DocumentSource(kStageName, pExpCtx), _limit(limit) {}

REGISTER_DOCUMENT_SOURCE(limit,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceLimit::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(limit, DocumentSourceLimit::id)

constexpr StringData DocumentSourceLimit::kStageName;

DocumentSourceContainer::iterator DocumentSourceLimit::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextLimit) {
        _limit = std::min(_limit, nextLimit->getLimit());
        container->erase(std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

Value DocumentSourceLimit::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), opts.serializeLiteral(_limit)}});
}

intrusive_ptr<DocumentSourceLimit> DocumentSourceLimit::create(
    const intrusive_ptr<ExpressionContext>& pExpCtx, long long limit) {
    uassert(15958, "the limit must be positive", limit > 0);
    intrusive_ptr<DocumentSourceLimit> source(new DocumentSourceLimit(pExpCtx, limit));
    return source;
}

intrusive_ptr<DocumentSource> DocumentSourceLimit::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    const auto limit = elem.parseIntegerElementToNonNegativeLong();
    uassert(5107201,
            str::stream() << "invalid argument to $limit stage: " << limit.getStatus().reason(),
            limit.isOK());
    return DocumentSourceLimit::create(pExpCtx, limit.getValue());
}
}  // namespace mongo
