/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/replay/replay_command.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/replay/rawop_document.h"
#include "mongo/replay/replay_test_server.h"
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
    BSONObj findCommand = BSON("find" << "test"
                                      << "$db" << "test"
                                      << "filter" << filter << "projection" << projection);
    RawOpDocument opDoc{"find", findCommand};
    ReplayCommand command{opDoc.getDocument()};
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

    BSONObj insertCommand = BSON("insert" << "test" << "$db" << "test" << "documents"
                                          << BSON_ARRAY(document1 << document2 << document3));

    RawOpDocument opDoc{"insert", insertCommand};
    ReplayCommand command{opDoc.getDocument()};
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
    BSONObj aggregateCommand =
        BSON("aggregate" << "test" << "$db" << "test" << "pipeline" << pipeline << "cursor"
                         << BSON("batchSize" << 100));
    RawOpDocument opDoc{"aggregate", aggregateCommand};
    ReplayCommand command{opDoc.getDocument()};
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
}

TEST(ReplayCommandTest, TestStartRecording) {
    BSONObj startRecording =
        BSON("startTrafficRecording"
             << "1.0" << "destination" << "rec" << "lsid"
             << BSON("id" << "UUID(\"a8ac2bdc-5457-4a86-9b1c-b0a3253bc43e\")") << "$db" << "admin");
    RawOpDocument opDoc{"startTrafficRecording", startRecording};
    ReplayCommand command{opDoc.getDocument()};
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
    ASSERT_TRUE(command.isStartRecording());
}

TEST(ReplayCommandTest, TestStopRecording) {
    BSONObj stopRecording =
        BSON("stopTrafficRecording"
             << "1.0" << "lsid" << BSON("id" << "UUID(\"a8ac2bdc-5457-4a86-9b1c-b0a3253bc43e\")")
             << "$db" << "admin");
    RawOpDocument opDoc{"stopTrafficRecording", stopRecording};
    ReplayCommand command{opDoc.getDocument()};
    OpMsgRequest m = command.fetchMsgRequest();
    ASSERT_TRUE(m.body.toString() == command.toString());
    ASSERT_TRUE(command.isStopRecording());
}

}  // namespace mongo
