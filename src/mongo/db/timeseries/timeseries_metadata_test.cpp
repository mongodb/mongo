// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tracking/context.h"

#include <string_view>

namespace mongo::timeseries::metadata {
using namespace std::literals::string_view_literals;

constexpr std::string_view kMetaFieldName = "metadata"sv;
constexpr std::string_view kMetaFieldNameAlt = "meta"sv;

BSONObj getNormalizedValue(const BSONElement& elem) {
    tracking::Context trackingContext;
    allocator_aware::BSONObjBuilder<tracking::Allocator<void>> builder{
        trackingContext.makeAllocator<void>()};
    normalize(elem, builder);
    return builder.done().getOwned();
}

BSONArray createBSONArraysWithFieldNamesAndValues(std::vector<std::string_view> fieldNames,
                                                  std::vector<std::string_view> values) {
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
    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>>
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

    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>>
        BSONObjCases = {
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
                            BSON(kMetaFieldName << BSON("a" << "1")).getOwned(),
                            kMetaFieldName,
                            false),  // Simple BSONObj inequality on field type
            std::make_tuple(BSON(kMetaFieldName << BSON("a" << 1)).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON("a" << 1 << "b" << 1)).getOwned(),
                            kMetaFieldName,
                            false),  // Simple BSONObj inequality, extra field
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 2))).getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("a" << BSON("c" << 2 << "b" << 1))).getOwned(),
                kMetaFieldName,
                true),  // Different field order, same values
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 2 << "c" << 2))).getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("a" << BSON("c" << 3 << "a" << 1))).getOwned(),
                kMetaFieldName,
                false),  // Different field order, different fieldNames
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 2 << "a" << 2))).getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("a" << BSON("a" << "thirty" << "b" << 1))).getOwned(),
                kMetaFieldName,
                false),  // Different field order, different types
        };

    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>> arrayCases =
        {
            std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            true),  // Equal array fields
            std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON_ARRAY("a")),
                            kMetaFieldName,
                            false),  // Missing value
            std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON_ARRAY("a" << "b" << "c")),
                            kMetaFieldName,
                            false),  // Extra value
            std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON_ARRAY("b" << "a")),
                            kMetaFieldName,
                            false),  // Different order
            std::make_tuple(BSON(kMetaFieldName << BSON_ARRAY("a" << "b")),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON_ARRAY("a" << "c")),
                            kMetaFieldName,
                            false),  // Different values
        };

    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>>
        nestedObjects = {
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1)))
                    .getOwned(),
                kMetaFieldName,
                true),  // Different order
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName
                     << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1 << "e" << 1)))
                    .getOwned(),
                kMetaFieldName,
                false),  // Extra field
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1 << "e" << 1)) << "d"
                                    << 1)
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1)))
                    .getOwned(),
                kMetaFieldName,
                false),  // Missing field
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << 1 << "c" << 1) << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 2)))
                    .getOwned(),
                kMetaFieldName,
                false),  // Different values
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON("b" << "1" << "c" << 1) << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON("c" << 1 << "b" << 1)))
                    .getOwned(),
                kMetaFieldName,
                false),  // Different types
        };

    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>>
        nestedArrays = {
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                true),  // Equal arrays.
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c" << "b") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                false),  // Different order
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c" << "b") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c" << "e") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                false),  // Extra field
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("c" << "b") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b") << "d" << 1)).getOwned(),
                kMetaFieldName,
                false),  // Missing field
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "f") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                false),  // Different values
            std::make_tuple(
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << "c") << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON_ARRAY("a" << BSON_ARRAY("b" << 1) << "d" << 1))
                    .getOwned(),
                kMetaFieldName,
                false),  // Different types
        };

    std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>> mixedCases =
        {
            std::make_tuple(
                BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << "c") << "d" << 1)).getOwned(),
                kMetaFieldName,
                BSON(kMetaFieldName << BSON("d" << 1 << "a" << BSON_ARRAY("b" << "c"))).getOwned(),
                kMetaFieldName,
                true),  // Nested equal arrays, different order field for object
            std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << "c"))).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("c" << "b"))).getOwned(),
                            kMetaFieldName,
                            false),  // Nested array with different order
            std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << "c"))).getOwned(),
                            kMetaFieldName,
                            BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << "d"))).getOwned(),
                            kMetaFieldName,
                            false),  // Nested array with different values
            std::make_tuple(BSON(kMetaFieldName << BSON("a" << BSON_ARRAY("b" << "c"))).getOwned(),
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


    using TestCases =
        std::vector<std::tuple<BSONObj, std::string_view, BSONObj, std::string_view, bool>>;

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

            EXPECT_EQ(areMetadataEqual(lhsElement, rhsElement), expectedEqual);

            auto lhsObjNormalized = getNormalizedValue(lhsElement);
            auto lhsElementNormalized = lhsObjNormalized.getField(lhsMetaFieldName);
            auto rhsObjNormalized = getNormalizedValue(rhsElement);
            auto rhsElementNormalized = rhsObjNormalized.getField(rhsMetaFieldName);

            // Assert that the inputted values are equal to themselves when normalized.
            ASSERT(areMetadataEqual(lhsElement, lhsElementNormalized));
            ASSERT(areMetadataEqual(rhsElement, rhsElementNormalized));

            // Assert that normalizing either or both of the values behaves accordingly with
            // 'expectedEqual'.
            EXPECT_EQ(areMetadataEqual(lhsElement, rhsElementNormalized), expectedEqual);
            EXPECT_EQ(areMetadataEqual(lhsElementNormalized, rhsElement), expectedEqual);
            EXPECT_EQ(areMetadataEqual(lhsElementNormalized, rhsElementNormalized), expectedEqual);
        }
    }
}

TEST(TimeseriesMetadataTest, IncorrectFieldNamesForArraysGetIgnored) {
    using TestCase = std::tuple<std::vector<std::string_view>,
                                std::vector<std::string_view>,
                                std::vector<std::string_view>,
                                std::vector<std::string_view>,
                                bool>;

    std::vector<TestCase> testCases{
        std::make_tuple(std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        true),  // Simple equality
        std::make_tuple(std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "d", "c"},
                        false),  // Simple inequality
        std::make_tuple(std::vector<std::string_view>{"1", "0", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        true),  // Incorrect order for implicit field names, should ignore names
        std::make_tuple(std::vector<std::string_view>{"2", "1", "0"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        true),  // Reverse order for implicit field names, should ignore names
        std::make_tuple(std::vector<std::string_view>{"2", "1", "0"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"a", "b", "d"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        true),  // Non-numerical fieldNames
        std::make_tuple(std::vector<std::string_view>{"2", "1", "0"},
                        std::vector<std::string_view>{"a", "b", "c"},
                        std::vector<std::string_view>{"0", "1", "2"},
                        std::vector<std::string_view>{"a", "d", "c"},
                        false),  // Incorrect order and different values
    };
    for (auto& [lhsFieldNames, lhsValues, rhsFieldNames, rhsValues, expectedEqual] : testCases) {
        auto lhsArr = createBSONArraysWithFieldNamesAndValues(lhsFieldNames, lhsValues);
        auto lhs = BSON(kMetaFieldName << lhsArr).getOwned();
        auto rhsArr = createBSONArraysWithFieldNamesAndValues(rhsFieldNames, rhsValues);
        auto rhs = BSON(kMetaFieldName << rhsArr).getOwned();

        auto lhsElement = lhs.getField(kMetaFieldName);
        auto rhsElement = rhs.getField(kMetaFieldName);
        EXPECT_EQ(areMetadataEqual(lhsElement, rhsElement), expectedEqual);

        auto lhsObjNormalized = getNormalizedValue(lhsElement);
        auto lhsElementNormalized = lhsObjNormalized.getField(kMetaFieldName);
        auto rhsObjNormalized = getNormalizedValue(rhsElement);
        auto rhsElementNormalized = rhsObjNormalized.getField(kMetaFieldName);

        // Assert that the inputted values are equal to themselves when normalized.
        ASSERT(areMetadataEqual(lhsElement, lhsElementNormalized));
        ASSERT(areMetadataEqual(rhsElement, rhsElementNormalized));

        // Assert that normalizing either or both of the values behaves accordingly with
        // 'expectedEqual'.
        EXPECT_EQ(areMetadataEqual(lhsElement, rhsElementNormalized), expectedEqual);
        EXPECT_EQ(areMetadataEqual(lhsElementNormalized, rhsElement), expectedEqual);
        EXPECT_EQ(areMetadataEqual(lhsElementNormalized, rhsElementNormalized), expectedEqual);
    }
}

}  // namespace mongo::timeseries::metadata
