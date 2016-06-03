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

#include "mongo/platform/basic.h"

#include <set>
#include <vector>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace dps = ::mongo::dotted_path_support;

TEST(DottedPathSupport, CompareObjectsAccordingToSort) {
    ASSERT_LT(dps::compareObjectsAccordingToSort(
                  BSON("a" << 1), BSON("a" << 2), BSON("b" << 1 << "a" << 1)),
              0);
    ASSERT_EQ(
        dps::compareObjectsAccordingToSort(BSON("a" << BSONNULL), BSON("b" << 1), BSON("a" << 1)),
        0);
}

TEST(DottedPathSupport, ExtractElementAtPath) {
    BSONObj obj = BSON("a" << 1 << "b" << BSON("a" << 2) << "c"
                           << BSON_ARRAY(BSON("a" << 3) << BSON("a" << 4)));
    ASSERT_EQUALS(1, dps::extractElementAtPath(obj, "a").numberInt());
    ASSERT_EQUALS(2, dps::extractElementAtPath(obj, "b.a").numberInt());
    ASSERT_EQUALS(3, dps::extractElementAtPath(obj, "c.0.a").numberInt());
    ASSERT_EQUALS(4, dps::extractElementAtPath(obj, "c.1.a").numberInt());

    ASSERT_TRUE(dps::extractElementAtPath(obj, "x").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "a.x").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "x.y").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, ".").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "..").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "...").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "a.").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, ".a").eoo());
    ASSERT_TRUE(dps::extractElementAtPath(obj, "b.a.").eoo());
}

TEST(DottedPathSupport, ExtractElementsBasedOnTemplate) {
    BSONObj obj = BSON("a" << 10 << "b" << 11);

    ASSERT_EQ(BSON("a" << 10).woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1))),
              0);
    ASSERT_EQ(BSON("b" << 11).woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("b" << 1))),
              0);
    ASSERT_EQ(obj.woCompare(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "b" << 1))),
              0);

    ASSERT_EQ(dps::extractElementsBasedOnTemplate(obj, BSON("a" << 1 << "c" << 1))
                  .firstElement()
                  .fieldNameStringData(),
              "a");
}

void dumpBSONElementSet(const BSONElementSet& elements, StringBuilder* sb) {
    *sb << "[ ";
    bool firstIteration = true;
    for (auto&& elem : elements) {
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << "'" << elem << "'";
        firstIteration = false;
    }
    *sb << " ]";
}

void assertBSONElementSetsAreEqual(const std::vector<BSONObj>& expectedObjs,
                                   const BSONElementSet& actualElements) {
    BSONElementSet expectedElements;
    for (auto&& obj : expectedObjs) {
        expectedElements.insert(obj.firstElement());
    }

    if (expectedElements.size() != actualElements.size()) {
        StringBuilder sb;
        sb << "Expected set to contain " << expectedElements.size()
           << " element(s), but actual set contains " << actualElements.size()
           << " element(s); Expected set: ";
        dumpBSONElementSet(expectedElements, &sb);
        sb << ", Actual set: ";
        dumpBSONElementSet(actualElements, &sb);
        FAIL(sb.str());
    }

    // We do our own comparison of the two BSONElementSets because BSONElement::operator== considers
    // the field name.
    auto expectedIt = expectedElements.begin();
    auto actualIt = actualElements.begin();

    for (size_t i = 0; i < expectedElements.size(); ++i) {
        if (!expectedIt->valuesEqual(*actualIt)) {
            StringBuilder sb;
            sb << "Element '" << *expectedIt << "' doesn't have the same value as element '"
               << *actualIt << "'; Expected set: ";
            dumpBSONElementSet(expectedElements, &sb);
            sb << ", Actual set: ";
            dumpBSONElementSet(actualElements, &sb);
            FAIL(sb.str());
        }

        ++expectedIt;
        ++actualIt;
    }
}

void dumpArrayComponents(const std::set<size_t>& arrayComponents, StringBuilder* sb) {
    *sb << "[ ";
    bool firstIteration = true;
    for (const auto pos : arrayComponents) {
        if (!firstIteration) {
            *sb << ", ";
        }
        *sb << pos;
        firstIteration = false;
    }
    *sb << " ]";
}

void assertArrayComponentsAreEqual(const std::set<size_t>& expectedArrayComponents,
                                   const std::set<size_t>& actualArrayComponents) {
    if (expectedArrayComponents != actualArrayComponents) {
        StringBuilder sb;
        sb << "Expected: ";
        dumpArrayComponents(expectedArrayComponents, &sb);
        sb << ", Actual: ";
        dumpArrayComponents(actualArrayComponents, &sb);
        FAIL(sb.str());
    }
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithScalarValue) {
    BSONObj obj = BSON("a" << BSON("b" << 1));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithEmptyArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSONArrayBuilder().arr()));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(std::vector<BSONObj>{}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithSingletonArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(std::set<size_t>{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithArrayOfSubobjectsWithScalarValue) {
    BSONObj obj = BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2) << BSON("b" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithArrayOfSubobjectsWithArrayValues) {
    BSONObj obj =
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1 << 2)) << BSON("b" << BSON_ARRAY(2 << 3))
                                                               << BSON("b" << BSON_ARRAY(3 << 1))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath,
     ObjectWithArrayOfSubobjectsWithArrayValuesButNotExpandingTrailingArrayValues) {
    BSONObj obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(1)) << BSON("b" << BSON_ARRAY(2))
                                                                    << BSON("b" << BSON_ARRAY(3))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    std::set<size_t> actualArrayComponents;
    dps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(
        {BSON("" << BSON_ARRAY(1)), BSON("" << BSON_ARRAY(2)), BSON("" << BSON_ARRAY(3))},
        actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

}  // namespace
}  // namespace mongo
