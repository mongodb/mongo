/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/split_vector.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kPattern = "_id";

class SplitVectorTest : public ShardServerTestFixture {
public:
    void setUp() {
        ShardServerTestFixture::setUp();
        DBDirectClient dbclient(operationContext());
        ASSERT_TRUE(dbclient.createCollection(kNss.ns()));

        // Insert 100 documents into the collection so the tests can test splitting with different
        // constraints.
        for (int i = 0; i < 100; i++) {
            BSONObjBuilder builder;
            builder.append(kPattern, i);
            BSONObj obj = builder.obj();
            dbclient.insert(kNss.toString(), obj);
        }
        ASSERT_EQUALS(100ULL, dbclient.count(kNss.toString()));
    }

    const long long& getDocSizeBytes() {
        return docSizeBytes;
    }

private:
    // Number of bytes in each of the same-size documents we insert into the collection.
    const long long docSizeBytes = BSON(kPattern << 1).objsize();
};

namespace {

TEST_F(SplitVectorTest, SplitVectorInHalf) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     boost::none,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 100LL));
    std::vector<BSONObj> expected = {BSON(kPattern << 50)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, ForceSplit) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     true,
                                                                     boost::none,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 6LL,
                                                                     getDocSizeBytes() * 6LL));
    std::vector<BSONObj> expected = {BSON(kPattern << 50)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, MaxChunkObjectsSet) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     10,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 100LL));
    // Unlike the SplitVectorInHalf test, should split at every 10th key.
    std::vector<BSONObj> expected = {BSON(kPattern << 10),
                                     BSON(kPattern << 21),
                                     BSON(kPattern << 32),
                                     BSON(kPattern << 43),
                                     BSON(kPattern << 54),
                                     BSON(kPattern << 65),
                                     BSON(kPattern << 76),
                                     BSON(kPattern << 87),
                                     BSON(kPattern << 98)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, SplitEveryThird) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     boost::none,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 6LL));
    std::vector<BSONObj> expected = {
        BSON(kPattern << 3),  BSON(kPattern << 7),  BSON(kPattern << 11), BSON(kPattern << 15),
        BSON(kPattern << 19), BSON(kPattern << 23), BSON(kPattern << 27), BSON(kPattern << 31),
        BSON(kPattern << 35), BSON(kPattern << 39), BSON(kPattern << 43), BSON(kPattern << 47),
        BSON(kPattern << 51), BSON(kPattern << 55), BSON(kPattern << 59), BSON(kPattern << 63),
        BSON(kPattern << 67), BSON(kPattern << 71), BSON(kPattern << 75), BSON(kPattern << 79),
        BSON(kPattern << 83), BSON(kPattern << 87), BSON(kPattern << 91), BSON(kPattern << 95),
        BSON(kPattern << 99)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, MaxSplitPointsSet) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     3,
                                                                     boost::none,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 6LL));
    // Unlike the SplitEveryThird test, should only return the first 3 split points since
    // maxSplitPoints is 3.
    std::vector<BSONObj> expected = {
        BSON(kPattern << 3), BSON(kPattern << 7), BSON(kPattern << 11)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() || expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, IgnoreMaxChunkObjects) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     10,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 6LL));
    // The "maxChunkObjects"th key (10) is larger than the key count at half the maxChunkSize (3),
    // so it should be ignored.
    std::vector<BSONObj> expected = {
        BSON(kPattern << 3),  BSON(kPattern << 7),  BSON(kPattern << 11), BSON(kPattern << 15),
        BSON(kPattern << 19), BSON(kPattern << 23), BSON(kPattern << 27), BSON(kPattern << 31),
        BSON(kPattern << 35), BSON(kPattern << 39), BSON(kPattern << 43), BSON(kPattern << 47),
        BSON(kPattern << 51), BSON(kPattern << 55), BSON(kPattern << 59), BSON(kPattern << 63),
        BSON(kPattern << 67), BSON(kPattern << 71), BSON(kPattern << 75), BSON(kPattern << 79),
        BSON(kPattern << 83), BSON(kPattern << 87), BSON(kPattern << 91), BSON(kPattern << 95),
        BSON(kPattern << 99)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, NoSplit) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     boost::none,
                                                                     boost::none,
                                                                     getDocSizeBytes() * 1000LL));

    ASSERT_EQUALS(splitKeys.size(), 0UL);
}

TEST_F(SplitVectorTest, MaxChunkSizeSpecified) {
    std::vector<BSONObj> splitKeys = unittest::assertGet(splitVector(operationContext(),
                                                                     kNss,
                                                                     BSON(kPattern << 1),
                                                                     BSON(kPattern << 0),
                                                                     BSON(kPattern << 100),
                                                                     false,
                                                                     boost::none,
                                                                     boost::none,
                                                                     1LL,
                                                                     getDocSizeBytes() * 6LL));
    // If both maxChunkSize and maxChunkSizeBytes are specified, maxChunkSize takes precedence.
    // Since the size of the collection is not yet a megabyte, should not split.
    ASSERT_EQ(splitKeys.size(), 0UL);
}

TEST_F(SplitVectorTest, NoCollection) {
    auto status = splitVector(operationContext(),
                              NamespaceString("dummy", "collection"),
                              BSON(kPattern << 1),
                              BSON(kPattern << 0),
                              BSON(kPattern << 100),
                              false,
                              boost::none,
                              boost::none,
                              boost::none,
                              boost::none)
                      .getStatus();
    ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
}

TEST_F(SplitVectorTest, NoIndex) {
    auto status = splitVector(operationContext(),
                              kNss,
                              BSON("foo" << 1),
                              BSON(kPattern << 0),
                              BSON(kPattern << 100),
                              false,
                              boost::none,
                              boost::none,
                              boost::none,
                              boost::none)
                      .getStatus();
    ASSERT_EQUALS(status.code(), ErrorCodes::IndexNotFound);
}

TEST_F(SplitVectorTest, NoMaxChunkSize) {
    auto status = splitVector(operationContext(),
                              kNss,
                              BSON(kPattern << 1),
                              BSON(kPattern << 0),
                              BSON(kPattern << 100),
                              false,
                              boost::none,
                              boost::none,
                              boost::none,
                              boost::none)
                      .getStatus();
    ASSERT_EQUALS(status.code(), ErrorCodes::InvalidOptions);
}

}  // namespace
}  // namespace mongo
