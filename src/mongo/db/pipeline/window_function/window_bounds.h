/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#pragma once

namespace mongo {

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
    static constexpr StringData kArgDocuments = "documents"_sd;
    static constexpr StringData kArgRange = "range"_sd;
    static constexpr StringData kArgUnit = "unit"_sd;

    static constexpr StringData kValUnbounded = "unbounded"_sd;
    static constexpr StringData kValCurrent = "current"_sd;

    struct Unbounded {};
    struct Current {};
    template <class T>
    using Bound = stdx::variant<Unbounded, Current, T>;

    struct DocumentBased {
        Bound<int> lower;
        Bound<int> upper;
    };
    struct RangeBased {
        Bound<Value> lower;
        Bound<Value> upper;
        boost::optional<TimeUnit> unit;
    };

    stdx::variant<DocumentBased, RangeBased> bounds;

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
    static WindowBounds parse(BSONObj args,
                              const boost::optional<SortPattern>& sortBy,
                              ExpressionContext* expCtx);

    void serialize(MutableDocument& args) const;
};

}  // namespace mongo
