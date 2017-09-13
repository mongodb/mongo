/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_comparator.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/unittest/unittest.h"

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
 * Document/Value comparison utility macro. Do not use directly.
 */
#define _ASSERT_DOCVAL_COMPARISON(NAME, a, b) \
    ::mongo::unittest::assertComparison_##NAME(__FILE__, __LINE__, #a, #b, a, b)

namespace mongo {
namespace unittest {

#define _DECLARE_DOCVAL_CMP_FUNC(DOCVAL, NAME)                       \
    void assertComparison_##DOCVAL##NAME(const std::string& theFile, \
                                         unsigned theLine,           \
                                         StringData aExpression,     \
                                         StringData bExpression,     \
                                         const DOCVAL& aValue,       \
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

}  // namespace unittest
}  // namespace mongo
