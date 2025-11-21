/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tracking_allocator.h"
#include "mongo/util/tracking_context.h"

namespace mongo::timeseries::metadata {

constexpr StringData kMetaFieldName = "metadata"_sd;
constexpr StringData kMetaFieldNameAlt = "meta"_sd;

BSONObj getNormalizedValue(const BSONElement& elem) {
    TrackingContext trackingContext;
    allocator_aware::BSONObjBuilder<TrackingAllocator<void>> builder{
        trackingContext.makeAllocator<void>()};
    normalize(elem, builder);
    return builder.done().getOwned();
}

BSONArray createBSONArraysWithFieldNamesAndValues(std::vector<StringData> fieldNames,
                                                  std::vector<StringData> values) {
    ASSERT_EQ(fieldNames.size(), values.size());
    BSONObjBuilder builder;
    {
        BSONObjBuilder subBuilder(builder.subarrayStart("metaField"));
        for (size_t i = 0; i < fieldNames.size(); i++) {
            subBuilder.append(fieldNames[i], values[i]);
        }
    }
    BSONArray malformedArray(builder.obj());
    return malformedArray;
}

TEST(TimeseriesMetadataTest, AreMetadataEqualIgnoresFieldOrder) {
    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>>
        simpleBSONElementTestCases = {
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            true),  // Simple BSONElement equality
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << 2).getOwned(),
                            kMetaFieldName,
                            false),  // Simple BSONElement inequality on value
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << "1").getOwned(),
                            kMetaFieldName,
                            false),  // Simple BSONElement inequality on type
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldNameAlt << 1).getOwned(),
                            kMetaFieldNameAlt,
                            true),  // Ignore fieldNames
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldNameAlt << 2).getOwned(),
                            kMetaFieldNameAlt,
                            false),  // Different fieldNames and different values
            std::make_tuple(BSON(kMetaFieldName << 1).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldNameAlt << "1").getOwned(),
                            kMetaFieldNameAlt,
                            false),  // Different fieldNames and different types
        };

    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>> BSONObjCases = {
        std::make_tuple(BSON(kMetaFieldName << BSONObj()).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSONObj()).getOwned(),
                        kMetaFieldName,
                        true),  // Empty objects
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        true),  // Simple BSONObj equality
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("b" << 1)).getOwned(),
                        kMetaFieldName,
                        false),  // Simple BSONObj inequality on field name
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << 2)).getOwned(),
                        kMetaFieldName,
                        false),  // Simple BSONObj inequality on field value
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a"
                                                    << "1"))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Simple BSONObj inequality on field type
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << 1 << "b" << 1)).getOwned(),
                        kMetaFieldName,
                        false),  // Simple BSONObj inequality, extra field
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 2))).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON("c" << 2 << "b" << 1))).getOwned(),
                        kMetaFieldName,
                        true),  // Different field order, same values
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON("b" << 2 << "c" << 2))).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON("c" << 3 << "a" << 1))).getOwned(),
                        kMetaFieldName,
                        false),  // Different field order, different fieldNames
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON("b" << 2 << "a" << 2))).getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON("a"
                                                                << "thirty"
                                                                << "b" << 1)))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Different field order, different types
    };

    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>> arrayCases = {
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        true),  // Equal array fields
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a")),
                        kMetaFieldName,
                        false),  // Missing value
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b"
                                                          << "c")),
                        kMetaFieldName,
                        false),  // Extra value
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("b"
                                                          << "a")),
                        kMetaFieldName,
                        false),  // Different order
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "b")),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a"
                                                          << "c")),
                        kMetaFieldName,
                        false),  // Different values
    };

    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>> nestedObjects = {
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1)).getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1))).getOwned(),
            kMetaFieldName,
            true),  // Different order
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1)).getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1 << "e" << 1)))
                .getOwned(),
            kMetaFieldName,
            false),  // Extra field
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1 << "e" << 1)) << "d" << 1)
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1))).getOwned(),
            kMetaFieldName,
            false),  // Missing field
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1)).getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 2))).getOwned(),
            kMetaFieldName,
            false),  // Different values
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON("b"
                                                    << "1"
                                                    << "c" << 1)
                                            << "d" << 1))
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1))).getOwned(),
            kMetaFieldName,
            false),  // Different types
    };

    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>> nestedArrays = {
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "c")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "c")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        true),  // Equal arrays.
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c"
                                                                            << "b")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "c")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Different order
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c"
                                                                            << "b")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "c"
                                                                            << "e")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Extra field
        std::make_tuple(
            BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c"
                                                                << "b")
                                                  << "d" << 1))
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b") << "d" << 1)).getOwned(),
            kMetaFieldName,
            false),  // Missing field
        std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "c")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                            << "f")
                                                              << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Different values
        std::make_tuple(
            BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b"
                                                                << "c")
                                                  << "d" << 1))
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << 1) << "d" << 1)).getOwned(),
            kMetaFieldName,
            false),  // Different types
    };

    std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>> mixedCases = {
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b"
                                                                      << "c")
                                                        << "d" << 1))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("d" << 1 << "a"
                                                        << BSON_ARRAY("b"
                                                                      << "c")))
                            .getOwned(),
                        kMetaFieldName,
                        true),  // Nested equal arrays, different order field for object
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b"
                                                                      << "c")))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("c"
                                                                      << "b")))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Nested array with different order
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b"
                                                                      << "c")))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b"
                                                                      << "d")))
                            .getOwned(),
                        kMetaFieldName,
                        false),  // Nested array with different values
        std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b"
                                                                      << "c")))
                            .getOwned(),
                        kMetaFieldName,
                        BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << 1))).getOwned(),
                        kMetaFieldName,
                        false),  // Nested array with different types
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << BSON("c" << 1 << "d" << 2))))
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << BSON("d" << 2 << "c" << 1))))
                .getOwned(),
            kMetaFieldName,
            true),  // Array with single nested object with equal values but different order
        std::make_tuple(
            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << BSON("c" << 1 << "d" << 2))))
                .getOwned(),
            kMetaFieldName,
            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY(BSON("d" << 2 << "c" << 1) << "b")))
                .getOwned(),
            kMetaFieldName,
            false),  // Array with multiple elements, different order
    };


    using TestCases = std::vector<std::tuple<BSONObj, StringData, BSONObj, StringData, bool>>;

    std::vector<TestCases> allTests = {simpleBSONElementTestCases,
                                       BSONObjCases,
                                       arrayCases,
                                       nestedObjects,
                                       nestedArrays,
                                       mixedCases};

    for (const auto& testCases : allTests) {
        for (auto& [lhs, lhsMetaFieldName, rhs, rhsMetaFieldName, expectedEqual] : testCases) {
            auto lhsElement = lhs.getField(lhsMetaFieldName);
            auto rhsElement = rhs.getField(rhsMetaFieldName);

            ASSERT_EQ(areMetadataEqual(lhsElement, rhsElement), expectedEqual);

            auto lhsObjNormalized = getNormalizedValue(lhsElement);
            auto lhsElementNormalized = lhsObjNormalized.getField(lhsMetaFieldName);
            auto rhsObjNormalized = getNormalizedValue(rhsElement);
            auto rhsElementNormalized = rhsObjNormalized.getField(rhsMetaFieldName);

            // Assert that the inputted values are equal to themselves when normalized.
            ASSERT(areMetadataEqual(lhsElement, lhsElementNormalized));
            ASSERT(areMetadataEqual(rhsElement, rhsElementNormalized));

            // Assert that normalizing either or both of the values behaves accordingly with
            // 'expectedEqual'.
            ASSERT_EQ(areMetadataEqual(lhsElement, rhsElementNormalized), expectedEqual);
            ASSERT_EQ(areMetadataEqual(lhsElementNormalized, rhsElement), expectedEqual);
            ASSERT_EQ(areMetadataEqual(lhsElementNormalized, rhsElementNormalized), expectedEqual);
        }
    }
}

TEST(TimeseriesMetadataTest, IncorrectFieldNamesForArraysGetIgnored) {
    using TestCase = std::tuple<std::vector<StringData>,
                                std::vector<StringData>,
                                std::vector<StringData>,
                                std::vector<StringData>,
                                bool>;

    std::vector<TestCase> testCases{
        std::make_tuple(std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        true),  // Simple equality
        std::make_tuple(std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "d", "c"},
                        false),  // Simple inequality
        std::make_tuple(std::vector<StringData>{"1", "0", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        true),  // Incorrect order for implicit field names, should ignore names
        std::make_tuple(std::vector<StringData>{"2", "1", "0"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "b", "c"},
                        true),  // Reverse order for implicit field names, should ignore names
        std::make_tuple(std::vector<StringData>{"2", "1", "0"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"a", "b", "d"},
                        std::vector<StringData>{"a", "b", "c"},
                        true),  // Non-numerical fieldNames
        std::make_tuple(std::vector<StringData>{"2", "1", "0"},
                        std::vector<StringData>{"a", "b", "c"},
                        std::vector<StringData>{"0", "1", "2"},
                        std::vector<StringData>{"a", "d", "c"},
                        false),  // Incorrect order and different values
    };
    for (auto& [lhsFieldNames, lhsValues, rhsFieldNames, rhsValues, expectedEqual] : testCases) {
        auto lhsArr = createBSONArraysWithFieldNamesAndValues(lhsFieldNames, lhsValues);
        auto lhs = BSON(kMetaFieldName << lhsArr).getOwned();
        auto rhsArr = createBSONArraysWithFieldNamesAndValues(rhsFieldNames, rhsValues);
        auto rhs = BSON(kMetaFieldName << rhsArr).getOwned();

        auto lhsElement = lhs.getField(kMetaFieldName);
        auto rhsElement = rhs.getField(kMetaFieldName);
        ASSERT_EQ(areMetadataEqual(lhsElement, rhsElement), expectedEqual);

        auto lhsObjNormalized = getNormalizedValue(lhsElement);
        auto lhsElementNormalized = lhsObjNormalized.getField(kMetaFieldName);
        auto rhsObjNormalized = getNormalizedValue(rhsElement);
        auto rhsElementNormalized = rhsObjNormalized.getField(kMetaFieldName);

        // Assert that the inputted values are equal to themselves when normalized.
        ASSERT(areMetadataEqual(lhsElement, lhsElementNormalized));
        ASSERT(areMetadataEqual(rhsElement, rhsElementNormalized));

        // Assert that normalizing either or both of the values behaves accordingly with
        // 'expectedEqual'.
        ASSERT_EQ(areMetadataEqual(lhsElement, rhsElementNormalized), expectedEqual);
        ASSERT_EQ(areMetadataEqual(lhsElementNormalized, rhsElement), expectedEqual);
        ASSERT_EQ(areMetadataEqual(lhsElementNormalized, rhsElementNormalized), expectedEqual);
    }
}

}  // namespace mongo::timeseries::metadata
