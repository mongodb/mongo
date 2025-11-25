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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

void checkTime(int expected, int now_time, int period) {
    ASSERT_TRUE(Date_t::fromMillisSinceEpoch(expected) ==
                FTDCUtil::roundTime(Date_t::fromMillisSinceEpoch(now_time), Milliseconds(period)));
}

// Validate time rounding
TEST(FTDCUtilTest, TestRoundTime) {
    checkTime(4, 3, 1);
    checkTime(7, 3, 7);
    checkTime(14, 8, 7);
    checkTime(14, 13, 7);
}

// Validate the MongoS FTDC path is computed correctly from a log file path.
TEST(FTDCUtilTest, TestMongoSPath) {

    std::vector<std::pair<std::string, std::string>> testCases = {
        {"/var/log/mongos.log", "/var/log/mongos.diagnostic.data"},
        {"/var/log/mongos.foo.log", "/var/log/mongos.diagnostic.data"},
        {"/var/log/log_file", "/var/log/log_file.diagnostic.data"},
        {"./mongos.log", "./mongos.diagnostic.data"},
        {"../mongos.log", "../mongos.diagnostic.data"},
        {"c:\\var\\log\\mongos.log", "c:\\var\\log\\mongos.diagnostic.data"},
        {"c:\\var\\log\\mongos.foo.log", "c:\\var\\log\\mongos.diagnostic.data"},
        {"c:\\var\\log\\log_file", "c:\\var\\log\\log_file.diagnostic.data"},
        {"/var/some.log/mongos.log", "/var/some.log/mongos.diagnostic.data"},
        {"/var/some.log/log_file", "/var/some.log/log_file.diagnostic.data"},

        {"foo/mongos.log", "foo/mongos.diagnostic.data"},
    };

    for (const auto& p : testCases) {
        ASSERT_EQUALS(FTDCUtil::getMongoSPath(p.first), p.second);
    }
}

TEST(FTDCUtilTest, applyExtractionConversionsToDocumentWorks) {
    BSONObjBuilder refBuilder;
    refBuilder.append("field1", 10.5);
    refBuilder.append("field2", 1);
    refBuilder.append("field3", INT64_MAX);
    refBuilder.append("field4", BSONElement::kLongLongMaxPlusOneAsDouble);
    double lessThanInt64Min = -9223372036854777856.0;
    refBuilder.append("field5", lessThanInt64Min);

    BSONObjBuilder subObjBuilder1;
    subObjBuilder1.append("subField1", true);
    subObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));

    refBuilder.append("field6", subObjBuilder1.obj());

    BSONObjBuilder subObjBuilder2;
    subObjBuilder2.append("subField3", Timestamp(100, 10));
    subObjBuilder2.append("subField4", std::nan(""));

    refBuilder.appendArray("field7", subObjBuilder2.obj());


    BSONObjBuilder expectedBuilder;
    expectedBuilder.append("field1", 10.5);
    expectedBuilder.append("field2", 1);
    expectedBuilder.append("field3", INT64_MAX);
    expectedBuilder.append("field4", INT64_MAX);
    expectedBuilder.append("field5", INT64_MIN);

    BSONObjBuilder expectedSubObjBuilder1;
    expectedSubObjBuilder1.append("subField1", true);
    expectedSubObjBuilder1.append("subField2", Date_t::fromMillisSinceEpoch(100000000000000));

    expectedBuilder.append("field6", expectedSubObjBuilder1.obj());

    BSONObjBuilder expectedSubObjBuilder2;
    expectedSubObjBuilder2.append("subField3", Timestamp(100, 10));
    expectedSubObjBuilder2.append("subField4", 0);

    expectedBuilder.appendArray("field7", expectedSubObjBuilder2.obj());

    auto swResultDoc = FTDCBSONUtil::applyExtractionConversionsToDocument(refBuilder.obj());
    ASSERT_OK(swResultDoc);

    BSONObj resultDoc = swResultDoc.getValue();

    ASSERT_BSONOBJ_EQ(expectedBuilder.obj(), resultDoc);
}

TEST(FTDCUtilTest, applyExtractionConversionsToDocumentFailsAtMaxRecursion) {
    BSONObjBuilder builder1;
    builder1.append("1", 1);

    BSONObjBuilder builder2;
    builder2.append("2", builder1.obj());
    BSONObjBuilder builder3;
    builder3.append("3", builder2.obj());
    BSONObjBuilder builder4;
    builder4.append("4", builder3.obj());
    BSONObjBuilder builder5;
    builder5.append("5", builder4.obj());
    BSONObjBuilder builder6;
    builder6.append("6", builder5.obj());
    BSONObjBuilder builder7;
    builder7.append("7", builder6.obj());
    BSONObjBuilder builder8;
    builder8.append("8", builder7.obj());
    BSONObjBuilder builder9;
    builder9.append("9", builder8.obj());
    BSONObjBuilder builder10;
    builder10.append("10", builder9.obj());
    BSONObjBuilder builder11;
    builder11.append("11", builder10.obj());

    BSONObj nestedObj = builder11.obj();

    auto swOkResultDoc = FTDCBSONUtil::applyExtractionConversionsToDocument(nestedObj);
    ASSERT_OK(swOkResultDoc);

    BSONObjBuilder builder12;
    builder12.append("12", nestedObj);

    auto swNotOkResultDoc = FTDCBSONUtil::applyExtractionConversionsToDocument(builder12.obj());
    ASSERT_NOT_OK(swNotOkResultDoc);
}

}  // namespace mongo
