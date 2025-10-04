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

#include "mongo/db/exec/document_value/document_value_test_util.h"

#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/unittest/unittest.h"

#include <ostream>

namespace mongo {
namespace unittest {

#define _GENERATE_DOCVAL_CMP_FUNC(DOCVAL, NAME, COMPARATOR, OPERATOR)                \
    void assertComparison_##DOCVAL##NAME(const std::string& theFile,                 \
                                         unsigned theLine,                           \
                                         StringData aExpression,                     \
                                         StringData bExpression,                     \
                                         const DOCVAL& aValue,                       \
                                         const DOCVAL& bValue) {                     \
        if (!COMPARATOR().evaluate(aValue OPERATOR bValue)) {                        \
            std::ostringstream os;                                                   \
            os << "Expected [ " << aExpression << " " #OPERATOR " " << bExpression   \
               << " ] but found [ " << aValue << " " #OPERATOR " " << bValue << "]"; \
            TestAssertionFailure(theFile, theLine, os.str()).stream();               \
        }                                                                            \
    }

_GENERATE_DOCVAL_CMP_FUNC(Value, EQ, ValueComparator, ==);
_GENERATE_DOCVAL_CMP_FUNC(Value, LT, ValueComparator, <);
_GENERATE_DOCVAL_CMP_FUNC(Value, LTE, ValueComparator, <=);
_GENERATE_DOCVAL_CMP_FUNC(Value, GT, ValueComparator, >);
_GENERATE_DOCVAL_CMP_FUNC(Value, GTE, ValueComparator, >=);
_GENERATE_DOCVAL_CMP_FUNC(Value, NE, ValueComparator, !=);

_GENERATE_DOCVAL_CMP_FUNC(Document, EQ, DocumentComparator, ==);
_GENERATE_DOCVAL_CMP_FUNC(Document, LT, DocumentComparator, <);
_GENERATE_DOCVAL_CMP_FUNC(Document, LTE, DocumentComparator, <=);
_GENERATE_DOCVAL_CMP_FUNC(Document, GT, DocumentComparator, >);
_GENERATE_DOCVAL_CMP_FUNC(Document, GTE, DocumentComparator, >=);
_GENERATE_DOCVAL_CMP_FUNC(Document, NE, DocumentComparator, !=);
#undef _GENERATE_DOCVAL_CMP_FUNC

}  // namespace unittest
}  // namespace mongo
