/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/ce/ce_common.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/path.h"

#include <algorithm>
#include <concepts>

#include <boost/optional/optional.hpp>

namespace mongo::ce {

namespace {
class SameSizeVectorBSONElementCmp {
public:
    bool operator()(const std::vector<BSONElement>& l, const std::vector<BSONElement>& r) const {
        // From below, we know these vector are all the same size.
        tassert(11214702, "Vectors must be the same size", l.size() == r.size());

        for (size_t i = 0; i < l.size(); i++) {
            auto cmp = l[i].woCompare(r[i], false, nullptr /* stringComparator */);
            if (cmp != 0) {
                return cmp < 0;
            }
        }
        return false;
    }
};
}  // namespace

BSONObj FieldPathAndEqSemantics::toBSON() const {
    return BSON("path" << path.fullPath() << "isExprEq" << isExprEq);
}


static const BSONObj kNullObj = BSON("" << BSONNULL);
static const BSONElement kNullElt = kNullObj.firstElement();

static const BSONObj kUndefinedObj = BSON("" << BSONUndefined);
static const BSONElement kUndefinedElt = kUndefinedObj.firstElement();

/**
 * Helper for transforming BSONObj instances by projecting down to a set of fields.
 * Provided callback is invoked exactly once, with a vector of projected BSONElements
 * corresponding to the provided fields.
 *
 * Does not traverse arrays.
 */
class NonArrayProjector {
public:
    NonArrayProjector(const std::vector<FieldPathAndEqSemantics>& fields) : fields(fields) {
        projectedFieldValues.reserve(fields.size());
    }

    /**
     * Invoke the callback with the projected version of the BSONObj.
     *
     * As arrays are not traversed, the callback will be invoked with a single vector of fields.
     */
    void operator()(const BSONObj& doc, std::invocable<std::vector<BSONElement>> auto&& callback) {
        projectedFieldValues.clear();
        for (const auto& field : fields) {
            // These "array behavior" settings ensure we stop and return any arrays we encounter.
            const ElementPath eltPath(field.path.fullPath(),
                                      ElementPath::LeafArrayBehavior::kNoTraversal,
                                      ElementPath::NonLeafArrayBehavior::kMatchSubpath);
            BSONElementIterator it(&eltPath, doc);
            tassert(
                11158501, "Should always find at least one element at path in document", it.more());

            const auto elt = it.next();
            tassert(11158502,
                    "Encountered unexpected array in NDV computation",
                    elt.element().type() != BSONType::array);
            if (elt.element().eoo() && !field.isExprEq) {
                // Use $eq equality semantics, which consider null & missing to be equal.
                projectedFieldValues.push_back(kNullElt);
            } else {
                // Use $expr equality semantics.
                projectedFieldValues.push_back(elt.element());
            }
        }

        tassert(11214701,
                "Unexpected number of fields in tuple",
                projectedFieldValues.size() == fields.size());
        callback(projectedFieldValues);
    }

    const std::vector<FieldPathAndEqSemantics>& fields;

    std::vector<BSONElement> projectedFieldValues;
};

/**
 * Helper for transforming BSONObj instances by projecting down to a set of fields, and
 * invoking a provided callback.
 *
 * Unwinds arrays. When an array-valued field is encountered, an instance per element
 * will be emitted.
 *
 * {a: 1, b:[1, 2, 3, 4, 5]} -> (1, 1), (1, 2), (1, 3), (1, 4), (1, 5)
 *
 * This is used to approximate the transformation a multikey index would apply.
 *
 * As multikey indexes only permit a single array-valued index key component for a given document,
 * it is asserted that any provided document meets this expectation.
 *
 * {a:1, b:[1, 2]} -> (1, 1), (1, 2) // Ok
 * {a:[1, 2], b:1} -> (1, 1), (2, 1) // Ok
 * {a:[1, 2], b:[1, 2]} -> XXX // Assertion failure
 */
class ArrayUnwindProjector {
public:
    ArrayUnwindProjector(const std::vector<FieldPathAndEqSemantics>& fields) : fields(fields) {
        fieldsInDoc.reserve(fields.size());
        iterators.reserve(fields.size());
        for (const auto& field : fields) {
            iterators.emplace_back(ElementPath{field.path.fullPath(),
                                               ElementPath::LeafArrayBehavior::kTraverseOmitArray,
                                               ElementPath::NonLeafArrayBehavior::kTraverse},
                                   BSONElementIterator());
        }
    }

    void operator()(const BSONObj& doc, std::invocable<std::vector<BSONElement>> auto&& callback) {
        fieldsInDoc.clear();

        for (auto& [path, iter] : iterators) {
            iter.reset(&path, doc);
        }

        // Exactly zero or one path may include an array if there is a multikey index over the
        // provided fields.
        // If an array is encountered, retain the index of the field/iterator required to "flatten"
        // the array into multiple keys.
        boost::optional<size_t> multiKeyFieldIndex;

        for (size_t idx = 0; idx < fields.size(); ++idx) {
            const auto& field = fields[idx];

            auto& it = iterators[idx].second;

            if (!it.more()) {
                // This document has an empty array at this path, so this iteration mode
                // (traversing arrays) reports no values. A multikey index represents this as an
                // undefined key.
                fieldsInDoc.push_back(kUndefinedElt);
                continue;
            }
            const auto elt = it.next();
            if (elt.element().eoo() && !field.isExprEq) {
                // Use $eq equality semantics, which consider null & missing to be equal.
                fieldsInDoc.push_back(kNullElt);
            } else {
                // Use $expr equality semantics.
                fieldsInDoc.push_back(elt.element());
            }

            if (it.more()) {
                // This element of the index is multikey. There can be only one for a given doc
                // in a given index.
                tassert(10061113,
                        "Parallel arrays are not supported; at most one index field may be "
                        "array-valued per document",
                        !multiKeyFieldIndex);
                multiKeyFieldIndex = idx;
            }
        }

        tassert(
            10061112, "Unexpected number of fields in tuple", fieldsInDoc.size() == fields.size());

        callback(fieldsInDoc);

        if (multiKeyFieldIndex) {
            auto idx = *multiKeyFieldIndex;
            auto& iter = iterators[idx].second;
            while (iter.more()) {
                fieldsInDoc[idx] = iter.next().element();
                callback(fieldsInDoc);
            }
        }
    }

    const std::vector<FieldPathAndEqSemantics>& fields;

    // Scratch space for accumulating projected fields, avoids reallocating this vector for each
    // document.
    std::vector<BSONElement> fieldsInDoc;
    // Pre-constructed iterators (and referenced paths) to avoid constructing for every document.
    std::vector<std::pair<ElementPath, BSONElementIterator>> iterators;
};

namespace filter {
/**
 * Check if the provided BSONElement fields fall within `bounds`.
 *
 * Caller must ensure order and number of fields/bounds match.
 *
 * e.g.,
 *
 *  auto filter = forBounds(oil);
 *  if (filter(fields)) {
 *      ...
 *  }
 */
auto forBounds(std::span<const OrderedIntervalList> bounds) {
    return [bounds](const std::vector<BSONElement>& fields) {
        for (size_t i = 0; i < fields.size(); ++i) {
            if (!matchesInterval(bounds[i], fields[i])) {
                return false;
            }
        }
        return true;
    };
}

/**
 * Filter accepting all provided values.
 */
bool any(const std::vector<BSONElement>&) {
    return true;
}
}  // namespace filter

// TODO SERVER-112198: Compute all NDVs in a single pass over the sample.
KeyCountResult countNDVInner(const std::vector<FieldPathAndEqSemantics>& fields,
                             const std::vector<BSONObj>& docs,
                             auto&& projector,
                             auto&& filter) {
    tassert(11214700, "Field names cannot be empty", !fields.empty());
    std::set<std::vector<BSONElement>, SameSizeVectorBSONElementCmp> distinctValues;
    size_t totalSampleKeys = 0;
    size_t uniqueMatchingKeys = 0;

    for (auto&& doc : docs) {
        projector(doc, [&](const std::vector<BSONElement>& projectedDoc) {
            // When array fields are encountered (if permitted by the projector)
            // each document may produce [1,N] keys, as with a multikey index.
            ++totalSampleKeys;
            if (filter(projectedDoc)) {
                ++uniqueMatchingKeys;
                distinctValues.insert(projectedDoc);
            }
        });
    }
    return {.totalSampleKeys = totalSampleKeys,
            .uniqueMatchingKeys = uniqueMatchingKeys,
            .sampleUniqueKeys = distinctValues.size()};
}

size_t countNDV(const std::vector<FieldPathAndEqSemantics>& fields,
                const std::vector<BSONObj>& docs,
                boost::optional<std::span<const OrderedIntervalList>> bounds) {
    if (bounds) {
        tassert(10061114,
                "Should have the same number of bounds as fields",
                fields.size() == bounds->size());
        return countNDVInner(fields, docs, NonArrayProjector(fields), filter::forBounds(*bounds))
            .sampleUniqueKeys;
    } else {
        return countNDVInner(fields, docs, NonArrayProjector(fields), filter::any).sampleUniqueKeys;
    }
}

KeyCountResult countNDVMultiKey(const std::vector<FieldPathAndEqSemantics>& fields,
                                const std::vector<BSONObj>& docs,
                                boost::optional<std::span<const OrderedIntervalList>> bounds) {
    if (bounds) {
        tassert(10061115,
                "Should have the same number of bounds as fields",
                fields.size() == bounds->size());
        return countNDVInner(
            fields, docs, ArrayUnwindProjector(fields), filter::forBounds(*bounds));
    } else {
        return countNDVInner(fields, docs, ArrayUnwindProjector(fields), filter::any);
    }
}

size_t countUniqueDocuments(const std::vector<BSONObj>& docs) {
    BSONElementSet uniqueIds;
    for (const auto& doc : docs) {
        uniqueIds.insert(doc["_id"]);
    }
    return uniqueIds.size();
}

bool matchesInterval(const Interval& interval, BSONElement val) {
    int startCmp = val.woCompare(interval.start, 0 /*ignoreFieldNames*/);
    int endCmp = val.woCompare(interval.end, 0 /*ignoreFieldNames*/);

    if (startCmp == 0) {
        /**
         * The document value is equal to the starting point of the interval; the document is inside
         * the bounds of this index interval if the starting point is included in the interval.
         */
        return interval.startInclusive;
    } else if (startCmp < 0 && endCmp < 0) {
        /**
         * The document value is less than both the starting point and the end point and is thus
         * not inside the bounds of this index interval. Depending on the index spec and the
         * direction the index is traversed, endCmp can be < startCmp which is why it's necesary to
         * check both interval end points.
         */
        return false;
    }

    if (endCmp == 0) {
        /**
         * The document value is equal to the end point of the interval; the document is inside the
         * bounds of this index interval if the end point is included in the interval.
         */
        return interval.endInclusive;
    } else if (endCmp > 0 && startCmp > 0) {
        /**
         * The document value is greater than both the starting point and the end point and is thus
         * not inside the bounds of this index interval. Depending on the index spec and the
         * direction the index is traversed, startCmp can be < endCmp which is why it's necesary to
         * check both interval end points.
         */
        return false;
    }
    return true;
}

bool matchesInterval(const OrderedIntervalList& oil, BSONElement val) {
    if (oil.intervals.empty()) {
        return false;
    }

    const Interval::Direction direction = oil.computeDirection();

    // Binary search for the first interval val is not strictly AHEAD of.
    auto it =
        std::lower_bound(oil.intervals.begin(),
                         oil.intervals.end(),
                         std::make_pair(val, direction),
                         [](const Interval& interval,
                            const std::pair<BSONElement, Interval::Direction>& valAndDirection) {
                             return IndexBoundsChecker::intervalCmp(
                                        interval, valAndDirection.first, valAndDirection.second) ==
                                 IndexBoundsChecker::AHEAD;
                         });

    if (it == oil.intervals.end()) {
        return false;
    }
    return IndexBoundsChecker::intervalCmp(*it, val, direction) == IndexBoundsChecker::WITHIN;
}

}  // namespace mongo::ce
