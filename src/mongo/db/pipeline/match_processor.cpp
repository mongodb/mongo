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

#include "mongo/db/pipeline/match_processor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_path_support.h"

namespace mongo {

MatchProcessor::MatchProcessor(std::unique_ptr<MatchExpression> expr,
                               DepsTracker dependencies,
                               BSONObj&& predicate)
    : _expression(std::move(expr)),
      _dependencies(std::move(dependencies)),
      _hasUniquePrefixes(_dependencies.fields.size() < 2 ||
                         hasUniquePrefixes(_dependencies.fields)),
      _predicate(std::move(predicate)) {
    tassert(10422701, "expecting 'predicate' to be owned", _predicate.isOwned());
}

bool MatchProcessor::process(const Document& input) const {
    // MatchExpression only takes BSON documents, so we have to make one. As an optimization,
    // only serialize the fields we need to do the match. Specify BSONObj::LargeSizeTrait so
    // that matching against a large document mid-pipeline does not throw a BSON max-size error.
    BSONObj toMatch = [&]() {
        if (_dependencies.needWholeDocument) {
            return input.toBson<BSONObj::LargeSizeTrait>();
        }
        if (_hasUniquePrefixes) {
            // Use optimized function that does not check whether we have already seen a specific
            // prefix.
            return document_path_support::documentToBsonWithPaths<BSONObj::LargeSizeTrait,
                                                                  /* EnsureUniquePrefixes */ false>(
                input, _dependencies.fields);
        }

        // Use slow function that will check for prefix uniqueness.
        return document_path_support::documentToBsonWithPaths<BSONObj::LargeSizeTrait,
                                                              /* EnsureUniquePrefixes */ true>(
            input, _dependencies.fields);
    }();
    return exec::matcher::matchesBSON(_expression.get(), toMatch);
}

bool MatchProcessor::hasUniquePrefixes(const OrderedPathSet& fields) {
    StringDataSet prefixes;
    for (auto&& path : fields) {
        auto prefix = FieldPath::extractFirstFieldFromDottedPath(path);
        if (!prefixes.insert(prefix).second) {
            return false;
        }
    }
    return true;
}

}  // namespace mongo
