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

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/unittest/unittest.h"

/**
 * BSON comparison utility macro. Do not use directly.
 */
#define ASSERT_BSON_COMPARISON(NAME, a, b) \
    ::mongo::unittest::assertComparison_##NAME(__FILE__, __LINE__, #a, #b, a, b)

/**
 * Use to compare two instances of type BSONObj under the default comparator in unit tests.
 */
#define ASSERT_BSONOBJ_EQ(a, b) ASSERT_BSON_COMPARISON(BSONObjEQ, a, b)
#define ASSERT_BSONOBJ_LT(a, b) ASSERT_BSON_COMPARISON(BSONObjLT, a, b)
#define ASSERT_BSONOBJ_LTE(a, b) ASSERT_BSON_COMPARISON(BSONObjLTE, a, b)
#define ASSERT_BSONOBJ_GT(a, b) ASSERT_BSON_COMPARISON(BSONObjGT, a, b)
#define ASSERT_BSONOBJ_GTE(a, b) ASSERT_BSON_COMPARISON(BSONObjGTE, a, b)
#define ASSERT_BSONOBJ_NE(a, b) ASSERT_BSON_COMPARISON(BSONObjNE, a, b)

namespace mongo {
namespace unittest {

#define DECLARE_BSON_CMP_FUNC(BSONTYPE, NAME)                          \
    void assertComparison_##BSONTYPE##NAME(const std::string& theFile, \
                                           unsigned theLine,           \
                                           StringData aExpression,     \
                                           StringData bExpression,     \
                                           const BSONTYPE& aValue,     \
                                           const BSONTYPE& bValue);

DECLARE_BSON_CMP_FUNC(BSONObj, EQ);
DECLARE_BSON_CMP_FUNC(BSONObj, LT);
DECLARE_BSON_CMP_FUNC(BSONObj, LTE);
DECLARE_BSON_CMP_FUNC(BSONObj, GT);
DECLARE_BSON_CMP_FUNC(BSONObj, GTE);
DECLARE_BSON_CMP_FUNC(BSONObj, NE);
#undef DECLARE_BSON_CMP_FUNC

}  // namespace unittest
}  // namespace mongo
