/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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


#include "mongo/config.h"

#ifdef MONGO_CONFIG_OTEL

#include "mongo/otel/utils/bson_to_http_headers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::otel {
namespace {
using testing::ElementsAre;
using testing::HasSubstr;
using testing::IsEmpty;
using testing::Pair;
using testing::SizeIs;
using unittest::match::StatusIs;
using unittest::match::StatusWithHasStatus;
using unittest::match::StatusWithHasValue;

StatusWith<HttpHeaderMap> parseHeaders(BSONObj doc) {
    auto b = BSON("v" << doc);
    return parseHttpHeadersFromBson(b.firstElement());
}

TEST(BsonToHttpHeadersTest, StringValue) {
    EXPECT_THAT(parseHeaders(BSON("Authorization" << "Bearer tok")), StatusWithHasValue(SizeIs(1)));

    EXPECT_THAT(parseHeaders(BSON("Authorization" << "Bearer tok" << "X-Tenant-ID" << "acme"
                                                  << "X-Empty-Value" << "")),
                StatusWithHasValue(SizeIs(3)));

    EXPECT_THAT(parseHeaders(BSONObj{}), StatusWithHasValue(IsEmpty()));
}

TEST(BsonToHttpHeadersTest, ArrayValue) {
    EXPECT_THAT(parseHeaders(BSON("X-Multi" << BSON_ARRAY("value1" << "value2"))),
                StatusWithHasValue(ElementsAre(Pair("X-Multi", ElementsAre("value1", "value2")))));
}

TEST(BsonToHttpHeadersTest, DuplicateKeyFails) {
    // BSON allows duplicate field names; the parameter must reject them.
    EXPECT_THAT(parseHeaders(BSON("X-Same" << "first" << "X-Same" << "second")),
                StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("duplicate key"))));
}

TEST(BsonToHttpHeadersTest, EmptyKeyFails) {
    // BSON allows empty field names; the parameter must reject them.
    EXPECT_THAT(parseHeaders(BSON("" << "value")),
                StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("empty key"))));
}

TEST(BsonToHttpHeadersTest, NonObjectElementFails) {
    auto storage = BSON("v" << 42);
    EXPECT_THAT(parseHttpHeadersFromBson(storage.firstElement()),
                StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("BSON document"))));
}

TEST(BsonToHttpHeadersTest, NonStringValueFails) {
    EXPECT_THAT(parseHeaders(BSON("X-Bad" << 5)),
                StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("strings or array"))));
}

TEST(BsonToHttpHeadersTest, ArrayWithNonStringElementFails) {
    EXPECT_THAT(parseHeaders(BSON("X-Bad" << BSON_ARRAY("IntKey" << 42))),
                StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("BSON array"))));
}

TEST(BsonToHttpHeadersTest, InvalidCharactersFails) {
    EXPECT_THAT(
        parseHeaders(BSON("X-Foo\nX-Injected=evil" << "val")),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));
    EXPECT_THAT(
        parseHeaders(BSON("X-Foo\rX-Injected=evil" << "val")),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));

    EXPECT_THAT(
        parseHeaders(BSON("X-Foo" << "val\nX-Injected=evil")),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));
    EXPECT_THAT(
        parseHeaders(BSON("X-Foo" << "val\rX-Injected=evil")),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));

    EXPECT_THAT(
        parseHeaders(BSON("X-Foo" << BSON_ARRAY("ok value" << "other val\nX-Injected=evil"))),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));
    EXPECT_THAT(
        parseHeaders(BSON("X-Foo" << BSON_ARRAY("ok value" << "other val\rX-Injected=evil"))),
        StatusWithHasStatus(StatusIs(ErrorCodes::BadValue, HasSubstr("invalid characters"))));
}
}  // namespace
}  // namespace mongo::otel

#endif
