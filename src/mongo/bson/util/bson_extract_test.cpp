/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

TEST(ExtractBSON, ExtractField) {
    BSONObj obj = BSON("a" << 1 << "b" << "hello");
    BSONElement element;
    ASSERT_OK(bsonExtractField(obj, "a", &element));
    ASSERT_EQUALS(1, element.Int());
    ASSERT_OK(bsonExtractField(obj, "b", &element));
    ASSERT_EQUALS(std::string("hello"), element.str());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractField(obj, "c", &element));
}

TEST(ExtractBSON, ExtractTypedField) {
    BSONObj obj = BSON("a" << 1 << "b" << "hello");
    BSONElement element;
    ASSERT_OK(bsonExtractTypedField(obj, "a", NumberInt, &element));
    ASSERT_EQUALS(1, element.Int());
    ASSERT_OK(bsonExtractTypedField(obj, "b", String, &element));
    ASSERT_EQUALS(std::string("hello"), element.str());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractTypedField(obj, "c", String, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractTypedField(obj, "a", String, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractTypedField(obj, "b", NumberDouble, &element));
}


TEST(ExtractBSON, ExtractStringField) {
    BSONObj obj = BSON("a" << 1 << "b" << "hello");
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractStringField(obj, "a", &s));
    ASSERT_OK(bsonExtractStringField(obj, "b", &s));
    ASSERT_EQUALS(std::string("hello"), s);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractStringField(obj, "c", &s));
}

TEST(ExtractBSON, ExtractStringFieldWithDefault) {
    BSONObj obj = BSON("a" << 1 << "b" << "hello");
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractStringFieldWithDefault(obj, "a", "default", &s));

    ASSERT_OK(bsonExtractStringFieldWithDefault(obj, "b", "default", &s));
    ASSERT_EQUALS(std::string("hello"), s);
    ASSERT_OK(bsonExtractStringFieldWithDefault(obj, "c", "default", &s));
    ASSERT_EQUALS(std::string("default"), s);
}

TEST(ExtractBSON, ExtractBooleanFieldWithDefault) {
    BSONObj obj1 = BSON("a" << 1 << "b" << "hello"  << "c" << true);
    BSONObj obj2 = BSON("a" << 0 << "b" << "hello"  << "c" << false);
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
