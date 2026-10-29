// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
