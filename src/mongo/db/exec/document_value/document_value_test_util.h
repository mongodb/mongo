// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/unittest/unittest.h"

#include <span>
#include <string>
#include <string_view>

/**
 * Use to compare two instances of type Value under the default ValueComparator in unit tests.
 */
#define ASSERT_VALUE_EQ(a, b) _ASSERT_DOCVAL_COMPARISON(ValueEQ, a, b)
#define ASSERT_VALUE_LT(a, b) _ASSERT_DOCVAL_COMPARISON(ValueLT, a, b)
#define ASSERT_VALUE_LTE(a, b) _ASSERT_DOCVAL_COMPARISON(ValueLTE, a, b)
#define ASSERT_VALUE_GT(a, b) _ASSERT_DOCVAL_COMPARISON(ValueGT, a, b)
#define ASSERT_VALUE_GTE(a, b) _ASSERT_DOCVAL_COMPARISON(ValueGTE, a, b)
#define ASSERT_VALUE_NE(a, b) _ASSERT_DOCVAL_COMPARISON(ValueNE, a, b)

/**
 * Use to compare two instances of type Document under the default DocumentComparator in unit tests.
 */
#define ASSERT_DOCUMENT_EQ(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentEQ, a, b)
#define ASSERT_DOCUMENT_LT(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentLT, a, b)
#define ASSERT_DOCUMENT_LTE(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentLTE, a, b)
#define ASSERT_DOCUMENT_GT(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentGT, a, b)
#define ASSERT_DOCUMENT_GTE(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentGTE, a, b)
#define ASSERT_DOCUMENT_NE(a, b) _ASSERT_DOCVAL_COMPARISON(DocumentNE, a, b)

/**
 * Use to compare two collections which are convertible to `std::span<const Document>` under the
 * default DocumentComparator in unit tests.
 */
#define ASSERT_DOCUMENTS_EQ(expectDocs, actualDocs) \
    ASSERT_THAT(actualDocs, ::mongo::unittest::detail::DocumentsEq(expectDocs))
#define ASSERT_DOCUMENTS_NE(expectDocs, actualDocs) \
    ASSERT_THAT(actualDocs, ::testing::Not(::mongo::unittest::detail::DocumentsEq(expectDocs)))

/**
 * Document/Value comparison utility macro. Do not use directly.
 */
#define _ASSERT_DOCVAL_COMPARISON(NAME, a, b) \
    ::mongo::unittest::assertComparison_##NAME(__FILE__, __LINE__, #a, #b, a, b)

#define ASSERT_VALUE_EQ_AUTO(expected, val) ASSERT_STR_EQ_AUTO(expected, val.toString())
#define ASSERT_DOCUMENT_EQ_AUTO(expected, actual) ASSERT_BSONOBJ_EQ_AUTO(expected, actual.toBson())
namespace mongo {
namespace unittest {

#define _DECLARE_DOCVAL_CMP_FUNC(DOCVAL, NAME)                         \
    void assertComparison_##DOCVAL##NAME(const std::string& theFile,   \
                                         unsigned theLine,             \
                                         std::string_view aExpression, \
                                         std::string_view bExpression, \
                                         const DOCVAL& aValue,         \
                                         const DOCVAL& bValue);

_DECLARE_DOCVAL_CMP_FUNC(Value, EQ);
_DECLARE_DOCVAL_CMP_FUNC(Value, LT);
_DECLARE_DOCVAL_CMP_FUNC(Value, LTE);
_DECLARE_DOCVAL_CMP_FUNC(Value, GT);
_DECLARE_DOCVAL_CMP_FUNC(Value, GTE);
_DECLARE_DOCVAL_CMP_FUNC(Value, NE);

_DECLARE_DOCVAL_CMP_FUNC(Document, EQ);
_DECLARE_DOCVAL_CMP_FUNC(Document, LT);
_DECLARE_DOCVAL_CMP_FUNC(Document, LTE);
_DECLARE_DOCVAL_CMP_FUNC(Document, GT);
_DECLARE_DOCVAL_CMP_FUNC(Document, GTE);
_DECLARE_DOCVAL_CMP_FUNC(Document, NE);
#undef _DECLARE_DOCVAL_CMP_FUNC

namespace detail {

MATCHER_P(DocumentsEq, expectDocs, "") {
    const auto actualDocs = arg;
    auto cmp = [](std::span<const Document> expectDocs, std::span<const Document> actualDocs) {
        for (size_t i = 0; i < expectDocs.size() && i < actualDocs.size(); i++) {
            if (DocumentComparator().evaluate(expectDocs[i] != actualDocs[i])) {
                return false;
            }
        }
        return expectDocs.size() == actualDocs.size();
    };
    return cmp(expectDocs, actualDocs);
}

}  // namespace detail

}  // namespace unittest
}  // namespace mongo
