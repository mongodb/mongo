// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/replay/replay_command.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_test_server.h"
#include "mongo/replay/test_packet.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <string>

namespace mongo {

TEST(ReplayCommandTest, TestFind) {
    BSONObj filter = BSON(
        "$and" << BSON_ARRAY(
            BSON("category" << "electronics") <<                       // Match electronics category
            BSON("price" << BSON("$gte" << 500 << "$lte" << 1500)) <<  // Price range: $500-$1500
            BSON("reviews" << BSON("$elemMatch" << BSON("rating" << BSON("$gt" << 4))))
                                              <<  // At least one review with rating > 4
            BSON("discounted" << false)           // Not discounted
            ));
    BSONObj projection = BSON("_id" << 1 << "name" << 1 << "price" << 1 << "supplier.name"
                                    << 1  // Include nested "supplier.name"
    );

    auto command = cmds::find({}, filter, projection);
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
}

TEST(ReplayCommandTest, TestInsert) {
    BSONObj document1 = BSON("_id" << 1 << "name" << "Smartphone" << "category" << "electronics"
                                   << "price" << 800 << "reviews"
                                   << BSON_ARRAY(BSON("rating" << 4.5 << "reviewer" << "John")
                                                 << BSON("rating" << 2 << "reviewer" << "Alice"))
                                   << "stock" << 50 << "discounted" << false << "supplier"
                                   << BSON("name" << "SupplierA" << "region" << "US"));

    BSONObj document2 =
        BSON("_id" << 2 << "name" << "Laptop" << "category" << "electronics" << "price" << 1200
                   << "reviews" << BSON_ARRAY(BSON("rating" << 5 << "reviewer" << "Bob")) << "stock"
                   << 20 << "discounted" << true << "supplier"
                   << BSON("name" << "SupplierB" << "region" << "EU"));

    BSONObj document3 = BSON("_id" << 3 << "name" << "Shoes" << "category" << "fashion" << "price"
                                   << 100 << "reviews"
                                   << BSON_ARRAY(BSON("rating" << 4 << "reviewer" << "Eve")
                                                 << BSON("rating" << 3 << "reviewer" << "Alice"))
                                   << "stock" << 100 << "discounted" << true << "supplier"
                                   << BSON("name" << "SupplierC" << "region" << "US"));

    auto command = cmds::insert({}, BSON_ARRAY(document1 << document2 << document3));
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
}

TEST(ReplayCommandTest, TestAggregate) {
    BSONObj matchStage =
        BSON("$match" << BSON("category" << "electronics" << "price" << BSON("$gte" << 500)));
    BSONObj groupStage =
        BSON("$group" << BSON("_id" << "$category" << "totalStock" << BSON("$sum" << "$stock")));
    BSONObj projectStage =
        BSON("$project" << BSON("category" << "$_id" << "totalStock" << 1 << "_id" << 0));
    BSONArray pipeline = BSON_ARRAY(matchStage << groupStage << projectStage);

    auto command = cmds::aggregate({}, pipeline);
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
}

TEST(ReplayCommandTest, TestSessionStartEvent) {
    auto command = cmds::start({});
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(command.isSessionStart());
}

TEST(ReplayCommandTest, TestSessionEnd) {
    auto command = cmds::stop({});
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(command.isSessionEnd());
}

}  // namespace mongo
