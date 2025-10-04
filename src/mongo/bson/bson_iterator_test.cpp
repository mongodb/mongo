/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <ostream>
#include <string>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {
using namespace mongo;

class BSONIterateTest : public unittest::Test {
public:
    BSONObj obj;
    void setUp() override {}
    void testNonStlIterator() {
        int count = obj.nFields();
        int size = obj.objsize();

        BSONObjIterator i(obj);
        while (i.more()) {
            BSONElement e = i.next();
            --count;
            size -= e.size();
        }
        ASSERT_EQ(count, 0);
        ASSERT_EQ(size, BSONObj().objsize());
    }

    void testStlIterator() {
        int count = obj.nFields();
        int size = obj.objsize();

        for (auto&& e : obj) {
            --count;
            size -= e.size();
        }
        ASSERT_EQ(count, 0);
        ASSERT_EQ(size, BSONObj().objsize());
    }

    void testIterators() {
        testNonStlIterator();
        testStlIterator();
        std::cout << "testIterators passed: " << obj.nFields() << "fields, total size "
                  << obj.objsize() << std::endl;
    }
};

const std::string longString =
    "To all to whom these Presents shall come, we, the undersigned Delegates of the States affixed "
    "to our Names send greeting. Whereas the Delegates of the United States of America in Congress "
    "assembled did on the fifteenth day of November in the year of our Lord One Thousand Seven "
    "Hundred and Seventy seven, and in the Second Year of the Independence of America agree to "
    "certain articles of Confederation and perpetual Union between the States of Newhampshire, "
    "Massachusetts-bay, Rhodeisland and Providence Plantations, Connecticut, New York, New Jersey, "
    "Pennsylvania, Delaware, Maryland, Virginia, North Carolina, South Carolina, and Georgia in "
    "the Words following, viz. “Articles of Confederation and perpetual Union between the States "
    "of Newhampshire, Massachusetts-bay, Rhodeisland and Providence Plantations, Connecticut, New "
    "York, New Jersey, Pennsylvania, Delaware, Maryland, Virginia, North Carolina, South Carolina, "
    "and Georgia.";

TEST_F(BSONIterateTest, AllTypesSimple) {
    obj = BSON(
        "1float" << 1.5  // 64-bit binary floating point
                 << "2string"
                 << "Hello"                                           // UTF-8 string
                 << "3document" << BSON("a" << 1)                     // Embedded document
                 << "4array" << BSON_ARRAY(1 << 2)                    // Array
                 << "5bindata" << BSONBinData("", 0, BinDataGeneral)  // Binary data
                 << "6undefined" << BSONUndefined  // Undefined (value) -- Deprecated
                 << "7objectid" << OID("deadbeefdeadbeefdeadbeef")  // ObjectId
                 << "8boolean" << true                              // Boolean
                 << "9datetime" << DATENOW                          // UTC datetime
                 << "10null" << BSONNULL                            // Null value
                 << "11regex" << BSONRegEx("reg.ex")                // Regular Expression
                 << "12dbref"
                 << BSONDBRef("db", OID("dbdbdbdbdbdbdbdbdbdbdbdb"))  // DBPointer -- Deprecated
                 << "13code" << BSONCode("(function(){})();")         // JavaScript code
                 << "14symbol" << BSONSymbol("symbol")                // Symbol. Deprecated
                 << "15code_w_s"
                 << BSONCodeWScope("(function(){})();", BSON("a" << 1))  // JavaScript code w/ scope
                 << "16int" << 42                                        // 32-bit integer
                 << "17timestamp" << Timestamp(1, 2)                     // Timestamp
                 << "18long" << 0x0123456789abcdefll                     // 64-bit integer
                 << "19decimal" << Decimal128("0.30")  // 128-bit decimal floating point
                 << "127maxkey" << MAXKEY              // MaxKey value
                 << "255minkey" << MINKEY              // MinKey value
    );
    testIterators();
}

TEST_F(BSONIterateTest, AllTypesLong) {
    obj =
        BSON("1float" + longString
             << 1.5                                   // 64-bit binary floating point
             << "2string" + longString << longString  // UTF-8 string
             << "3doc" + longString << BSON(longString << longString)          // Embedded document
             << "4array" + longString << BSON_ARRAY(1 << 2)                    // Array
             << "5bindata" + longString << BSONBinData("", 0, BinDataGeneral)  // Binary data
             << "6undefined" + longString << BSONUndefined  // Undefined (value) -- Deprecated
             << "7objectid" + longString << OID("deadbeefdeadbeefdeadbeef")  // ObjectId
             << "8boolean" + longString << true                              // Boolean
             << "9datetime" + longString << DATENOW                          // UTC datetime
             << "10null" + longString << BSONNULL                            // Null value
             << "11regex" + longString
             << BSONRegEx("reg.ex" + longString, "i")  // Regular Expression
             << "12dbref" + longString
             << BSONDBRef("db" + longString,
                          OID("dbdbdbdbdbdbdbdbdbdbdbdb"))  // DBPointer -- Deprecated
             << "13code" + longString
             << BSONCode("(function(){})(); // " + longString)                // JavaScript code
             << "14symbol" + longString << BSONSymbol("symbol" + longString)  // Symbol. Deprecated
             << "15code_w_s" + longString
             << BSONCodeWScope("(function(){})(); //" + longString,
                               BSON(longString << longString))  // JavaScript code w/ scope
             << "16int" + longString << 42                      // 32-bit integer
             << "17timestamp" + longString << Timestamp(1, 2)   // Timestamp
             << "18long" + longString << 0x0123456789abcdefll   // 64-bit integer
             << "19decimal" + longString << Decimal128("0.30")  // 128-bit decimal floating point
             << "127maxkey" + longString << MAXKEY              // MaxKey value
             << "255minkey" + longString << MINKEY              // MinKey value
        );
    testIterators();
}


}  // namespace
