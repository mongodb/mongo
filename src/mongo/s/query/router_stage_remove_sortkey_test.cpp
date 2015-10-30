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

#include "mongo/s/query/router_stage_remove_sortkey.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/s/query/router_stage_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

TEST(RouterStageRemoveSortKeyTest, RemovesSortKey) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 4 << "$sortKey" << 1 << "b" << 3));
    mockStage->queueResult(BSON("$sortKey" << BSON("" << 3) << "c" << BSON("d"
                                                                           << "foo")));
    mockStage->queueResult(BSON("a" << 3));
    mockStage->queueResult(BSONObj());

    auto sortKeyStage = stdx::make_unique<RouterStageRemoveSortKey>(std::move(mockStage));

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 4 << "b" << 3));

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(),
              BSON("c" << BSON("d"
                               << "foo")));

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue());
    ASSERT_EQ(*thirdResult.getValue(), BSON("a" << 3));

    auto fourthResult = sortKeyStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(fourthResult.getValue());
    ASSERT_EQ(*fourthResult.getValue(), BSONObj());

    auto fifthResult = sortKeyStage->next();
    ASSERT_OK(fifthResult.getStatus());
    ASSERT(!fifthResult.getValue());
}

TEST(RouterStageRemoveSortKeyTest, PropagatesError) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("$sortKey" << 1));
    mockStage->queueError(Status(ErrorCodes::BadValue, "bad thing happened"));

    auto sortKeyStage = stdx::make_unique<RouterStageRemoveSortKey>(std::move(mockStage));

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSONObj());

    auto secondResult = sortKeyStage->next();
    ASSERT_NOT_OK(secondResult.getStatus());
    ASSERT_EQ(secondResult.getStatus(), ErrorCodes::BadValue);
    ASSERT_EQ(secondResult.getStatus().reason(), "bad thing happened");
}

TEST(RouterStageRemoveSortKeyTest, ToleratesMidStreamEOF) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1 << "$sortKey" << 1 << "b" << 1));
    mockStage->queueEOF();
    mockStage->queueResult(BSON("a" << 2 << "$sortKey" << 1 << "b" << 2));

    auto sortKeyStage = stdx::make_unique<RouterStageRemoveSortKey>(std::move(mockStage));

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1 << "b" << 1));

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(!secondResult.getValue());

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(thirdResult.getValue());
    ASSERT_EQ(*thirdResult.getValue(), BSON("a" << 2 << "b" << 2));

    auto fourthResult = sortKeyStage->next();
    ASSERT_OK(fourthResult.getStatus());
    ASSERT(!fourthResult.getValue());
}

TEST(RouterStageRemoveSortKeyTest, RemotesExhausted) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    mockStage->queueResult(BSON("a" << 1 << "$sortKey" << 1 << "b" << 1));
    mockStage->queueResult(BSON("a" << 2 << "$sortKey" << 1 << "b" << 2));
    mockStage->markRemotesExhausted();

    auto sortKeyStage = stdx::make_unique<RouterStageRemoveSortKey>(std::move(mockStage));
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto firstResult = sortKeyStage->next();
    ASSERT_OK(firstResult.getStatus());
    ASSERT(firstResult.getValue());
    ASSERT_EQ(*firstResult.getValue(), BSON("a" << 1 << "b" << 1));
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto secondResult = sortKeyStage->next();
    ASSERT_OK(secondResult.getStatus());
    ASSERT(secondResult.getValue());
    ASSERT_EQ(*secondResult.getValue(), BSON("a" << 2 << "b" << 2));
    ASSERT_TRUE(sortKeyStage->remotesExhausted());

    auto thirdResult = sortKeyStage->next();
    ASSERT_OK(thirdResult.getStatus());
    ASSERT(!thirdResult.getValue());
    ASSERT_TRUE(sortKeyStage->remotesExhausted());
}

TEST(RouterStageRemoveSortKeyTest, ForwardsAwaitDataTimeout) {
    auto mockStage = stdx::make_unique<RouterStageMock>();
    auto mockStagePtr = mockStage.get();
    ASSERT_NOT_OK(mockStage->getAwaitDataTimeout().getStatus());

    auto sortKeyStage = stdx::make_unique<RouterStageRemoveSortKey>(std::move(mockStage));
    ASSERT_OK(sortKeyStage->setAwaitDataTimeout(Milliseconds(789)));

    auto awaitDataTimeout = mockStagePtr->getAwaitDataTimeout();
    ASSERT_OK(awaitDataTimeout.getStatus());
    ASSERT_EQ(789, durationCount<Milliseconds>(awaitDataTimeout.getValue()));
}

}  // namespace

}  // namespace mongo
