// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/query/util/field_set.h"

#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo::stage_builder {
namespace {

TEST(FieldSetTest, BasicTest) {
    std::vector<std::string> fieldNames = {"a", "b", "c", "d", "e", "f", "g", "h"};

    auto testMembership = [&](const FieldSet& fieldSet) {
        std::string result(fieldNames.size(), ' ');
        for (size_t i = 0; i < fieldNames.size(); ++i) {
            const auto& field = fieldNames[i];
            result[i] = fieldSet.count(field) ? 'T' : 'F';
        }
        return result;
    };

    auto fieldSetA = FieldSet::makeEmptySet();
    auto fieldSetB = FieldSet::makeUniverseSet();
    auto fieldSetC = FieldSet::makeClosedSet(std::vector<std::string>{"a", "b", "c", "d"});
    auto fieldSetD = FieldSet::makeClosedSet(std::vector<std::string>{"c", "b", "e", "f"});
    auto fieldSetE = FieldSet::makeClosedSet(std::vector<std::string>{"a", "c", "f", "g"});
    auto fieldSetF = FieldSet::makeOpenSet(std::vector<std::string>{"a", "b", "c", "d"});
    auto fieldSetG = FieldSet::makeOpenSet(std::vector<std::string>{"c", "b", "e", "f"});
    auto fieldSetH = FieldSet::makeOpenSet(std::vector<std::string>{"a", "c", "f", "g"});

    ASSERT_EQ(testMembership(fieldSetA), "FFFFFFFF");
    ASSERT_EQ(testMembership(fieldSetB), "TTTTTTTT");
    ASSERT_EQ(testMembership(fieldSetC), "TTTTFFFF");
    ASSERT_EQ(testMembership(fieldSetD), "FTTFTTFF");
    ASSERT_EQ(testMembership(fieldSetE), "TFTFFTTF");
    ASSERT_EQ(testMembership(fieldSetF), "FFFFTTTT");
    ASSERT_EQ(testMembership(fieldSetG), "TFFTFFTT");
    ASSERT_EQ(testMembership(fieldSetH), "FTFTTFFT");

    std::vector<FieldSet> fieldSets;
    fieldSets.emplace_back(fieldSetA);
    fieldSets.emplace_back(fieldSetB);
    fieldSets.emplace_back(fieldSetC);
    fieldSets.emplace_back(fieldSetD);
    fieldSets.emplace_back(fieldSetE);
    fieldSets.emplace_back(fieldSetF);
    fieldSets.emplace_back(fieldSetG);
    fieldSets.emplace_back(fieldSetH);

    for (auto&& lhsSet : fieldSets) {
        for (auto&& rhsSet : fieldSets) {
            auto unionSet = lhsSet;
            unionSet.setUnion(rhsSet);

            auto intersectionSet = lhsSet;
            intersectionSet.setIntersect(rhsSet);

            for (auto&& field : fieldNames) {
                bool inUnion = lhsSet.count(field) || rhsSet.count(field);
                bool inIntersection = lhsSet.count(field) && rhsSet.count(field);

                ASSERT_EQ(unionSet.count(field), inUnion);
                ASSERT_EQ(intersectionSet.count(field), inIntersection);
            }
        }
    }
}

}  // namespace
}  // namespace mongo::stage_builder
