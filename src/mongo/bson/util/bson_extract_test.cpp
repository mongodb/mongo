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
    BSONElement element;
    ASSERT_OK(bsonExtractField(BSON("a" << 1 << "b" << "hello"), "a", &element));
    ASSERT_EQUALS(1, element.Int());

    ASSERT_OK(bsonExtractField(BSON("a" << 1 << "b" << "hello"), "b", &element));
    ASSERT_EQUALS(std::string("hello"), element.str());

    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  bsonExtractField(BSON("a" << 1 << "b" << "hello"), "c", &element));
}

TEST(ExtractBSON, ExtractTypedField) {
    BSONElement element;
    ASSERT_OK(bsonExtractTypedField(BSON("a" << 1 << "b" << "hello"), "a", NumberInt, &element));
    ASSERT_EQUALS(1, element.Int());

    ASSERT_OK(bsonExtractTypedField(BSON("a" << 1 << "b" << "hello"), "b", String, &element));
    ASSERT_EQUALS(std::string("hello"), element.str());

    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  bsonExtractTypedField(BSON("a" << 1 << "b" << "hello"), "c", String, &element));

    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractTypedField(BSON("a" << 1 << "b" << "hello"), "a", String, &element));

    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractTypedField(BSON("a" << 1 << "b" << "hello"), "b", NumberDouble, &element));
}


TEST(ExtractBSON, ExtractStringField) {
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractStringField(BSON("a" << 1 << "b" << "hello"), "a", &s));

    ASSERT_OK(bsonExtractStringField(BSON("a" << 1 << "b" << "hello"), "b", &s));
    ASSERT_EQUALS(std::string("hello"), s);

    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  bsonExtractStringField(BSON("a" << 1 << "b" << "hello"), "c", &s));
}

TEST(ExtractBSON, ExtractStringFieldWithDefault) {
    std::string s;
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractStringFieldWithDefault(BSON("a" << 1 << "b" << "hello"),
                                                    "a", "default", &s));

    ASSERT_OK(bsonExtractStringFieldWithDefault(BSON("a" << 1 << "b" << "hello"), "b", "default", &s));
    ASSERT_EQUALS(std::string("hello"), s);

    ASSERT_OK(bsonExtractStringFieldWithDefault(BSON("a" << 1 << "b" << "hello"), "c", "default", &s));
    ASSERT_EQUALS(std::string("default"), s);
}

TEST(ExtractBSON, ExtractBooleanFieldWithDefault) {
    bool b;
    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 1 << "b" << "hello"  << "c" << true),
                                                 "a", false, &b));
    ASSERT_EQUALS(true, b);

    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 1 << "b" << "hello"  << "c" << true),
                                                 "c", false, &b));
    ASSERT_EQUALS(true, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 0 << "b" << "hello"  << "c" << false),
                                                 "a", true, &b));
    ASSERT_EQUALS(false, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 0 << "b" << "hello"  << "c" << false),
                                                 "c", true, &b));
    ASSERT_EQUALS(false, b);

    b = false;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 0 << "b" << "hello"  << "c" << false),
                                                 "d", true, &b));
    ASSERT_EQUALS(true, b);

    b = true;
    ASSERT_OK(bsonExtractBooleanFieldWithDefault(BSON("a" << 0 << "b" << "hello"  << "c" << false),
                                                 "d", false, &b));
    ASSERT_EQUALS(false, b);

    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractBooleanFieldWithDefault(BSON("a" << 1 << "b" << "hello"  << "c" << true),
                                                     "b", true, &b));
}
