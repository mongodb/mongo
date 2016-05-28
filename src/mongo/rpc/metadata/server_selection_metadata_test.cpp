/*
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

#include <utility>

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/unittest/unittest.h"

namespace {
using namespace mongo;
using namespace mongo::rpc;
using mongo::unittest::assertGet;

ServerSelectionMetadata checkParse(const BSONObj& metadata) {
    return assertGet(ServerSelectionMetadata::readFromMetadata(metadata));
}

TEST(ServerSelectionMetadata, ReadFromMetadata) {
    {
        // Empty object - should work just fine.
        auto ss = checkParse(BSONObj());
        ASSERT_FALSE(ss.isSecondaryOk());
        ASSERT_FALSE(ss.getReadPreference().is_initialized());
    }
    {
        // Set secondaryOk but not readPreference.
        auto ss = checkParse(BSON("$ssm" << BSON("$secondaryOk" << 1)));
        ASSERT_TRUE(ss.isSecondaryOk());
        ASSERT_FALSE(ss.getReadPreference().is_initialized());
    }
    {
        // Set readPreference but not secondaryOk.
        auto ss = checkParse(BSON("$ssm" << BSON("$readPreference" << BSON("mode"
                                                                           << "primary"))));
        ASSERT_FALSE(ss.isSecondaryOk());
        ASSERT_TRUE(ss.getReadPreference().is_initialized());
        ASSERT_TRUE(ss.getReadPreference()->pref == ReadPreference::PrimaryOnly);
    }
    {
        // Set both.
        auto ss = checkParse(BSON("$ssm" << BSON("$secondaryOk" << 1 << "$readPreference"
                                                                << BSON("mode"
                                                                        << "secondaryPreferred"))));
        ASSERT_TRUE(ss.isSecondaryOk());
        ASSERT_TRUE(ss.getReadPreference()->pref == ReadPreference::SecondaryPreferred);
    }
}

void checkUpconvert(const BSONObj& legacyCommand,
                    const int legacyQueryFlags,
                    const BSONObj& upconvertedCommand,
                    const BSONObj& upconvertedMetadata) {
    BSONObjBuilder upconvertedCommandBob;
    BSONObjBuilder upconvertedMetadataBob;
    auto convertStatus = ServerSelectionMetadata::upconvert(
        legacyCommand, legacyQueryFlags, &upconvertedCommandBob, &upconvertedMetadataBob);
    ASSERT_OK(convertStatus);
    // We don't care about the order of the fields in the metadata object
    const auto sorted = [](const BSONObj& obj) {
        BSONObjIteratorSorted iter(obj);
        BSONObjBuilder bob;
        while (iter.more()) {
            bob.append(iter.next());
        }
        return bob.obj();
    };

    ASSERT_EQ(upconvertedCommand, upconvertedCommandBob.done());
    ASSERT_EQ(sorted(upconvertedMetadata), sorted(upconvertedMetadataBob.done()));
}

TEST(ServerSelectionMetadata, UpconvertValidMetadata) {
    // Wrapped in $query, with readPref and slaveOk bit set.
    checkUpconvert(
        BSON("$query" << BSON("ping" << 1) << "$readPreference" << BSON("mode"
                                                                        << "secondary")),
        mongo::QueryOption_SlaveOk,
        BSON("ping" << 1),
        BSON("$ssm" << BSON("$secondaryOk" << 1 << "$readPreference" << BSON("mode"
                                                                             << "secondary"))));

    // Wrapped in 'query', with readPref.
    checkUpconvert(BSON("query" << BSON("pong" << 1 << "foo"
                                               << "bar")
                                << "$readPreference"
                                << BSON("mode"
                                        << "primary"
                                        << "tags"
                                        << BSON("dc"
                                                << "ny"))),
                   0,
                   BSON("pong" << 1 << "foo"
                               << "bar"),
                   BSON("$ssm" << BSON("$readPreference" << BSON("mode"
                                                                 << "primary"
                                                                 << "tags"
                                                                 << BSON("dc"
                                                                         << "ny")))));
    // Unwrapped, no readPref, no slaveOk
    checkUpconvert(BSON("ping" << 1), 0, BSON("ping" << 1), BSONObj());

    // Readpref wrapped in $queryOptions
    checkUpconvert(BSON("pang"
                        << "pong"
                        << "$queryOptions"
                        << BSON("$readPreference" << BSON("mode"
                                                          << "nearest"
                                                          << "tags"
                                                          << BSON("rack"
                                                                  << "city")))),
                   0,
                   BSON("pang"
                        << "pong"),
                   BSON("$ssm" << BSON("$readPreference" << BSON("mode"
                                                                 << "nearest"
                                                                 << "tags"
                                                                 << BSON("rack"
                                                                         << "city")))));
}

void checkUpconvertFails(const BSONObj& legacyCommand, ErrorCodes::Error error) {
    BSONObjBuilder upconvertedCommandBob;
    BSONObjBuilder upconvertedMetadataBob;
    auto upconvertStatus = ServerSelectionMetadata::upconvert(
        legacyCommand, 0, &upconvertedCommandBob, &upconvertedMetadataBob);
    ASSERT_NOT_OK(upconvertStatus);
    ASSERT_EQ(upconvertStatus.code(), error);
}

TEST(ServerSelectionMetadata, UpconvertInvalidMetadata) {
    // $readPreference not an object.
    checkUpconvertFails(BSON("$query" << BSON("pang"
                                              << "pong")
                                      << "$readPreference"
                                      << 2),
                        ErrorCodes::TypeMismatch);

    // has $maxTimeMS option
    checkUpconvertFails(BSON("query" << BSON("foo"
                                             << "bar")
                                     << "$maxTimeMS"
                                     << 200),
                        ErrorCodes::InvalidOptions);
    checkUpconvertFails(BSON("$query" << BSON("foo"
                                              << "bar")
                                      << "$maxTimeMS"
                                      << 200),
                        ErrorCodes::InvalidOptions);

    // has $queryOptions field, but invalid $readPreference
    checkUpconvertFails(BSON("ping"
                             << "pong"
                             << "$queryOptions"
                             << BSON("$readPreference" << 1.2)),
                        ErrorCodes::TypeMismatch);

    // has $queryOptions field, but no $readPreference
    checkUpconvertFails(BSON("ping"
                             << "pong"
                             << "$queryOptions"
                             << BSONObj()),
                        ErrorCodes::NoSuchKey);

    // invalid wrapped query
    checkUpconvertFails(BSON("$query" << 1), ErrorCodes::TypeMismatch);
    checkUpconvertFails(BSON("$query"
                             << ""),
                        ErrorCodes::TypeMismatch);
    checkUpconvertFails(BSON("query" << 1), ErrorCodes::TypeMismatch);
    checkUpconvertFails(BSON("query"
                             << ""),
                        ErrorCodes::TypeMismatch);
}

}  // namespace
