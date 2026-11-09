// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/field_values.h"

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"
#include "mongo/util/assert_util.h"

#include <algorithm>

namespace mongo::stats {

std::pair<std::vector<FieldValueTuple>, bool> extractFieldValues(const BSONObj& doc,
                                                                 std::span<const FieldPath> paths) {
    constexpr size_t kMaxTuplesPerDocument = 100'000;

    std::vector<FieldValueTuple> tuples(1);
    MultikeyComponents arrayComponents;
    for (const auto& path : paths) {
        BSONElementSet elements;
        multikey_dotted_path_support::extractAllElementsAlongPath(
            doc,
            path.fullPath(),
            elements,
            /*expandArrayOnTrailingField=*/true,
            &arrayComponents);

        const auto numValues = std::max<size_t>(elements.size(), 1);
        uassert(ErrorCodes::ExceededMemoryLimit,
                "Field value tuple count exceeds the per-document limit of 100000",
                tuples.size() <= kMaxTuplesPerDocument / numValues);

        std::vector<FieldValueTuple> expanded;
        expanded.reserve(tuples.size() * numValues);

        for (const auto& tuple : tuples) {
            if (elements.empty()) {
                expanded.push_back(tuple);
                expanded.back().push_back(sbe::value::TagValueView::nothing());
            }
            for (const auto& element : elements) {
                expanded.push_back(tuple);
                expanded.back().push_back(sbe::bson::convertToView(element));
            }
        }

        tuples = std::move(expanded);
    }
    return {std::move(tuples), !arrayComponents.empty()};
}

}  // namespace mongo::stats
