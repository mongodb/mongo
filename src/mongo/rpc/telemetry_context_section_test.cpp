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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/telemetry_context_section_gen.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace match = mongo::unittest::match;

namespace mongo::otel {
namespace {

constexpr auto kValidTraceparent = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

TEST(TelemetryContextSection, ParsesAndRoundTrips) {
    auto input = BSON("otel" << BSON("traceparent" << kValidTraceparent));
    auto section = TelemetryContextSection::parse(input, IDLParserContext{"test"});
    EXPECT_EQ(section.getOtel().getTraceparent(), kValidTraceparent);

    EXPECT_THAT(section.toBSON(), match::BSONObjEQ(input));
}

TEST(TelemetryContextSection, ParseRejectsMissingOtel) {
    EXPECT_THAT(
        [] {
            TelemetryContextSection::parse(
                BSON("not_otel" << BSON(
                         "traceparent"
                         << "00-00000000000000000000000000000000-00f067aa0ba902b7-01")),
                IDLParserContext{"test"});
        },
        match::Throws<DBException>(match::Property(
            &std::exception::what, match::HasSubstr("otel' is missing but a required field"))));
}

TEST(TelemetryContextSection, ParseRejectsMissingTraceparent) {
    EXPECT_THAT(
        [] {
            TelemetryContextSection::parse(
                BSON("otel" << BSON("not_traceparent"
                                    << "00-00000000000000000000000000000000-00f067aa0ba902b7-01")),
                IDLParserContext{"test"});
        },
        match::Throws<DBException>(
            match::Property(&std::exception::what,
                            match::HasSubstr("traceparent' is missing but a required field"))));
}

TEST(OtelContextSection, SetterValidates) {
    auto otel = OtelContextSection{};
    EXPECT_THAT(
        [&] { otel.setTraceparent(std::string{"bogus"}); },
        match::Throws<DBException>(match::Property(&DBException::code, ErrorCodes::BadValue)));
    otel.setTraceparent(std::string{kValidTraceparent});
    EXPECT_EQ(otel.getTraceparent(), kValidTraceparent);
}

}  // namespace
}  // namespace mongo::otel
