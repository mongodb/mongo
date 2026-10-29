// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"

#include <string>

using namespace mongo;

TEST(ExtractBSON, ExtractOpTimeField) {
    // Outer object cases.
    BSONObj obj = BSON("a" << BSON("ts" << Timestamp(10, 0) << "t" << 2LL) << "b"
                           << "notAnObj");
    repl::OpTime opTime;
    ASSERT_OK(bsonExtractOpTimeField(obj, "a", &opTime));
    ASSERT(repl::OpTime(Timestamp(10, 0), 2) == opTime);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractOpTimeField(obj, "b", &opTime));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractOpTimeField(obj, "c", &opTime));

    // Missing timestamp field.
    obj = BSON("a" << BSON("ts" << "notATimestamp"
                                << "t" << 2LL));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractOpTimeField(obj, "a", &opTime));
    // Wrong typed timestamp field.
    obj = BSON("a" << BSON("t" << 2LL));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractOpTimeField(obj, "a", &opTime));
    // Missing term field.
    obj = BSON("a" << BSON("ts" << Timestamp(10, 0) << "t"
                                << "notANumber"));
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, bsonExtractOpTimeField(obj, "a", &opTime));
    // Wrong typed term field.
    obj = BSON("a" << BSON("ts" << Timestamp(10, 0)));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, bsonExtractOpTimeField(obj, "a", &opTime));
}
