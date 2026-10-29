// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
