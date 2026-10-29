// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_limit.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceLimit::DocumentSourceLimit(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                         long long limit)
    : DocumentSource(kStageName, pExpCtx), _limit(limit) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(limit, LimitLiteParsed::parse, AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(limit, DocumentSourceLimit, LimitStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(limit, DocumentSourceLimit::id);

constexpr std::string_view DocumentSourceLimit::kStageName;

DocumentSourceContainer::iterator DocumentSourceLimit::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282987, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

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

Value DocumentSourceLimit::serialize(const query_shape::SerializationOptions& opts) const {
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
