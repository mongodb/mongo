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

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

// IWYU pragma: no_include "boost/container/detail/flat_tree.hpp"
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/query/bson/multikey_dotted_path_support.h"
#include "mongo/unittest/unittest.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/vector.hpp>

namespace mongo {
namespace {

namespace mdps = ::mongo::multikey_dotted_path_support;

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


    BSONElementComparator eltCmp(BSONElementComparator::FieldNamesMode::kIgnore,
                                 &simpleStringDataComparator);
    for (size_t i = 0; i < expectedElements.size(); ++i) {
        if (eltCmp.evaluate(*expectedIt != *actualIt)) {
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

void dumpArrayComponents(const MultikeyComponents& arrayComponents, StringBuilder* sb) {
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

void assertArrayComponentsAreEqual(const MultikeyComponents& expectedArrayComponents,
                                   const MultikeyComponents& actualArrayComponents) {
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
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedMaxDepthObjectWithScalarValue) {
    BSONObj obj = BSON("a" << 1);
    std::string dotted_path = "a";
    dotted_path.reserve(1 + BSONDepth::getMaxAllowableDepth() * 2);
    for (uint32_t i = 0; i < BSONDepth::getMaxAllowableDepth(); ++i) {
        obj = BSON("a" << obj);
        dotted_path.insert(0, "a.");
    }

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, dotted_path, actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithEmptyArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSONArrayBuilder().arr()));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(std::vector<BSONObj>{}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithEmptyArrayValueAndExpandParamIsFalse) {
    BSONObj obj(fromjson("{a: {b: []}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSONArray())}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithSingletonArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithSingletonArrayValueAndExpandParamIsFalse) {
    BSONObj obj(fromjson("{a: {b: {c: [3]}}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(3))}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, NestedObjectWithArrayValue) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2), BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithArrayOfSubobjectsWithScalarValue) {
    BSONObj obj = BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << 2) << BSON("b" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
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
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
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
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(
        {BSON("" << BSON_ARRAY(1)), BSON("" << BSON_ARRAY(2)), BSON("" << BSON_ARRAY(3))},
        actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotExpandArrayWithinTrailingArray) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(3 << 4))));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(1 << 2)), BSON("" << BSON_ARRAY(3 << 4))},
                                  actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithTwoDimensionalArrayOfSubobjects) {
    // Does not expand the array within the array.
    BSONObj obj = fromjson("{a: [[{b: 0}, {b: 1}], [{b: 2}, {b: 3}]]}");

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ObjectWithDiverseStructure) {
    BSONObj obj = fromjson(
        "{a: ["
        "     {b: 0},"
        "     [{b: 1}, {b: {c: -1}}],"
        "     'no b here!',"
        "     {b: [{c: -2}, 'no c here!']},"
        "     {b: {c: [-3, -4]}}"
        "]}");

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << -2), BSON("" << -3), BSON("" << -4)}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U, 2U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, AcceptsNumericFieldNames) {
    BSONObj obj = BSON("a" << BSON("0" << 1));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, UsesNumericFieldNameToExtractElementFromArray) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("0" << 2)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, TreatsNegativeIndexAsFieldName) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("-1" << 2) << BSON("b" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.-1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, ExtractsNoValuesFromOutOfBoundsIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("b" << 2) << BSON("10" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.10", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotTreatHexStringAsIndexSpecification) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("0x2" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.0x2", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptLeadingPlusAsArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("+2" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.+2", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptTrailingCharactersForArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2xyz" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.2xyz", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesNotAcceptNonDigitsForArrayIndex) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2x4" << 2) << BSON("NOT THIS ONE" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.2x4", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({0U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath,
     DoesExtractNestedValuesFromWithinArraysTraversedWithPositionalPaths) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << BSON("2" << 2) << BSON("target" << 3)));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.2.target", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesExpandMultiplePositionalPathSpecifications) {
    BSONObj obj(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.1.0.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << "(1, 0)")}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesAcceptNumericInitialField) {
    BSONObj obj = BSON("a" << 1 << "0" << 2);

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "0", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, DoesExpandArrayFoundAfterPositionalSpecification) {
    BSONObj obj(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.1.b", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << "(1, 0)"), BSON("" << "(1, 1)")}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, PositionalElementsNotConsideredArrayComponents) {
    BSONObj obj(fromjson("{a: [{b: [1, 2]}]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.0.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, TrailingArrayIsExpandedEvenIfPositional) {
    BSONObj obj(fromjson("{a: {b: [0, [1, 2]]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 1), BSON("" << 2)}, actualElements);
    assertArrayComponentsAreEqual({2U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, PositionalTrailingArrayNotExpandedIfExpandParameterIsFalse) {
    BSONObj obj(fromjson("{a: {b: [0, [1, 2]]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = false;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.1", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << BSON_ARRAY(1 << 2))}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, MidPathEmptyArrayIsConsideredAnArrayComponent) {
    BSONObj obj(fromjson("{a: [{b: []}]}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual(std::vector<BSONObj>{}, actualElements);
    assertArrayComponentsAreEqual({0U, 1U}, actualArrayComponents);
}

TEST(ExtractAllElementsAlongPath, MidPathSingletonArrayIsConsideredAnArrayComponent) {
    BSONObj obj(fromjson("{a: {b: [{c: 3}]}}"));

    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "a.b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("" << 3)}, actualElements);
    assertArrayComponentsAreEqual({1U}, actualArrayComponents);
}

TEST(ExtractElementAtPathOrArrayAlongPath, FieldWithDotsDontHideNestedObjects) {
    BSONObj obj(fromjson("{b: {c: 'foo'}, \"b.c\": 'bar'}"));
    BSONElementSet actualElements;
    const bool expandArrayOnTrailingField = true;
    MultikeyComponents actualArrayComponents;
    mdps::extractAllElementsAlongPath(
        obj, "b.c", actualElements, expandArrayOnTrailingField, &actualArrayComponents);

    assertBSONElementSetsAreEqual({BSON("c" << "foo")}, actualElements);
    assertArrayComponentsAreEqual(MultikeyComponents{}, actualArrayComponents);
}

}  // namespace
}  // namespace mongo
