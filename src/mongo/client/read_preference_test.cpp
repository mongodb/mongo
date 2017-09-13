/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace {

using namespace mongo;

static const Seconds kMinMaxStaleness = ReadPreferenceSetting::kMinimalMaxStalenessValue;

void checkParse(const BSONObj& rpsObj, const ReadPreferenceSetting& expected) {
    const auto swRps = ReadPreferenceSetting::fromInnerBSON(rpsObj);
    ASSERT_OK(swRps.getStatus());
    const auto rps = swRps.getValue();
    ASSERT_TRUE(rps.equals(expected));
}

TEST(ReadPreferenceSetting, ParseValid) {
    checkParse(BSON("mode"
                    << "primary"),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    // Check that primary with wildcard tags is accepted for backwards compatibility, but
    // that the tags are parsed as the empty TagSet.
    checkParse(BSON("mode"
                    << "primary"
                    << "tags"
                    << BSON_ARRAY(BSONObj())),
               ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet::primaryOnly()));

    checkParse(BSON("mode"
                    << "secondaryPreferred"
                    << "tags"
                    << BSON_ARRAY(BSON("dc"
                                       << "ny"))),
               ReadPreferenceSetting(ReadPreference::SecondaryPreferred,
                                     TagSet(BSON_ARRAY(BSON("dc"
                                                            << "ny")))));
    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << kMinMaxStaleness.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, kMinMaxStaleness));

    checkParse(BSON("mode"
                    << "secondary"
                    << "maxStalenessSeconds"
                    << 0),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly, Seconds(0)));

    checkParse(BSON("mode"
                    << "secondary"
                    << "tags"
                    << BSON_ARRAY(BSON("dc"
                                       << "ny"))
                    << "maxStalenessSeconds"
                    << kMinMaxStaleness.count()),
               ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                     TagSet(BSON_ARRAY(BSON("dc"
                                                            << "ny"))),
                                     kMinMaxStaleness));
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
    auto tagSet = TagSet(BSON_ARRAY(BSON("dc"
                                         << "ca")
                                    << BSON("foo"
                                            << "bar")));
    auto rps = ReadPreferenceSetting(ReadPreference::Nearest, tagSet, kMinMaxStaleness);

    auto unexpected1 =
        ReadPreferenceSetting(ReadPreference::Nearest, TagSet::primaryOnly(), kMinMaxStaleness);
    ASSERT_FALSE(rps.equals(unexpected1));

    auto unexpected2 = ReadPreferenceSetting(
        ReadPreference::Nearest, tagSet, Seconds(kMinMaxStaleness.count() + 1));
    ASSERT_FALSE(rps.equals(unexpected2));
}

TEST(ReadPreferenceSetting, ParseInvalid) {
    // mode primary can not have tags
    checkParseFails(BSON("mode"
                         << "primary"
                         << "tags"
                         << BSON_ARRAY(BSON("foo"
                                            << "bar"))));
    // bad mode
    checkParseFails(BSON("mode"
                         << "khalesi"));

    // no mode
    checkParseFails(BSON("foo"
                         << "bar"));

    // tags not an array
    checkParseFails(BSON("mode"
                         << "nearest"
                         << "tags"
                         << "bad"));

    // maxStalenessSeconds is negative
    checkParseFails(BSON("mode"
                         << "secondary"
                         << "maxStalenessSeconds"
                         << -1));

    // maxStalenessSeconds is NaN
    checkParseFails(BSON("mode"
                         << "secondary"
                         << "maxStalenessSeconds"
                         << "ONE"));

    // maxStalenessSeconds and primary
    checkParseFails(BSON("mode"
                         << "primary"
                         << "maxStalenessSeconds"
                         << kMinMaxStaleness.count()));

    // maxStalenessSeconds is less than min
    checkParseFailsWithError(BSON("mode"
                                  << "primary"
                                  << "maxStalenessSeconds"
                                  << Seconds(kMinMaxStaleness.count() - 1).count()),
                             ErrorCodes::MaxStalenessOutOfRange);

    // maxStalenessSeconds is greater than max
    checkParseFails(BSON("mode"
                         << "secondary"
                         << "maxStalenessSeconds"
                         << Seconds::max().count()));

    checkParseContainerFailsWithError(BSON("$query" << BSON("pang"
                                                            << "pong")
                                                    << "$readPreference"
                                                    << 2),
                                      ErrorCodes::TypeMismatch);
}

void checkRoundtrip(const ReadPreferenceSetting& rps) {
    auto parsed = ReadPreferenceSetting::fromInnerBSON(rps.toInnerBSON());
    ASSERT_OK(parsed.getStatus());
    ASSERT_TRUE(parsed.getValue().equals(rps));
}

TEST(ReadPreferenceSetting, Roundtrip) {
    checkRoundtrip(ReadPreferenceSetting(ReadPreference::Nearest,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca")
                                                           << BSON("foo"
                                                                   << "bar")))));
    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryOnly));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSet()));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::SecondaryOnly,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca"
                                                                << "rack"
                                                                << "bar")))));

    checkRoundtrip(ReadPreferenceSetting(ReadPreference::Nearest,
                                         TagSet(BSON_ARRAY(BSON("dc"
                                                                << "ca")
                                                           << BSON("foo"
                                                                   << "bar"))),
                                         kMinMaxStaleness));
}

}  // namespace
