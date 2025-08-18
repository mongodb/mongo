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
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/utility/in_place_factory.hpp>

namespace mongo {

using boost::intrusive_ptr;
using std::vector;

DocumentSourceRedact::DocumentSourceRedact(const intrusive_ptr<ExpressionContext>& expCtx,
                                           const intrusive_ptr<Expression>& expression,
                                           Variables::Id currentId)
    : DocumentSource(kStageName, expCtx),
      _redactProcessor(std::make_shared<RedactProcessor>(expCtx, expression, currentId)) {}

REGISTER_DOCUMENT_SOURCE(redact,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceRedact::createFromBson,
                         AllowedWithApiStrict::kAlways);
ALLOCATE_DOCUMENT_SOURCE_ID(redact, DocumentSourceRedact::id)

const char* DocumentSourceRedact::getSourceName() const {
    return kStageName.data();
}

static const Value descendVal = Value("descend"_sd);
static const Value pruneVal = Value("prune"_sd);
static const Value keepVal = Value("keep"_sd);

DocumentSourceContainer::iterator DocumentSourceRedact::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);

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

Value DocumentSourceRedact::serialize(const SerializationOptions& opts) const {
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
