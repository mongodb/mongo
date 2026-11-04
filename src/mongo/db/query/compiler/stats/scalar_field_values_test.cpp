// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/stats/scalar_field_values.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/unittest/unittest.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace mongo::stats {
namespace {

using sbe::value::TagValueView;
using sbe::value::TypeTags;

TagValueView bsonView(const BSONObj& doc) {
    return {TypeTags::bsonObject, sbe::value::bitcastFrom<const char*>(doc.objdata())};
}

template <typename Test>
void withDocumentViews(const BSONObj& doc, Test test) {
    test(bsonView(doc));
    const auto wrapped = BSON("" << doc);
    const auto native = sbe::bson::convertToOwned(wrapped.firstElement());
    test(native.view());
}

TEST(ScalarFieldValuesTest, PreservesPathOrder) {
    const std::array paths{FieldPath("b.c"), FieldPath("a")};
    withDocumentViews(BSON("a" << 7 << "b" << BSON("c" << BSONNULL)), [&](auto doc) {
        const auto values = extractScalarFieldValues(doc, paths);
        ASSERT_EQ(values[0].tag, TypeTags::Null);
        ASSERT_EQ(values[1].tag, TypeTags::NumberInt32);
        ASSERT_EQ(sbe::value::bitcastTo<int32_t>(values[1].value), 7);
    });
}

TEST(ScalarFieldValuesTest, MissingFieldBecomesNothing) {
    const std::array paths{FieldPath("absent")};
    withDocumentViews(BSONObj{}, [&](auto doc) {
        const auto values = extractScalarFieldValues(doc, paths);
        ASSERT_EQ(values[0].tag, TypeTags::Nothing);
    });
}

TEST(ScalarFieldValuesTest, ScalarAlongDottedPathBecomesMissing) {
    const std::array paths{FieldPath("a.b")};
    withDocumentViews(BSON("a" << 7), [&](auto doc) {
        const auto values = extractScalarFieldValues(doc, paths);
        ASSERT_EQ(values[0].tag, TypeTags::Nothing);
    });
}

TEST(ScalarFieldValuesTest, RejectsArrayField) {
    const std::array paths{FieldPath("a")};
    withDocumentViews(BSON("a" << BSON_ARRAY(1)), [&](auto doc) {
        ASSERT_THROWS_CODE(extractScalarFieldValues(doc, paths), DBException, 13175701);
    });
}

TEST(ScalarFieldValuesTest, RejectsArrayAlongDottedPath) {
    const std::array paths{FieldPath("a.b")};
    withDocumentViews(BSON("a" << BSON_ARRAY(BSON("b" << 1))), [&](auto doc) {
        ASSERT_THROWS_CODE(extractScalarFieldValues(doc, paths), DBException, 13175701);
    });
}

TEST(ScalarFieldValuesTest, BorrowsBsonStorage) {
    const std::array paths{FieldPath("a")};
    const auto doc = BSON("a" << "a heap allocated string value");
    const auto values = extractScalarFieldValues(bsonView(doc), paths);
    ASSERT_EQ(values[0].tag, TypeTags::bsonString);
    ASSERT_EQ(values[0].value, sbe::bson::convertToView(doc.firstElement()).value);
}

TEST(ScalarFieldValuesTest, BorrowsNativeStorage) {
    const std::array paths{FieldPath("a")};
    const auto wrapped = BSON("" << BSON("a" << "a heap allocated string value"));
    const auto doc = sbe::bson::convertToOwned(wrapped.firstElement());
    const auto values = extractScalarFieldValues(doc.view(), paths);
    const auto field = sbe::value::getObjectView(doc.value())->getField("a");
    ASSERT_EQ(values[0].tag, field.tag);
    ASSERT_EQ(values[0].value, field.value);
}

TEST(ScalarFieldValuesTest, PreservesDbRefNamespace) {
    const std::array paths{FieldPath("a")};
    BSONObjBuilder input;
    input.appendDBRef("a", std::string_view("db\0suffix", 9), OID("507f1f77bcf86cd799439011"));
    const auto doc = input.obj();
    withDocumentViews(doc, [&](auto inputView) {
        const auto values = extractScalarFieldValues(inputView, paths);
        ASSERT_EQ(sbe::value::getBsonDBPointerView(values[0].value).ns,
                  std::string_view("db\0suffix", 9));
    });
}

}  // namespace
}  // namespace mongo::stats
