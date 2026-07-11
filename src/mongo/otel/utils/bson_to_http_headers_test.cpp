// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
