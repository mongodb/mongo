// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace pattern_cmp {

/**
 * When we include a sort specification, that object should pass the checks in
 * this function.
 *
 * Checks include:
 *   1. The sort pattern cannot be empty.
 *   2. The value of each pattern element is 1 or -1.
 *   3. The sort field cannot be empty.
 *   4. If the sort field is a dotted field, it does not have any empty parts.
 */
Status checkSortClause(const BSONObj& sortObject);

}  // namespace pattern_cmp

// Extracts the value for 'pattern' for both 'lhs' and 'rhs' and return true if 'lhs' <
// 'rhs'. We expect that both 'lhs' and 'rhs' be key patterns.
class PatternElementCmp {
public:
    PatternElementCmp();

    PatternElementCmp(const BSONObj& pattern, const CollatorInterface* collator);

    bool operator()(const mutablebson::Element& lhs, const mutablebson::Element& rhs) const;

    BSONObj sortPattern;
    bool useWholeValue = true;
    const CollatorInterface* collator = nullptr;
};

class PatternValueCmp {
public:
    PatternValueCmp();

    PatternValueCmp(const BSONObj& pattern,
                    const BSONElement& originalElement,
                    const CollatorInterface* collator);

    bool operator()(const Value& lhs, const Value& rhs) const;

    // Extracts the BSON sort key for a single Value under this pattern. Returns a null-padded
    // key for non-object values. Use this to pre-compute keys before sorting to avoid the
    // O(N log N) extraction cost of calling operator() directly in a sort comparator.
    BSONObj extractSortKey(const Value& val) const;

    // Returns the original element passed into the PatternValueCmp constructor.
    BSONElement getOriginalElement() const;

    BSONObj sortPattern;
    bool useWholeValue = true;

    /**
     * We store the original element as an object so that we can call the copy() method on it.
     * This way, the PatternValueCmp class can have its own copy of the object.
     */
    BSONObj originalObj;
    const CollatorInterface* collator = nullptr;
};

}  // namespace mongo
