/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/array_filter.h"

#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using unittest::assertGet;

TEST(ArrayFilterTest, ParseBasic) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{i: 0}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator));
    ASSERT_EQ(arrayFilter->getId(), "i");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ArrayFilterTest, ParseDottedField) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'i.a': 0, 'i.b': 1}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator));
    ASSERT_EQ(arrayFilter->getId(), "i");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 1}}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: {a: 0, b: 0}}")));
}

TEST(ArrayFilterTest, ParseLogicalQuery) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{$and: [{i: {$gte: 0}}, {i: {$lte: 0}}]}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator));
    ASSERT_EQ(arrayFilter->getId(), "i");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 0}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 1}")));
}

TEST(ArrayFilterTest, ParseElemMatch) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{i: {$elemMatch: {a: 0}}}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator));
    ASSERT_EQ(arrayFilter->getId(), "i");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: [{a: 0}]}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: [{a: 1}]}")));
}

TEST(ArrayFilterTest, ParseCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    auto rawArrayFilter = fromjson("{i: 'abc'}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), &collator));
    ASSERT_EQ(arrayFilter->getId(), "i");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 'cba'}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{i: 0}")));
}

TEST(ArrayFilterTest, ParseIdContainsNumbersAndCapitals) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{iA3: 0}");
    auto arrayFilter = assertGet(
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator));
    ASSERT_EQ(arrayFilter->getId(), "iA3");
    ASSERT_TRUE(arrayFilter->getFilter()->matchesBSON(fromjson("{'iA3': 0}")));
    ASSERT_FALSE(arrayFilter->getFilter()->matchesBSON(fromjson("{'iA3': 1}")));
}

TEST(ArrayFilterTest, BadMatchExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{$and: 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, EmptyMatchExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, NestedEmptyMatchExpressionFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{$or: [{i: 0}, {$and: [{}]}]}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, EmptyFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'': 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, EmptyElemMatchFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'': {$elemMatch: {a: 0}}}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, EmptyTopLevelFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'.i': 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, MultipleTopLevelFieldsFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{$and: [{i: 0}, {j: 0}]}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, SpecialCharactersInFieldNameFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'i&': 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, FieldNameStartingWithNumberFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'3i': 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

TEST(ArrayFilterTest, FieldNameStartingWithCapitalFailsToParse) {
    const CollatorInterface* collator = nullptr;
    auto rawArrayFilter = fromjson("{'Ai': 0}");
    auto status =
        ArrayFilter::parse(rawArrayFilter, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(status.getStatus());
}

}  // namespace
