// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document_value_test_util.h"

#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/unittest/unittest.h"

#include <string_view>


namespace mongo {
namespace unittest {

#define _GENERATE_DOCVAL_CMP_FUNC(DOCVAL, NAME, COMPARATOR, OPERATOR)                \
    void assertComparison_##DOCVAL##NAME(const std::string& theFile,                 \
                                         unsigned theLine,                           \
                                         std::string_view aExpression,               \
                                         std::string_view bExpression,               \
                                         const DOCVAL& aValue,                       \
                                         const DOCVAL& bValue) {                     \
        if (!COMPARATOR().evaluate(aValue OPERATOR bValue)) {                        \
            std::ostringstream os;                                                   \
            os << "Expected [ " << aExpression << " " #OPERATOR " " << bExpression   \
               << " ] but found [ " << aValue << " " #OPERATOR " " << bValue << "]"; \
            GTEST_MESSAGE_AT_(theFile.c_str(),                                       \
                              theLine,                                               \
                              os.str().c_str(),                                      \
                              ::testing::TestPartResult::kFatalFailure);             \
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
