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

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/split_vector.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const NamespaceString kNss = NamespaceString("foo", "bar");
const std::string kPattern = "_id";

void setUnshardedFilteringMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetDb autoDb(opCtx, nss.dbName(), MODE_IX);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
    CollectionShardingRuntime::get(opCtx, nss)->setFilteringMetadata(opCtx, CollectionMetadata());
}

class SplitVectorTest : public ShardServerTestFixture {
public:
    void setUp() {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            uassertStatusOK(
                createCollection(operationContext(), kNss.dbName(), BSON("create" << kNss.coll())));
        }
        setUnshardedFilteringMetadata(opCtx, kNss);
        DBDirectClient client(opCtx);
        client.createIndex(kNss.ns(), BSON(kPattern << 1));

        // Insert 100 documents into the collection so the tests can test splitting with different
        // constraints.
        for (int i = 0; i < 100; i++) {
            BSONObjBuilder builder;
            builder.append(kPattern, i);
            BSONObj obj = builder.obj();
            client.insert(kNss.toString(), obj);
        }
        ASSERT_EQUALS(100ULL, client.count(kNss));
    }

    const long long& getDocSizeBytes() {
        return docSizeBytes;
    }

private:
    // Number of bytes in each of the same-size documents we insert into the collection.
    const long long docSizeBytes = BSON(kPattern << 1).objsize();
};

TEST_F(SplitVectorTest, SplitVectorInHalf) {
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 boost::none,
                                                 boost::none,
                                                 getDocSizeBytes() * 100LL);
    std::vector<BSONObj> expected = {BSON(kPattern << 50)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, ForceSplit) {
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 true,
                                                 boost::none,
                                                 boost::none,
                                                 getDocSizeBytes() * 6LL);
    std::vector<BSONObj> expected = {BSON(kPattern << 50)};
    ASSERT_EQ(splitKeys.size(), expected.size());

    for (auto splitKeysIt = splitKeys.begin(), expectedIt = expected.begin();
         splitKeysIt != splitKeys.end() && expectedIt != expected.end();
         ++splitKeysIt, ++expectedIt) {
        ASSERT_BSONOBJ_EQ(*splitKeysIt, *expectedIt);
    }
}

TEST_F(SplitVectorTest, MaxChunkObjectsSet) {
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 boost::none,
                                                 10,
                                                 getDocSizeBytes() * 100LL);
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
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 boost::none,
                                                 boost::none,
                                                 getDocSizeBytes() * 6LL);
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
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 3,
                                                 boost::none,
                                                 getDocSizeBytes() * 6LL);
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
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 boost::none,
                                                 10,
                                                 getDocSizeBytes() * 6LL);
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
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kNss,
                                                 BSON(kPattern << 1),
                                                 BSON(kPattern << 0),
                                                 BSON(kPattern << 100),
                                                 false,
                                                 boost::none,
                                                 boost::none,
                                                 getDocSizeBytes() * 1000LL);

    ASSERT_EQUALS(splitKeys.size(), 0UL);
}

TEST_F(SplitVectorTest, NoCollection) {
    ASSERT_THROWS_CODE(splitVector(operationContext(),
                                   NamespaceString("dummy", "collection"),
                                   BSON(kPattern << 1),
                                   BSON(kPattern << 0),
                                   BSON(kPattern << 100),
                                   false,
                                   boost::none,
                                   boost::none,
                                   boost::none),
                       DBException,
                       ErrorCodes::NamespaceNotFound);
}

TEST_F(SplitVectorTest, NoIndex) {
    ASSERT_THROWS_CODE(splitVector(operationContext(),
                                   kNss,
                                   BSON("foo" << 1),
                                   BSON(kPattern << 0),
                                   BSON(kPattern << 100),
                                   false,
                                   boost::none,
                                   boost::none,
                                   boost::none),
                       DBException,
                       ErrorCodes::IndexNotFound);
}

TEST_F(SplitVectorTest, NoMaxChunkSize) {
    ASSERT_THROWS_CODE(splitVector(operationContext(),
                                   kNss,
                                   BSON(kPattern << 1),
                                   BSON(kPattern << 0),
                                   BSON(kPattern << 100),
                                   false,
                                   boost::none,
                                   boost::none,
                                   boost::none),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

const NamespaceString kJumboNss = NamespaceString("foo", "bar2");
const std::string kJumboPattern = "a";

class SplitVectorJumboTest : public ShardServerTestFixture {
public:
    void setUp() {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            uassertStatusOK(createCollection(
                operationContext(), kJumboNss.dbName(), BSON("create" << kJumboNss.coll())));
        }
        setUnshardedFilteringMetadata(opCtx, kJumboNss);
        DBDirectClient client(opCtx);
        client.createIndex(kJumboNss.ns(), BSON(kJumboPattern << 1));

        // Insert 10000 documents into the collection with the same shard key value.
        BSONObjBuilder builder;
        builder.append(kJumboPattern, 1);
        BSONObj obj = builder.obj();
        for (int i = 0; i < 1000; i++) {
            client.insert(kJumboNss.toString(), obj);
        }
        ASSERT_EQUALS(1000ULL, client.count(kJumboNss));
    }

    const long long& getDocSizeBytes() {
        return docSizeBytes;
    }

private:
    // Number of bytes in each of the same-size documents we insert into the collection.
    const long long docSizeBytes = BSON(kJumboPattern << 1).objsize();
};

TEST_F(SplitVectorJumboTest, JumboChunk) {
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kJumboNss,
                                                 BSON(kJumboPattern << 1),
                                                 BSON(kJumboPattern << 1),
                                                 BSON(kJumboPattern << 2),
                                                 false,
                                                 boost::none,
                                                 boost::none,
                                                 getDocSizeBytes() * 1LL);

    ASSERT_EQUALS(splitKeys.size(), 0UL);
}

const NamespaceString kMaxResponseNss = NamespaceString("foo", "bar3");

// This is not the actual max bytes size -- we are rounding down from 512000.
int maxShardingUnitTestOplogDocSize = 510000;

// We need a number of documents two over the threshold so that we will hit the max size threshold
// before we reach the end of the document scan.
int numDocs = (BSONObjMaxUserSize / maxShardingUnitTestOplogDocSize) + 2;

/**
 * Assert that once the cumulative size of the splitVector BSON objects reaches the max BSON size
 * limit (adding another split point would tip over the limit), the splitVector function returns.
 */
class SplitVectorMaxResponseSizeTest : public ShardServerTestFixture {
public:
    void setUp() {
        ShardServerTestFixture::setUp();

        auto opCtx = operationContext();

        {
            OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE
                unsafeCreateCollection(opCtx);
            uassertStatusOK(createCollection(operationContext(),
                                             kMaxResponseNss.dbName(),
                                             BSON("create" << kMaxResponseNss.coll())));
        }
        setUnshardedFilteringMetadata(opCtx, kMaxResponseNss);
        DBDirectClient client(opCtx);
        client.createIndex(kMaxResponseNss.ns(), BSON("a" << 1));

        for (int i = 0; i < numDocs; ++i) {
            BSONObjBuilder builder;
            // splitVector won't create more than one split key for each unique document, so we must
            // ensure that our documents are unique.
            builder.append("a", createUniqueHalfMegabyteString(i));
            BSONObj obj = builder.obj();
            client.insert(kMaxResponseNss.toString(), obj);
        }
        ASSERT_EQUALS(numDocs, (int)client.count(kMaxResponseNss));
    }

    std::string createUniqueHalfMegabyteString(int uniqueInt) {
        StringBuilder sb;
        for (int i = 0; i < maxShardingUnitTestOplogDocSize; ++i) {
            sb << "a";
        }
        sb << uniqueInt;
        return sb.str();
    }
};

TEST_F(SplitVectorMaxResponseSizeTest, MaxResponseSize) {
    std::vector<BSONObj> splitKeys = splitVector(operationContext(),
                                                 kMaxResponseNss,
                                                 BSON("a" << 1),
                                                 {},
                                                 {},
                                                 false,
                                                 boost::none,
                                                 boost::none,
                                                 1LL);

    ASSERT_EQUALS((int)splitKeys.size(), numDocs - 2);

    int runningDocSize = 0;
    for (const auto& key : splitKeys) {
        ASSERT_LT(key.objsize(), BSONObjMaxUserSize);
        ASSERT_LT(runningDocSize, BSONObjMaxUserSize);
        runningDocSize += key.objsize();
    }

    auto overflowDoc = BSON("a" << createUniqueHalfMegabyteString(100));
    ASSERT_GT(runningDocSize + overflowDoc.objsize(), BSONObjMaxUserSize);
}

}  // namespace
}  // namespace mongo
