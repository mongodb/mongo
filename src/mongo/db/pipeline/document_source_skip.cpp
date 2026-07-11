// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

DocumentSourceSkip::DocumentSourceSkip(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                       long long nToSkip)
    : DocumentSource(kStageName, pExpCtx), _nToSkip(nToSkip) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(skip, SkipLiteParsed::parse, AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(skip, DocumentSourceSkip, SkipStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(skip, DocumentSourceSkip::id)

constexpr std::string_view DocumentSourceSkip::kStageName;

Value DocumentSourceSkip::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << opts.serializeLiteral(_nToSkip)));
}

intrusive_ptr<DocumentSource> DocumentSourceSkip::optimize() {
    return _nToSkip == 0 ? nullptr : this;
}

DocumentSourceContainer::iterator DocumentSourceSkip::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282962, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

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
