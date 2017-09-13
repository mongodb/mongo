/**
 *    Copyright 2016 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnEmptyDocument) {
    auto status = WriteConcernOptions().parse({});
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("write concern object cannot be empty", status.reason());
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidJValue) {
    auto status = WriteConcernOptions().parse(BSON("j"
                                                   << "abc"));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("j must be numeric or a boolean value", status.reason());
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnInvalidFSyncValue) {
    auto status = WriteConcernOptions().parse(BSON("fsync"
                                                   << "abc"));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("fsync must be numeric or a boolean value", status.reason());
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseIfBothJAndFSyncAreTrue) {
    auto status = WriteConcernOptions().parse(BSON("j" << true << "fsync" << true));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("fsync and j options cannot be used together", status.reason());
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToJournelIfJIsTrue) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("j" << true)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::JOURNAL == options.syncMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToFSyncIfFSyncIsTrue) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("fsync" << true)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::FSYNC == options.syncMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsSyncModeToNoneIfJIsFalse) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("j" << false)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::NONE == options.syncMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseLeavesSyncModeAsUnsetIfFSyncIsFalse) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("fsync" << false)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseIfWIsNotNumberOrString) {
    auto status = WriteConcernOptions().parse(BSON("w" << BSONObj()));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("w has to be a number or a string", status.reason());
}

TEST(WriteConcernOptionsTest, ParseSetsWNumNodesIfWIsANumber) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("w" << 3)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS(3, options.wNumNodes);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseSetsWTimeoutToZeroIfWTimeoutIsNotANumber) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("wtimeout"
                                 << "abc")));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS(0, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseWTimeoutAsNumber) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON("wtimeout" << 123)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS(123, options.wTimeout);
}

TEST(WriteConcernOptionsTest, ParseReturnsFailedToParseOnUnknownField) {
    auto status = WriteConcernOptions().parse(BSON("x" << 123));
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status);
    ASSERT_EQUALS("unrecognized write concern field: x", status.reason());
}

void _testIgnoreWriteConcernField(const char* fieldName) {
    WriteConcernOptions options;
    ASSERT_OK(options.parse(BSON(fieldName << 1)));
    ASSERT_TRUE(WriteConcernOptions::SyncMode::UNSET == options.syncMode);
    ASSERT_EQUALS("", options.wMode);
    ASSERT_EQUALS(1, options.wNumNodes);
    ASSERT_EQUALS(0, options.wTimeout);
}
TEST(WriteConcernOptionsTest, ParseIgnoresSpecialFields) {
    _testIgnoreWriteConcernField("wElectionId");
    _testIgnoreWriteConcernField("wOpTime");
    _testIgnoreWriteConcernField("getLastError");
    _testIgnoreWriteConcernField("getlasterror");
    _testIgnoreWriteConcernField("GETLastErrOR");
}

}  // namespace
}  // namespace mongo
