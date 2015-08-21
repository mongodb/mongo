/**
 *    Copyright 2015 MongoDB Inc.
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

#include "mongo/s/query/cluster_client_cursor_impl.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(ClusterClientCursorImpl, NumReturnedSoFar) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    for (int i = 1; i < 10; ++i) {
        mockStage->queueResult(BSON("a" << i));
    }

    ClusterClientCursorImpl cursor(std::move(mockStage));

    ASSERT_EQ(cursor.getNumReturnedSoFar(), 0);

    for (int i = 1; i < 10; ++i) {
        auto result = cursor.next();
        ASSERT(result.isOK());
        ASSERT_EQ(*result.getValue(), BSON("a" << i));
        ASSERT_EQ(cursor.getNumReturnedSoFar(), i);
    }
    // Now check that if nothing is fetched the getNumReturnedSoFar stays the same.
    auto result = cursor.next();
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue());
    ASSERT_EQ(cursor.getNumReturnedSoFar(), 9LL);
}

}  // namespace

}  // namespace mongo
