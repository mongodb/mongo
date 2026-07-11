// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <cmath>
// IWYU pragma: no_include "ext/type_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <string>

using namespace mongo;

TEST(ExtractBSON, ExtractField) {
    BSONObj obj = BSON("a" << 1 << "b"
                           << "hello");
    BSONElement element;
    ASSERT_OK(bsonExtractField(obj, "a", &element));
    ASSERT_EQUALS(1, element.Int());
    ASSERT_OK(bsonExtractField(obj, "b", &element));
    ASSERT_EQUALS(std::string("hello"), element.str());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractField(obj, "c", &element));
}

TEST(ExtractBSON, ExtractTypedField) {
    BSONObj obj = BSON("a" << 1 << "b"
                           << "hello");
    BSONElement element;
    ASSERT_OK(bsonExtractTypedField(obj, "a", BSONType::numberInt, &element));
    ASSERT_EQUALS(1, element.Int());
    ASSERT_OK(bsonExtractTypedField(obj, "b", BSONType::string, &element));
    ASSERT_EQUALS(std::string("hello"), element.str());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  bsonExtractTypedField(obj, "c", BSONType::string, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractTypedField(obj, "a", BSONType::string, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractTypedField(obj, "b", BSONType::numberDouble, &element));
}


TEST(ExtractBSON, ExtractStringField) {
    BSONObj obj = BSON("a" << 1 << "b"
                           << "hello");
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractStringField(obj, "a", &s));
    ASSERT_OK(bsonExtractStringField(obj, "b", &s));
    ASSERT_EQUALS(std::string("hello"), s);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractStringField(obj, "c", &s));
}

TEST(ExtractBSON, ExtractStringFieldWithDefault) {
    BSONObj obj = BSON("a" << 1 << "b"
                           << "hello");
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractStringFieldWithDefault(obj, "a", "default", &s));

    ASSERT_OK(bsonExtractStringFieldWithDefault(obj, "b", "default", &s));
    ASSERT_EQUALS(std::string("hello"), s);
    ASSERT_OK(bsonExtractStringFieldWithDefault(obj, "c", "default", &s));
    ASSERT_EQUALS(std::string("default"), s);
}

TEST(ExtractBSON, ExtractBooleanFieldWithDefault) {
    BSONObj obj1 = BSON("a" << 1 << "b"
                            << "hello"
                            << "c" << true);
    BSONObj obj2 = BSON("a" << 0 << "b"
                            << "hello"
                            << "c" << false);
    bool b;
    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj1, "a", false, &b));
    ASSERT_EQUALS(true, b);

    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj1, "c", false, &b));
    ASSERT_EQUALS(true, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj2, "a", true, &b));
    ASSERT_EQUALS(false, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj2, "c", true, &b));
    ASSERT_EQUALS(false, b);

    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj2, "d", true, &b));
    ASSERT_EQUALS(true, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(obj2, "d", false, &b));
    ASSERT_EQUALS(false, b);

    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractBooleanFieldWithDefault(obj1, "b", true, &b));
}

TEST(ExtractBSON, ExtractIntegerField) {
    long long v;
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractIntegerField(BSON("a" << 1), "b", &v));
    ASSERT_OK(bsonExtractIntegerFieldWithDefault(BSON("a" << 1), "b", -1LL, &v));
    ASSERT_EQUALS(-1LL, v);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractIntegerField(BSON("a" << false), "a", &v));
    ASSERT_EQUALS(
        ErrorCodes::BadValue,
        bsonExtractIntegerField(BSON("a" << std::numeric_limits<float>::quiet_NaN()), "a", &v));
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  bsonExtractIntegerField(BSON("a" << pow(2.0, 64)), "a", &v));
    ASSERT_EQUALS(ErrorCodes::BadValue, bsonExtractIntegerField(BSON("a" << -1.5), "a", &v));
    ASSERT_OK(bsonExtractIntegerField(BSON("a" << -pow(2.0, 55)), "a", &v));
    ASSERT_EQUALS(-(1LL << 55), v);
    ASSERT_OK(bsonExtractIntegerField(BSON("a" << 5178), "a", &v));
    ASSERT_EQUALS(5178, v);
}
