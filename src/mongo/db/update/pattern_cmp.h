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

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/query/collation/collator_interface.h"

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
