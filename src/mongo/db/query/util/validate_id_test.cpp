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

#include "mongo/db/query/util/validate_id.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

BSONElement idElem(const BSONObj& obj) {
    auto e = obj.getField("_id");
    ASSERT_TRUE(e.ok()) << "test helper expects _id to exist in constructed object";
    return e;
}

TEST(ValidIdField, RejectsArray) {
    auto status = validIdField(idElem(BSON("_id" << BSON_ARRAY(1 << 2 << 3))));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::InvalidIdField);
    ASSERT_NE(status.reason().find("array"), std::string::npos);
}

TEST(ValidIdField, RejectsRegex) {
    auto status = validIdField(idElem(BSON("_id" << BSONRegEx("x", ""))));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::InvalidIdField);
    ASSERT_NE(status.reason().find("regex"), std::string::npos);
}

TEST(ValidIdField, RejectsUndefined) {
    auto status = validIdField(idElem(BSON("_id" << BSONUndefined)));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::InvalidIdField);
    ASSERT_NE(status.reason().find("undefined"), std::string::npos);
}

TEST(ValidIdField, AcceptsCommonScalars) {
    ASSERT_OK(validIdField(idElem(BSON("_id" << 42))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << "abc"))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << OID::gen()))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << true))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << BSONNULL))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << Date_t::now()))));
    ASSERT_OK(validIdField(idElem(BSON("_id" << 3.14))));
}

TEST(ValidIdField, AcceptsSimpleObject) {
    ASSERT_OK(validIdField(idElem(BSON("_id" << BSON("a" << 1)))));
}

TEST(ValidIdField, RejectsObjectWithDollarPrefixedField) {
    auto status = validIdField(idElem(BSON("_id" << BSON("$a" << 1))));
    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::DollarPrefixedFieldName);
    ASSERT_TRUE(status.reason().find("_id fields may not contain '$'-prefixed fields:") == 0)
        << "Unexpected reason: " << status.reason();
}
}  // namespace
}  // namespace mongo
