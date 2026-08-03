// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/matcher/matcher.h"

#include <string_view>

namespace mongo {

MatchProcessor::MatchProcessor(std::unique_ptr<MatchExpression> expr,
                               DepsTracker dependencies,
                               BSONObj&& predicate)
    : _expression(std::move(expr)),
      _dependencies(std::move(dependencies)),
      _dependenciesHaveUniqueFirstFields(_dependencies.fields.size() < 2 ||
                                         dependenciesHaveUniqueFirstFields(_dependencies.fields)),
      _predicate(std::move(predicate)) {
    tassert(10422701, "expecting 'predicate' to be owned", _predicate.isOwned());
}

bool MatchProcessor::process(const Document& input, const EvaluationContext& ctx) const {
    DocumentMatchableDocument toMatch(input, _dependencies, _dependenciesHaveUniqueFirstFields);
    return exec::matcher::matches(_expression.get(), &toMatch, /*details*/ nullptr, ctx);
}

bool MatchProcessor::dependenciesHaveUniqueFirstFields(const OrderedPathSet& paths) {
    boost::optional<std::string_view> prevFirstField = boost::none;
    for (auto&& path : paths) {
        auto firstField = FieldPath::extractFirstFieldFromDottedPath(path);
        if (prevFirstField == firstField) {
            return false;
        }
        prevFirstField = firstField;
    }
    return true;
}

}  // namespace mongo
