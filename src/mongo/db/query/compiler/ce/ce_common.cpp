// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/ce/ce_common.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/dotted_path/dotted_path_support.h"
#include "mongo/db/matcher/path.h"

#include <algorithm>
#include <concepts>
#include <string>
#include <vector>

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
 * it is asserted that any provided document meets this expectation. For instance, for input fields
 * of {a, b}:
 *
 * {a:1, b:[1, 2]} -> (1, 1), (1, 2) // Ok
 * {a:[1, 2], b:1} -> (1, 1), (2, 1) // Ok
 * {a:[1, 2], b:[1, 2]} -> XXX // Assertion failure
 *
 * For dotted paths that share an array-valued prefix (e.g. 'a' and 'a.b' when 'a' is an array),
 * the behavior matches BtreeKeyGenerator: each outer array element is visited once, and all field
 * paths are resolved relative to that element. Under regular $eq semantics, scalar elements produce
 * null for any remaining sub-path (e.g. element '1' yields null for 'a.b'); under $expr $eq the
 * remaining sub-path stays missing (EOO). For example, for input fields {a, a.b}:
 *
 * {a:[{b:0}, {b:0}, 1]} -> ({b:0}, 0), ({b:0}, 0), (1, null)
 *
 * Here 'a' and 'a.b' both resolve against the same outer array. The scalar element '1' has no
 * embedded 'b', so 'a.b' yields null for it. This is *not* a parallel-array case; the assertion
 * only fires when two fields resolve to two genuinely distinct arrays.
 */
class ArrayUnwindProjector {
public:
    ArrayUnwindProjector(const std::vector<FieldPathAndEqSemantics>& fields) : _fields(fields) {}

    void operator()(const BSONObj& doc, std::invocable<std::vector<BSONElement>> auto&& callback) {
        // Seed the recursion with the full, still-unresolved paths and an all-null value tuple.
        std::vector<std::string_view> paths(_fields.size());
        for (size_t i = 0; i < _fields.size(); ++i) {
            paths[i] = _fields[i].path.fullPath();
        }
        _processObj(
            doc, std::move(paths), std::vector<BSONElement>(_fields.size(), kNullElt), callback);
    }

private:
    // Recursively projects 'obj' down to one key tuple per array-element combination, mirroring
    // BtreeKeyGenerator::_getKeysWithArray + _getKeysArrEltFixed.
    //
    // Invariant: for each field 'i', 'paths[i]' is the portion of the path still to be resolved
    // (empty once fully resolved) and 'values[i]' is its value resolved so far. The recursion
    // resolves fields against 'obj' until the first array is hit, then unwinds that array,
    // resolving any remaining path suffixes against each element in a recursive call.
    void _processObj(const BSONObj& obj,
                     std::vector<std::string_view> paths,
                     std::vector<BSONElement> values,
                     std::invocable<std::vector<BSONElement>> auto&& callback) const {
        // Phase 1: resolve each unresolved field against 'obj', stopping at the first array. Fields
        // that hit the same array are correlated and unwound together below.
        BSONElement arrElt;
        std::vector<size_t> arrIdxs;

        for (size_t i = 0; i < _fields.size(); ++i) {
            // An empty path indicates it's fully resolved.
            if (paths[i].empty()) {
                continue;
            }

            // extractElementAtOrArrayAlongDottedPath advances 'p' past the consumed prefix and
            // stops at the first array along the path (if any).
            const char* p = paths[i].data();
            BSONElement elt = bson::extractElementAtOrArrayAlongDottedPath(obj, p);

            if (elt.eoo()) {
                // Path is missing. Under $eq semantics null == missing; under $expr keep the EOO.
                values[i] = _fields[i].isExprEq ? elt : kNullElt;
                paths[i] = {};
            } else if (elt.type() == BSONType::array) {
                // The unwind point. Only one distinct array may be indexed per document, so any
                // other field reaching a *different* array is a genuine parallel-array case.
                uassert(10061118,
                        "Parallel arrays are not supported",
                        arrElt.eoo() || elt.rawdata() == arrElt.rawdata());
                arrElt = elt;
                paths[i] = std::string_view{p};  // remaining suffix after the array
                arrIdxs.push_back(i);
            } else {
                // Scalar (or subobject) leaf value; the field is fully resolved.
                values[i] = elt;
                paths[i] = {};
            }
        }

        // Phase 2 (base case): no array was found, so every field is resolved -- emit one tuple.
        if (arrElt.eoo()) {
            callback(values);
            return;
        }

        // Phase 3: unwind the shared array. For each element, pin terminal fields (those whose
        // remaining path ended at the array) to the element value, then recurse into the element's
        // embedded object to resolve any deeper suffixes. Non-object elements recurse into an empty
        // object, so their remaining suffixes resolve to null.
        auto processElement = [&](const BSONElement& elem) {
            auto pathsCopy = paths;
            auto valuesCopy = values;
            for (size_t i : arrIdxs) {
                if (pathsCopy[i].empty()) {
                    valuesCopy[i] = elem;
                }
            }
            BSONObj sub = elem.type() == BSONType::object ? elem.embeddedObject() : BSONObj{};
            _processObj(sub, std::move(pathsCopy), std::move(valuesCopy), callback);
        };

        BSONObj arrObj = arrElt.embeddedObject();
        if (arrObj.isEmpty()) {
            // Empty array: emit one key with undefined for terminal fields (matching btree
            // behavior).
            processElement(kUndefinedElt);
        } else {
            for (const auto& elem : arrObj) {
                processElement(elem);
            }
        }
    }

    const std::vector<FieldPathAndEqSemantics>& _fields;
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
