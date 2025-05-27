/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/client/read_preference.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace {

using namespace mongo;

static const Seconds kMinMaxStaleness = ReadPreferenceSetting::kMinimalMaxStalenessValue;

ReadPreferenceSetting parse(const BSONObj& rpsObj) {
    const auto swRps = ReadPreferenceSetting::fromInnerBSON(rpsObj);
    ASSERT_OK(swRps.getStatus());
    return swRps.getValue();
}

void checkParse(const BSONObj& rpsObj, const ReadPreferenceSetting& expected) {
    const auto rps = parse(rpsObj);
    ASSERT_TRUE(rps.equals(expected));
}

TEST(ReadPreferenceSetting, ParseValid) {
    checkParse(BSON("mode" << "primary"),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Check that primary with wildcard tags is accepted for backwards compatibility, but
    // that the tags are parsed as the empty TagSet.
    checkParse(BSON("mode" << "primary"
                           << "tags" << BSON_ARRAY(BSONObj())),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    checkParse(BSON("mode" << "secondaryPreferred"
                           << "tags" << BSON_ARRAY(BSON("dc" << "ny"))),
               ReadPreferenceSetting(ReadPreference::SecondaryPreferred,
                                     TagSet(BSON_ARRAY(BSON("dc" << "ny")))));
    checkParse(BSON("mode" << "secondary"
                           << "maxStalenessSeconds" << kMinMaxStaleness.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, kMinMaxStaleness));

    checkParse(BSON("mode" << "secondary"
                           << "maxStalenessSeconds" << 0),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, Seconds(0)));

    checkParse(BSON("mode" << "secondary"
                           << "tags" << BSON_ARRAY(BSON("dc" << "ny")) << "maxStalenessSeconds"
                           << kMinMaxStaleness.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                     TagSet(BSON_ARRAY(BSON("dc" << "ny"))),
                                     kMinMaxStaleness));

    checkParse(BSON("mode" << "nearest"
                           << "tags" << BSON_ARRAY(BSON("dc" << "ny")) << "maxStalenessSeconds"
                           << kMinMaxStaleness.count() << "hedge" << BSONObj()),
               ReadPreferenceSetting(ReadPreference::Nearest,
                                     TagSet(BSON_ARRAY(BSON("dc" << "ny"))),
                                     kMinMaxStaleness,
                                     HedgingMode()));
}

TEST(ReadPreferenceSetting, ParseHedgingMode) {
    // No hedging mode.
    auto rpsObj = BSON("mode" << "primary");
    auto rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::PrimaryOnly);
    ASSERT_FALSE(rps.hedgingMode.has_value());

    // Implicit opt-in for readPreference mode "nearest" should no longer default to hedging mode
    // enabled.
    rpsObj = BSON("mode" << "nearest");
    rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::Nearest);
    ASSERT_FALSE(rps.hedgingMode.has_value());

    // Default hedging mode.
    rpsObj = BSON("mode" << "primaryPreferred"
                         << "hedge" << BSONObj());
    rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::PrimaryPreferred);
    ASSERT_TRUE(rps.hedgingMode.has_value());
    ASSERT_TRUE(rps.hedgingMode->getEnabled());

    // Input hedging mode.
    rpsObj = BSON("mode" << "nearest"
                         << "hedge" << BSON("enabled" << true));
    rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::Nearest);
    ASSERT_TRUE(rps.hedgingMode.has_value());
    ASSERT_TRUE(rps.hedgingMode->getEnabled());

    rpsObj = BSON("mode" << "nearest"
                         << "hedge" << BSON("enabled" << false));
    rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::Nearest);
    ASSERT_TRUE(rps.hedgingMode.has_value());
    ASSERT_FALSE(rps.hedgingMode->getEnabled());

    rpsObj = BSON("mode" << "primary"
                         << "hedge" << BSON("enabled" << false));
    rps = parse(rpsObj);

    ASSERT_TRUE(rps.pref == ReadPreference::PrimaryOnly);
    ASSERT_TRUE(rps.hedgingMode.has_value());
    ASSERT_FALSE(rps.hedgingMode->getEnabled());
}

TEST(ReadPreferenceSetting, ParseIsPretargeted) {
    // No $_isPretargeted.
    auto rpsObj = BSON("mode" << "primary");
    auto rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::PrimaryOnly);
    ASSERT_FALSE(rps.isPretargeted);

    // $_isPretargeted true.
    rpsObj = BSON("mode" << "secondary"
                         << "$_isPretargeted" << true);
    rps = parse(rpsObj);
    ASSERT_TRUE(rps.pref == ReadPreference::SecondaryOnly);
    ASSERT_TRUE(rps.isPretargeted);
}

void checkParseFails(const BSONObj& rpsObj) {
    auto swRps = ReadPreferenceSetting::fromInnerBSON(rpsObj);
    ASSERT_NOT_OK(swRps.getStatus());
}

void checkParseFailsWithError(const BSONObj& rpsObj, ErrorCodes::Error error) {
    auto swRps = ReadPreferenceSetting::fromInnerBSON(rpsObj);
    ASSERT_NOT_OK(swRps.getStatus());
    ASSERT_EQUALS(swRps.getStatus().code(), error);
}

void checkParseContainerFailsWithError(const BSONObj& rpsObj, ErrorCodes::Error error) {
    auto swRps = ReadPreferenceSetting::fromContainingBSON(rpsObj);
    ASSERT_NOT_OK(swRps.getStatus());
    ASSERT_EQUALS(swRps.getStatus().code(), error);
}

TEST(ReadPreferenceSetting, NonEquality) {
    auto tagSet = TagSet(BSON_ARRAY(BSON("dc" << "ca") << BSON("foo" << "bar")));
    auto hedgingMode = HedgingMode();
    hedgingMode.setEnabled(false);
    auto rps =
        ReadPreferenceSetting(ReadPreference::Nearest, tagSet, kMinMaxStaleness, hedgingMode);

    auto unexpected1 = ReadPreferenceSetting(
        ReadPreference::Nearest, TagSet::primaryOnly(), kMinMaxStaleness, hedgingMode);
    ASSERT_FALSE(rps.equals(unexpected1));

    auto unexpected2 = ReadPreferenceSetting(
        ReadPreference::Nearest, tagSet, Seconds(kMinMaxStaleness.count() + 1), hedgingMode);
    ASSERT_FALSE(rps.equals(unexpected2));

    auto unexpected3 =
        ReadPreferenceSetting(ReadPreference::Nearest, tagSet, kMinMaxStaleness, HedgingMode());
    ASSERT_FALSE(rps.equals(unexpected3));

    auto unexpected4 = ReadPreferenceSetting(ReadPreference::PrimaryOnly,
                                             tagSet,
                                             kMinMaxStaleness,
                                             boost::none,
                                             true /* isPretargeted */);
    ASSERT_FALSE(rps.equals(unexpected4));
}

TEST(ReadPreferenceSetting, ParseInvalid) {
    // mode primary can not have tags
    checkParseFails(BSON("mode" << "primary"
                                << "tags" << BSON_ARRAY(BSON("foo" << "bar"))));
    // bad mode
    checkParseFails(BSON("mode" << "khalesi"));

    // no mode
    checkParseFails(BSON("foo" << "bar"));

    // tags not an array
    checkParseFails(BSON("mode" << "nearest"
                                << "tags"
                                << "bad"));

    // maxStalenessSeconds is negative
    checkParseFails(BSON("mode" << "secondary"
                                << "maxStalenessSeconds" << -1));

    // maxStalenessSeconds is NaN
    checkParseFails(BSON("mode" << "secondary"
                                << "maxStalenessSeconds"
                                << "ONE"));

    // maxStalenessSeconds and primary
    checkParseFails(BSON("mode" << "primary"
                                << "maxStalenessSeconds" << kMinMaxStaleness.count()));

    // maxStalenessSeconds is less than min
    checkParseFailsWithError(BSON("mode" << "primary"
                                         << "maxStalenessSeconds"
                                         << Seconds(kMinMaxStaleness.count() - 1).count()),
                             ErrorCodes::MaxStalenessOutOfRange);

    // maxStalenessSeconds is greater than max
    checkParseFails(BSON("mode" << "secondary"
                                << "maxStalenessSeconds" << Seconds::max().count()));

    // Hedging is not supported for read preference mode "primary".
    checkParseFailsWithError(BSON("mode" << "primary"
                                         << "hedge" << BSONObj()),
                             ErrorCodes::InvalidOptions);

    checkParseContainerFailsWithError(
        BSON("abc" << BSON("pang" << "pong") << "$readPreference" << 2), ErrorCodes::TypeMismatch);

    // $_isPretargeted false.
    checkParseFailsWithError(BSON("mode" << "nearest"
                                         << "$_isPretargeted" << false),
                             ErrorCodes::InvalidOptions);

    // $_isPretargeted wrong type.
    checkParseFailsWithError(BSON("mode" << "primaryPreferred"
                                         << "$_isPretargeted"
                                         << "foo"),
                             ErrorCodes::TypeMismatch);
}

void checkRoundtrip(const ReadPreferenceSetting& rps) {
    auto parsed = ReadPreferenceSetting::fromInnerBSON(rps.toInnerBSON());
    ASSERT_OK(parsed.getStatus());
    ASSERT_TRUE(parsed.getValue().equals(rps));
}

TEST(ReadPreferenceSetting, Roundtrip) {
    checkRoundtrip(
        ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                              TagSet(BSON_ARRAY(BSON("dc" << "ca") << BSON("foo" << "bar")))));
    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSet()));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                         TagSet(BSON_ARRAY(BSON("dc" << "ca"
                                                                     << "rack"
                                                                     << "bar")))));

    checkRoundtrip(
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred,
                              TagSet(BSON_ARRAY(BSON("dc" << "ca") << BSON("foo" << "bar"))),
                              kMinMaxStaleness));

    auto hedgingMode = HedgingMode();
    checkRoundtrip(
        ReadPreferenceSetting(ReadPreference::Nearest,
                              TagSet(BSON_ARRAY(BSON("dc" << "ca") << BSON("foo" << "bar"))),
                              kMinMaxStaleness,
                              hedgingMode));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::SecondaryPreferred,
                                         TagSet(),
                                         kMinMaxStaleness,
                                         boost::none,
                                         true /* isPretargeted */));
}

// Verify TagSet isFlags work correctly
TEST(TagSet, TestFlags) {
    TagSet tsPrimary = TagSet::primaryOnly();
    ASSERT_TRUE(tsPrimary.isPrimaryOnly());
    ASSERT_FALSE(tsPrimary.isMatchAnyNode());

    TagSet tsMatchAny;
    ASSERT_FALSE(tsMatchAny.isPrimaryOnly());
    ASSERT_TRUE(tsMatchAny.isMatchAnyNode());
    ASSERT_FALSE(tsMatchAny == TagSet::primaryOnly());

    TagSet tsSomething(BSON_ARRAY(BSON("x" << 1)));
    ASSERT_FALSE(tsSomething.isPrimaryOnly());
    ASSERT_FALSE(tsSomething.isMatchAnyNode());
    ASSERT_FALSE(tsSomething == TagSet::primaryOnly());
}


}  // namespace
