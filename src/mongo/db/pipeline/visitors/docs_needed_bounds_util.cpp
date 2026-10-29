// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/visitors/docs_needed_bounds_util.h"

#include "mongo/util/overloaded_visitor.h"

namespace mongo::docs_needed_bounds {
boost::optional<long long> calcExtractableLimit(DocsNeededBounds docsNeededBounds) {
    return visit(OverloadedVisitor{
                     [](long long minVal, long long maxVal) -> boost::optional<long long> {
                         if (minVal == maxVal) {
                             // Both the min and the max must be the same value for
                             // the limit of the query to be extractable.
                             return minVal;
                         } else {
                             return boost::none;
                         }
                     },
                     [](auto& minVal, auto& maxVal) -> boost::optional<long long> {
                         // Catch all - unless both min and max are articulated long long values
                         // the query has no extractable limit.
                         return boost::none;
                     },
                 },
                 docsNeededBounds.getMinBounds(),
                 docsNeededBounds.getMaxBounds());
}
};  // namespace mongo::docs_needed_bounds
