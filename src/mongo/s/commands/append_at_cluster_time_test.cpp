/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

/**
 *  Add atClusterTime to a non-empty readConcern.
 */
TEST(ClusterCommands, AddAtClusterTimeNormal) {
    BSONObj command = BSON("aggregate"
                           << "testColl"
                           << "readConcern"
                           << BSON("level"
                                   << "snapshot"));
    BSONObj expectedCommand = BSON("aggregate"
                                   << "testColl"
                                   << "readConcern"
                                   << BSON("level"
                                           << "snapshot"
                                           << "atClusterTime"
                                           << Timestamp(1, 0)));
    BSONObj newCommand = appendAtClusterTime(command, LogicalTime(Timestamp(1, 0)));
    ASSERT_BSONOBJ_EQ(expectedCommand, newCommand);
}

// Adding atClusterTime overwrites an existing afterClusterTime.
TEST(ClusterCommands, AddingAtClusterTimeOverwritesExistingAfterClusterTime) {
    const auto existingAfterClusterTime = Timestamp(1, 1);
    BSONObj command = BSON("aggregate"
                           << "testColl"
                           << "readConcern"
                           << BSON("level"
                                   << "snapshot"
                                   << "afterClusterTime"
                                   << existingAfterClusterTime));

    const auto computedAtClusterTime = Timestamp(2, 1);
    BSONObj expectedCommand = BSON("aggregate"
                                   << "testColl"
                                   << "readConcern"
                                   << BSON("level"
                                           << "snapshot"
                                           << "atClusterTime"
                                           << computedAtClusterTime));

    BSONObj newCommand = appendAtClusterTime(command, LogicalTime(computedAtClusterTime));
    ASSERT_BSONOBJ_EQ(expectedCommand, newCommand);
}

// Add atClusterTime to a standalone readConcern object with level snapshot.
TEST(ClusterCommands, AddAtClusterTimeToReadConcern) {
    BSONObj readConcern = BSON("level"
                               << "snapshot");
    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << Timestamp(1, 0));

    BSONObj newReadConcern =
        appendAtClusterTimeToReadConcern(readConcern, LogicalTime(Timestamp(1, 0)));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newReadConcern);
}

// Adding atClusterTime to a standalone readConcern object overwrites an existing afterClusterTime.
TEST(ClusterCommands, AddingAtClusterTimeToReadConcernOverwritesExistingAfterClusterTime) {
    const auto existingAfterClusterTime = Timestamp(1, 1);
    BSONObj readConcern = BSON("level"
                               << "snapshot"
                               << "afterClusterTime"
                               << existingAfterClusterTime);

    const auto computedAtClusterTime = Timestamp(2, 1);
    BSONObj expectedReadConcern = BSON("level"
                                       << "snapshot"
                                       << "atClusterTime"
                                       << computedAtClusterTime);

    BSONObj newReadConcern =
        appendAtClusterTimeToReadConcern(readConcern, LogicalTime(computedAtClusterTime));
    ASSERT_BSONOBJ_EQ(expectedReadConcern, newReadConcern);
}

}  // namespace
}  // namespace mongo
