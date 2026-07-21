// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/pipeline/document_path_support.h"

#include <string_view>

namespace mongo {
namespace {
// Upper bound on the document size (in bytes) for which the 'match against the whole trivially
// convertible document' fast path (see MatchProcessor::process) is used. Bounds the cost of the
// matcher's linear field scan over fields the predicate does not need.
constexpr int kWholeDocumentMatchMaxSizeBytes = 16 * 1024;
}  // namespace

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
        // Fast path: if the document is already backed by owned BSON (e.g. a document materialized
        // by a SequentialDocumentCache), matching against the whole document avoids rebuilding a
        // projected BSON. This is a win when the same document is matched repeatedly (a nested-loop
        // $lookup re-scanning its cached prefix). Correctness is unchanged -- the matcher only
        // reads the paths it needs -- and the size gate bounds the extra field-scan cost for wide
        // docs.
        if (auto whole = input.toBsonIfTriviallyConvertible();
            whole && whole->objsize() <= kWholeDocumentMatchMaxSizeBytes) {
            return std::move(*whole);
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
