// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Window bounds describe a set of documents based on the current document.
 *
 * Document-based bounds select documents by their position in the input:
 *
 *     documents: [-2, +4]
 *     documents: [-2, 0]
 *
 * Range-based bounds select documents by the value of the sortBy field:
 *
 *     range: [-0.3, +2.4]
 *     range: [-0.3, +2.4], unit: 'second'
 *
 * In either case, the lower and upper bound can each be 'unbounded', or 'current':
 *
 *     documents: ['unbounded', +4]
 *     range: ['unbounded', 'current']
 *
 * Note that bounds do not necessarily include the current document:
 *
 *     documents: ['unbounded', -2]
 *     documents: [+100, +100]
 *
 *     range: [-3, -1]
 *     range: [-3, -1], unit: 'day'
 */
struct WindowBounds {
    static constexpr std::string_view kArgDocuments = "documents"sv;
    static constexpr std::string_view kArgRange = "range"sv;
    static constexpr std::string_view kArgUnit = "unit"sv;

    static constexpr std::string_view kValUnbounded = "unbounded"sv;
    static constexpr std::string_view kValCurrent = "current"sv;

    struct Unbounded {};
    struct Current {};
    template <class T>
    using Bound = std::variant<Unbounded, Current, T>;

    struct DocumentBased {
        Bound<int> lower;
        Bound<int> upper;
    };
    struct RangeBased {
        Bound<Value> lower;
        Bound<Value> upper;
        boost::optional<TimeUnit> unit;
    };

    std::variant<DocumentBased, RangeBased> bounds;

    static WindowBounds defaultBounds() {
        return WindowBounds{DocumentBased{Unbounded{}, Unbounded{}}};
    }

    static WindowBounds documentBounds(int lower, int upper) {
        return WindowBounds{DocumentBased{lower, upper}};
    }

    /**
     * Checks whether these bounds are unbounded on both ends.
     * This case is special because it means you don't need a sortBy to interpret the bounds:
     * the bounds include every document (in the current partition).
     */
    bool isUnbounded() const;

    /**
     * Parses bounds from the arguments object of a window-function expression.
     * For example, in:
     *
     *     {$setWindowFields: {
     *         output: {
     *             v: {$sum: "$x", window: {range: [-1, +1], unit: 'seconds'}},
     *         }
     *     }}
     *
     * 'args' would be {range: [-1, +1], unit: 'seconds'}.
     *
     * If the BSON doesn't specify bounds, we default to:
     *
     *     documents: ['unbounded', 'unbounded']
     *
     * Some combinations of bounds and sortBy are invalid: for example, sortBy: {a: 1, b: 1}
     * doesn't make sense with time-based bounds. The 'sortBy' argument lets us check these
     * constraints during parsing.
     */
    static WindowBounds parse(BSONElement args,
                              const boost::optional<SortPattern>& sortBy,
                              ExpressionContext* expCtx);

    void serialize(MutableDocument& args, const query_shape::SerializationOptions& opts) const;
};

}  // namespace mongo
