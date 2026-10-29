// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_redact.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <iterator>
#include <list>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/utility/in_place_factory.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;
using std::vector;

DocumentSourceRedact::DocumentSourceRedact(const intrusive_ptr<ExpressionContext>& expCtx,
                                           const intrusive_ptr<Expression>& expression,
                                           Variables::Id currentId)
    : DocumentSource(kStageName, expCtx),
      _redactProcessor(std::make_shared<RedactProcessor>(expCtx, expression, currentId)) {}

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(redact,
                                     RedactLiteParsed::parse,
                                     AllowedWithApiStrict::kAlways);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(redact, DocumentSourceRedact, RedactStageParams);

ALLOCATE_DOCUMENT_SOURCE_ID(redact, DocumentSourceRedact::id)

std::string_view DocumentSourceRedact::getSourceName() const {
    return kStageName;
}

static const Value descendVal = Value("descend"sv);
static const Value pruneVal = Value("prune"sv);
static const Value keepVal = Value("keep"sv);

DocumentSourceContainer::iterator DocumentSourceRedact::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(11282971, "Expecting DocumentSource iterator pointing to this stage", *itr == this);

    if (std::next(itr) == container->end()) {
        return container->end();
    }

    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());

    if (nextMatch) {
        const BSONObj redactSafePortion = nextMatch->redactSafePortion();

        if (!redactSafePortion.isEmpty()) {
            // Because R-M turns into M-R-M without modifying the original $match, we cannot step
            // backwards and optimize from before the $redact, otherwise this will just loop and
            // create an infinite number of $matches.
            DocumentSourceContainer::iterator returnItr = std::next(itr);

            container->insert(itr, DocumentSourceMatch::create(redactSafePortion, getExpCtx()));

            return returnItr;
        }
    }
    return std::next(itr);
}

intrusive_ptr<DocumentSource> DocumentSourceRedact::optimize() {
    _redactProcessor->setExpression(_redactProcessor->getExpression()->optimize());
    return this;
}

Value DocumentSourceRedact::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << _redactProcessor->getExpression().get()->serialize(opts)));
}

intrusive_ptr<DocumentSource> DocumentSourceRedact::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    VariablesParseState vps = expCtx->variablesParseState;
    Variables::Id currentId = vps.defineVariable("CURRENT");  // will differ from ROOT
    Variables::Id decendId = vps.defineVariable("DESCEND");
    Variables::Id pruneId = vps.defineVariable("PRUNE");
    Variables::Id keepId = vps.defineVariable("KEEP");
    intrusive_ptr<Expression> expression = Expression::parseOperand(expCtx.get(), elem, vps);
    intrusive_ptr<DocumentSourceRedact> source =
        new DocumentSourceRedact(expCtx, expression, currentId);

    // TODO figure out how much of this belongs in constructor and how much here.
    // Set up variables. Never need to reset DESCEND, PRUNE, or KEEP.
    auto& variables = expCtx->variables;
    variables.setValue(decendId, descendVal);
    variables.setValue(pruneId, pruneVal);
    variables.setValue(keepId, keepVal);

    return source;
}
}  // namespace mongo
