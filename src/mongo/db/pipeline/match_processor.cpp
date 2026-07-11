// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_path_support.h"

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
    // MatchExpression only takes BSON documents, so we have to make one. As an optimization,
    // only serialize the fields we need to do the match. Specify BSONObj::LargeSizeTrait so
    // that matching against a large document mid-pipeline does not throw a BSON max-size error.
    BSONObj toMatch = [&]() {
        if (_dependencies.needWholeDocument) {
            return input.toBson<BSONObj::LargeSizeTrait>();
        }
        if (_dependenciesHaveUniqueFirstFields) {
            // Use optimized function that does not check whether we have already seen a specific
            // first field.
            return document_path_support::documentToBsonWithPaths<
                BSONObj::LargeSizeTrait,
                /* PathsHaveUniqueFirstFields */ true>(input, _dependencies.fields);
        }

        // Use slow function that will check for first field uniqueness.
        return document_path_support::documentToBsonWithPaths<
            BSONObj::LargeSizeTrait,
            /* PathsHaveUniqueFirstFields */ false>(input, _dependencies.fields);
    }();
    return exec::matcher::matchesBSON(_expression.get(), toMatch, /*details*/ nullptr, ctx);
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
