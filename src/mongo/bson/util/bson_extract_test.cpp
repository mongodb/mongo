/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include <functional>
#include <limits>
#include <string>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/functional.h"
#include "mongo/unittest/unittest.h"

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
    ASSERT_OK(bsonExtractTypedField(obj, "a", NumberInt, &element));
    ASSERT_EQUALS(1, element.Int());
    ASSERT_OK(bsonExtractTypedField(obj, "b", String, &element));
    ASSERT_EQUALS(std::string("hello"), element.str());
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractTypedField(obj, "c", String, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractTypedField(obj, "a", String, &element));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  bsonExtractTypedField(obj, "b", NumberDouble, &element));
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
                            << "c"
                            << true);
    BSONObj obj2 = BSON("a" << 0 << "b"
                            << "hello"
                            << "c"
                            << false);
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
    auto pred = stdx::bind(std::greater<long long>(), stdx::placeholders::_1, 0);
    ASSERT_OK(bsonExtractIntegerFieldWithDefaultIf(BSON("a" << 1), "a", -1LL, pred, &v));
    ASSERT_OK(bsonExtractIntegerFieldWithDefaultIf(BSON("a" << 1), "b", 1LL, pred, &v));
    auto msg = "'a' has to be greater than zero";
    auto status = bsonExtractIntegerFieldWithDefaultIf(BSON("a" << -1), "a", 1LL, pred, msg, &v);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_STRING_CONTAINS(status.reason(), msg);
    ASSERT_EQUALS(ErrorCodes::BadValue,
                  bsonExtractIntegerFieldWithDefaultIf(BSON("a" << 1), "b", -1LL, pred, &v));
}
