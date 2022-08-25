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

#include "mongo/platform/basic.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/hasher.h"
#include "mongo/db/json.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/s/concurrency/locker_mongos_client_observer.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace {

class ShardKeyPatternTest : public ServiceContextTest {
protected:
    ShardKeyPatternTest() {
        auto service = getServiceContext();
        service->registerClientObserver(std::make_unique<LockerMongosClientObserver>());
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
    }

    BSONObj queryKey(const ShardKeyPattern& pattern, const BSONObj& query) {
        const NamespaceString nss("foo");

        StatusWith<BSONObj> status = pattern.extractShardKeyFromQuery(_opCtx, nss, query);
        if (!status.isOK())
            return BSONObj();
        return status.getValue();
    }

    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
};

/**
 * Creates OplogEntry with given field values.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                repl::OpTypeEnum opType,
                                NamespaceString nss,
                                BSONObj oField,
                                boost::optional<BSONObj> o2Field = boost::none) {
    return {
        repl::DurableOplogEntry(opTime,                           // optime
                                opType,                           // opType
                                nss,                              // namespace
                                boost::none,                      // uuid
                                boost::none,                      // fromMigrate
                                repl::OplogEntry::kOplogVersion,  // version
                                oField,                           // o
                                o2Field,                          // o2
                                {},                               // sessionInfo
                                boost::none,                      // upsert
                                Date_t(),                         // wall clock time
                                {},                               // statement ids
                                boost::none,    // optime of previous write within same transaction
                                boost::none,    // pre-image optime
                                boost::none,    // post-image optime
                                boost::none,    // ShardId of resharding recipient
                                boost::none,    // _id
                                boost::none)};  // needsRetryImage
}

TEST_F(ShardKeyPatternTest, SingleFieldShardKeyPatternsValidityCheck) {
    ShardKeyPattern s1(BSON("a" << 1));
    ShardKeyPattern s2(BSON("a" << 1.0f));
    ShardKeyPattern s3(BSON("a" << (long long)1L));
    ShardKeyPattern s4(BSON("a"
                            << "hashed"));

    ASSERT_THROWS(ShardKeyPattern(BSONObj()), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << -1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a"
                                       << "1")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a"
                                       << "hash")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("" << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("." << 1)), DBException);
}

TEST_F(ShardKeyPatternTest, CompositeShardKeyPatternsValidityCheck) {
    ShardKeyPattern s1(BSON("a" << 1 << "b" << 1));
    ShardKeyPattern s2(BSON("a" << 1.0f << "b" << 1.0));
    ShardKeyPattern s3(BSON("a" << 1 << "b" << 1.0 << "c" << 1.0f));

    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b"
                                           << "1")),
                  DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "b." << 1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "" << 1.0)), DBException);
}

TEST_F(ShardKeyPatternTest, NestedShardKeyPatternsValidtyCheck) {
    ShardKeyPattern s1(BSON("a.b" << 1));
    ShardKeyPattern s2(BSON("a.b.c.d" << 1.0));
    ShardKeyPattern s3(BSON("a" << 1 << "c.d" << 1.0 << "e.f.g" << 1.0f));
    ShardKeyPattern s4(BSON("a" << 1 << "a.b" << 1.0 << "a.b.c" << 1.0f));

    ASSERT_THROWS(ShardKeyPattern(BSON("a.b" << -1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << BSON("b" << 1))), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a.b." << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a.b.." << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a..b" << 1)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << 1 << "a.b." << 1.0)), DBException);
    ASSERT_THROWS(ShardKeyPattern(BSON("a" << BSON("b" << 1) << "c.d" << 1.0)), DBException);
}

TEST_F(ShardKeyPatternTest, IsShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));

    ASSERT(pattern.isShardKey(BSON("a.b" << 10 << "c" << 30)));
    ASSERT(pattern.isShardKey(BSON("c" << 30 << "a.b" << 10)));

    ASSERT(!pattern.isShardKey(BSON("b" << 10)));
    ASSERT(!pattern.isShardKey(BSON("a" << 10 << "c" << 30)));
    ASSERT(!pattern.isShardKey(BSON("a" << BSON("b" << 10) << "c" << 30)));
}

static BSONObj normKey(const ShardKeyPattern& pattern, const BSONObj& doc) {
    return pattern.normalizeShardKey(doc);
}

TEST_F(ShardKeyPatternTest, NormalizeShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));

    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << 10 << "c" << 30)),
                      BSON("a.b" << 10 << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("c" << 30 << "a.b" << 10)),
                      BSON("a.b" << 10 << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << BSON("$notAndOperator" << 10) << "c" << 30)),
                      BSON("a.b" << BSON("$notAndOperator" << 10) << "c" << 30));
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a.b" << BSON("$gt" << 10) << "c" << 30)),
                      BSON("a.b" << BSON("$gt" << 10) << "c" << 30));

    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("b" << 10)), BSONObj());
    ASSERT_BSONOBJ_EQ(normKey(pattern, BSON("a" << 10 << "c" << 30)), BSONObj());
}

static BSONObj docKey(const ShardKeyPattern& pattern, const BSONObj& doc) {
    return pattern.extractShardKeyFromDoc(doc);
}

static BSONObj docKeyFromOplog(const ShardKeyPattern& pattern, const repl::OplogEntry& entry) {
    return pattern.extractShardKeyFromOplogEntry(entry);
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeySingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{a:{b:10}}"));
    const BSONRegEx regex("abc");
    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("a" << regex << "b"
                                      << "20")),
                      BSON("a" << regex));
    const BSONObj ref = BSON("$ref"
                             << "coll"
                             << "$id" << 1);
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << ref)), BSON("a" << ref));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$dollarPrefixKey:true}}")),
                      fromjson("{a:{$dollarPrefixKey:true}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$gt:10}}")), fromjson("{a:{$gt:10}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{$gt:{$dollarPrefixKey:10}}}}")),
                      fromjson("{a:{$gt:{$dollarPrefixKey:10}}}}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSONObj()), fromjson("{a: null}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{b:10}")), fromjson("{a: null}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("" << 10)), fromjson("{a: null}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[1,2]}")), BSONObj());
    // BSONObjIterator breaks this for now
    // ASSERT_EQUALS(docKey(pattern, BSON("a" << 10 << "a" << 20)), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeyCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:'20', c:30}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("c" << 30 << "b"
                                      << "20"
                                      << "a" << 10)),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:{$dollarPrefixKey:true}}")),
                      fromjson("{a:10, b:{$dollarPrefixKey:true}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:{$gt:20}}")),
                      fromjson("{a:10, b:{$gt:20}}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, b:[1, 2]}")), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{b:20}")), fromjson("{a:null, b:20}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern,
                             BSON("" << 10 << "b"
                                     << "20")),
                      fromjson("{a: null, b:'20'}"));

    // Ordering
    ASSERT_EQUALS(docKey(pattern, BSON("b" << 20 << "a" << 10)).firstElement().numberInt(), 10);
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeyNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{d:[1,2],b:10},c:30,d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    const BSONObj ref = BSON("$ref"
                             << "coll"
                             << "$id" << 1);
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << ref) << "c" << 30)),
                      BSON("a.b" << ref << "c" << 30));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:10, c:30}")), fromjson("{'a.b': null, c: 30}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{d:40}, c:30}")),
                      fromjson("{'a.b': null, c: 30}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:10}, {b:20}], c:30}")), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[10, 20]}, c:30}")), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromOplogUnnested) {
    //
    // Unnested ShardKeyPatterns from oplog entries with CRUD operation
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    auto deleteOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),  // optime
                                      repl::OpTypeEnum::kDelete,           // op type
                                      NamespaceString("a"),                // namespace
                                      BSON("_id" << 1 << "a" << 5));       // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                      repl::OpTypeEnum::kInsert,           // op type
                                      NamespaceString("a"),                // namespace
                                      BSON("_id" << 2 << "a" << 6));       // o
    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(70, 10), 1),  // optime
                                      repl::OpTypeEnum::kUpdate,           // op type
                                      NamespaceString("a"),                // namespace
                                      BSON("_id" << 3),                    // o
                                      BSON("_id" << 3 << "a" << 7));       // o2

    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, deleteOplog), fromjson("{a: 5}"));
    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, insertOplog), fromjson("{a: 6}"));
    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, updateOplog), fromjson("{a: 7}"));
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromOplogNested) {
    //
    // Nested ShardKeyPatterns from oplog entries with CRUD operation
    //

    ShardKeyPattern pattern(BSON("a.b" << 1));
    auto deleteOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),          // optime
                                      repl::OpTypeEnum::kDelete,                   // op type
                                      NamespaceString("a.b"),                      // namespace
                                      BSON("_id" << 1 << "a.b" << 5));             // o
    auto insertOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),          // optime
                                      repl::OpTypeEnum::kInsert,                   // op type
                                      NamespaceString("a.b"),                      // namespace
                                      BSON("_id" << 2 << "a" << BSON("b" << 6)));  // o
    auto updateOplog = makeOplogEntry(repl::OpTime(Timestamp(70, 10), 1),          // optime
                                      repl::OpTypeEnum::kUpdate,                   // op type
                                      NamespaceString("a.b"),                      // namespace
                                      BSON("_id" << 3),                            // o
                                      BSON("_id" << 3 << "a.b" << 7));             // o2

    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, deleteOplog), fromjson("{'a.b': 5}"));
    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, insertOplog), fromjson("{'a.b': 6}"));
    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, updateOplog), fromjson("{'a.b': 7}"));
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromOplogNonCRUD) {
    //
    // Oplogs with non-CRUD op types
    //

    ShardKeyPattern pattern(BSON("a.b" << 1));
    auto noopOplog = makeOplogEntry(repl::OpTime(Timestamp(50, 10), 1),     // optime
                                    repl::OpTypeEnum::kNoop,                // op type
                                    NamespaceString("a.b"),                 // namespace
                                    BSON("_id" << 1 << "a.b" << 5));        // o
    auto commandOplog = makeOplogEntry(repl::OpTime(Timestamp(60, 10), 1),  // optime
                                       repl::OpTypeEnum::kCommand,          // op type
                                       NamespaceString("a.b"),              // namespace
                                       BSON("create"
                                            << "c"));  // o

    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, noopOplog), BSONObj());
    ASSERT_BSONOBJ_EQ(docKeyFromOplog(pattern, commandOplog), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeyDeepNested) {
    //
    // Deeply nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b.c" << 1));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:{c:10}}}")), fromjson("{'a.b.c':10}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:{c:10}}]}")), fromjson("{}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[{c:10}]}}")), fromjson("{}"));

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:{c:[10, 20]}}}")), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{b:[{c:10}, {c:20}]}}")), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:{c:10}},{b:{c:20}}]}")), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{b:[{c:10},{c:20}]},{b:[{c:30},{c:40}]}]}}")),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeyWithNumericFieldName) {
    ShardKeyPattern pattern(BSON("a.0.1" << 1));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{'0':{'1':{shardKey:1}}}}")),
                      fromjson("{'a.0.1':{shardKey:1}}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{'0':[0,1]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{'1':1}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{'0':{'1':['array']}}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:[{'0':{'1':'value'}}]}")), BSONObj());

    // 'a.0.1', 'a.0' here should be considered as a different field. Arrays are allowed for non
    // shard key fields. Note that for all these input objects extractShardKeyFromDoc() should
    // return 'null' since we consider the shard key to be missing in the input object.
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{'a.0.1':'value'}")), fromjson("{'a.0.1':null}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{'0':{'2':'value'}}}")),
                      fromjson("{'a.0.1':null}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{'a.0.1':['value']}")), fromjson("{'a.0.1':null}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{'a.0':{'1':['value']}}")),
                      fromjson("{'a.0.1':null}"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, fromjson("{a:{'0':{'2':['value']}}}")),
                      fromjson("{'a.0.1':null}"));
}

TEST_F(ShardKeyPatternTest, ExtractDocShardKeyHashed) {
    //
    // Hashed ShardKeyPattern
    //

    const std::string value = "12345";
    const BSONObj bsonValue = BSON("" << value);
    const long long hashValue =
        BSONElementHasher::hash64(bsonValue.firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);

    const BSONObj nullBsonValue = BSON("" << BSONNULL);
    const long long nullHashValue = BSONElementHasher::hash64(nullBsonValue.firstElement(),
                                                              BSONElementHasher::DEFAULT_HASH_SEED);

    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << value))), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << value) << "c" << 30)),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("c" << 30 << "b" << value))),
                      BSON("a.b" << hashValue));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("c" << value))),
                      BSON("a.b" << nullHashValue));

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON("b" << BSON_ARRAY(value)))), BSONObj());

    ASSERT_BSONOBJ_EQ(docKey(pattern, BSON("a" << BSON_ARRAY(BSON("b" << value)))), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeySingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:30}")), fromjson("{a:{b:10}}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:{$gt:20}}")), fromjson("{a:10}"));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$gt:10}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10,b:{$invalid:'20'}}")), BSONObj());

    // Doc key extraction shouldn't work with query
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:[10, 20]}, c:30}")), BSONObj());

    // $eq/$or/$and/$all
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:10}}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}}]}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10}},{b:'20'}]}")),
                      fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$all:[10]}}")), fromjson("{a:10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{a:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:10},{a:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$all:[10,10]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{b:'20'}]}")), BSONObj());

    // Regex can't be extracted from query
    const BSONRegEx regex("abc");
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("a" << regex << "b"
                                        << "20")),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeyCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20'}")), fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:'20', c:30}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("c" << 30 << "b"
                                        << "20"
                                        << "a" << 10)),
                      fromjson("{a:10, b:'20'}"));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:[1, 2]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10,b:{$invalid:true}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{b:20}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern,
                               BSON("" << 10 << "b"
                                       << "20")),
                      BSONObj());

    // $eq/$or/$and/$all
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{$eq:10}, b:{$all:['20']}}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10},b:{$eq:'20'}}]}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{$eq:10}},{b:{$eq:'20'}}]}")),
                      fromjson("{a:10, b:'20'}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, b:{$gt:20}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{$eq:10}},{b:'20'}]}")), BSONObj());

    // Ordering
    ASSERT_EQUALS(queryKey(pattern, BSON("b" << 20 << "a" << 10)).firstElement().numberInt(), 10);
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeyNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0f));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:30}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':{$eq:10}, c:30, d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{'a.b':10, c:30, d:40}]}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':{$all:[10]}, c:30, d:40}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10,d:40}, c:30}")),
                      fromjson("{'a.b':10, c:30}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{'a.b':{$eq:10}}, {c:30}]}")),
                      fromjson("{'a.b':10, c:30}"));

    // Nested $eq is actually a document element
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{$eq:10}}, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$and:[{a:{b:{$eq:10}}},{c:30}]}")), BSONObj());

    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{$or:[{a:{b:{$eq:10}}},{c:30}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:10, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:10}, c:{$gt:30}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{d:40}, c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:10}, {b:20}],c:30}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{$eq:[10, 20]}},c:30}")), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeyDeepNested) {
    //
    // Deeply nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b.c" << 1));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{c:10}}}")), fromjson("{'a.b.c':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':10}")), fromjson("{'a.b.c':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':{$eq:10}}")), fromjson("{'a.b.c':10}"));

    // Arrays at any nesting level means we can't extract a shard key
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b.c':[10]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b':[{c:10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:{c:10}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:[{c:10}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:{c:[10, 20]}}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{b:[{c:10}, {c:20}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:{c:10}},{b:{c:20}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{b:[{c:10},{c:20}]},{b:[{c:30},{c:40}]}]}}")),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeyWithNumericFieldNames) {
    ShardKeyPattern pattern(BSON("a.0.0" << 1));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{'0':{'0':10}}}")), fromjson("{'a.0.0':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0.0':10}")), fromjson("{'a.0.0':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0.0':{$eq:10}}")), fromjson("{'a.0.0':10}"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0':{'0':'value'}}")),
                      fromjson("{'a.0.0':'value'}"));

    // Arrays at any nesting level means we can't extract a shard key
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0.0':[10]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0':[{'0':10}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:[{'0':{'0':10}}]}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{'0':[{'0':10}]}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0':{'0':['value']}}")), BSONObj());

    // Missing fields.
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{a:{'0':{'2':['value']}}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.0':{'2':['value']}}")), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{}")), BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractQueryShardKeyHashed) {
    //
    // Hashed ShardKeyPattern
    //

    const std::string value = "12345";
    const BSONObj bsonValue = BSON("" << value);
    const long long hashValue =
        BSONElementHasher::hash64(bsonValue.firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);

    // Hashed works basically the same as non-hashed, but applies the hash function at the end
    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << value)), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << value))), BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << BSON("$eq" << value))),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << value) << "c" << 30)),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("c" << 30 << "b" << value))),
                      BSON("a.b" << hashValue));
    ASSERT_BSONOBJ_EQ(queryKey(pattern,  //
                               BSON("$and" << BSON_ARRAY(BSON("a.b" << BSON("$eq" << value))))),
                      BSON("a.b" << hashValue));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << BSON("$eq" << value)))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << BSON("$gt" << value))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("c" << value))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON("b" << BSON_ARRAY(value)))), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a" << BSON_ARRAY(BSON("b" << value)))), BSONObj());

    pattern = ShardKeyPattern(BSON("a.b"
                                   << "hashed"
                                   << "c.d" << 1));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << value << "c.d" << value)),
                      BSON("a.b" << hashValue << "c.d" << value));
    ASSERT_BSONOBJ_EQ(
        queryKey(pattern, fromjson("{a : {b: '12345', p : 1}, c : {d : '12345', q: 2}}")),
        BSON("a.b" << hashValue << "c.d" << value));

    ASSERT_BSONOBJ_EQ(queryKey(pattern, BSON("a.b" << value)), BSONObj());
    ASSERT_BSONOBJ_EQ(queryKey(pattern, fromjson("{'a.b': [10], 'c.d': 1}")), BSONObj());
}

static bool indexComp(const ShardKeyPattern& pattern, const BSONObj& indexPattern) {
    return pattern.isUniqueIndexCompatible(indexPattern);
}

TEST_F(ShardKeyPatternTest, UniqueIndexCompatibleSingle) {
    //
    // Single field ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1));
    ASSERT(indexComp(pattern, BSON("a" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << -1)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << -1 << "b" << 1)));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "b" << 1)));

    ASSERT(!indexComp(pattern, BSON("b" << 1)));
    ASSERT(!indexComp(pattern, BSON("b" << -1 << "a" << 1)));
}

TEST_F(ShardKeyPatternTest, UniqueIndexCompatibleCompound) {
    //
    // Compound ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a" << 1 << "b" << 1.0));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << 1)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << -1.0)));
    ASSERT(indexComp(pattern, BSON("a" << 1 << "b" << -1.0 << "c" << 1)));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "c" << 1)));

    ASSERT(!indexComp(pattern, BSON("a" << 1)));
    ASSERT(!indexComp(pattern, BSON("b" << 1)));
    ASSERT(!indexComp(pattern, BSON("a" << 1 << "c" << 1.0f)));
    ASSERT(!indexComp(pattern, BSON("b" << -1 << "a" << 1 << "c" << 1)));
}

TEST_F(ShardKeyPatternTest, UniqueIndexCompatibleNested) {
    //
    // Nested ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b" << 1 << "c" << 1.0));
    ASSERT(indexComp(pattern, BSON("a.b" << 1 << "c" << 1.0f)));

    ASSERT(!indexComp(pattern, BSON("a.b" << 1)));
    ASSERT(!indexComp(pattern, BSON("a" << 1 << "c" << -1.0)));
    ASSERT(!indexComp(pattern, BSON("c" << -1 << "a.b" << 1)));
}

TEST_F(ShardKeyPatternTest, UniqueIndexCompatibleHashed) {
    //
    // Hashed ShardKeyPatterns
    //

    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"));

    ASSERT(indexComp(pattern, BSON("a.b" << 1)));
    ASSERT(indexComp(pattern, BSON("a.b" << -1)));
    ASSERT(indexComp(pattern, BSON("a.b" << 1 << "c" << 1)));
    ASSERT(indexComp(pattern, BSON("a.b" << -1 << "c" << 1)));

    ASSERT(indexComp(pattern,
                     BSON("a.b"
                          << "hashed")));

    ASSERT(indexComp(pattern, BSON("_id" << 1)));
    ASSERT(indexComp(pattern, BSON("_id" << -1 << "c" << 1)));

    ASSERT(!indexComp(pattern, BSON("c" << 1)));
    ASSERT(!indexComp(pattern, BSON("c" << -1 << "a.b" << 1)));
}

TEST_F(ShardKeyPatternTest, IsHashedPattern) {
    ASSERT(ShardKeyPattern(BSON("a.b"
                                << "hashed"))
               .isHashedPattern());
    ASSERT(ShardKeyPattern(BSON("a.b" << 1 << "c"
                                      << "hashed"
                                      << "d" << 1))
               .isHashedPattern());
    ASSERT(!ShardKeyPattern(BSON("a.b" << 1 << "d" << 1)).isHashedPattern());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_WithNumericFieldNames) {
    ShardKeyPattern pattern(BSON("a.0.1" << 1 << "c.d.0"
                                         << "hashed"));
    auto indexPattern = BSON("a.0.1" << 1.0 << "p" << 1 << "c.d.0"
                                     << "hashed");
    auto indexData = BSON("" << BSON("field"
                                     << "subObject")
                             << "" << 1 << ""
                             << "c.d.0_value");
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSON("a.0.1" << BSON("field"
                                           << "subObject")
                                   << "c.d.0"
                                   << "c.d.0_value"));

    // If the index provides array value for a field('a.0.1') we should return an empty object.
    pattern = ShardKeyPattern(BSON("a.0.1" << 1 << "c.d.0"
                                           << "hashed"));
    indexPattern = BSON("a.0.1" << 1.0 << "c.d.0"
                                << "hashed");
    indexData = BSON("" << BSON_ARRAY("1") << ""
                        << "c.d.0_value");
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_NonHashedIndexAndShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d" << 1.0));
    auto indexPattern = BSON("c.d" << 1.0 << "p"
                                   << "hashed"
                                   << "a.b" << 1);
    auto indexData = BSON("" << BSON("field"
                                     << "subObject")
                             << "" << 1 << ""
                             << "ab_value");
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSON("a.b"
                           << "ab_value"
                           << "c.d"
                           << BSON("field"
                                   << "subObject")));
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_HashedIndexAndShardKey) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d"
                                       << "hashed"
                                       << "p.q" << 1));
    auto indexPattern = BSON("c.d"
                             << "hashed"
                             << "p.q" << 1 << "a.b" << 1);
    auto indexData = BSON("" << 123 << "" << BSONNULL << ""
                             << "ab_value");
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSON("a.b"
                           << "ab_value"
                           << "c.d" << 123 << "p.q" << BSONNULL));
}

DEATH_TEST_REGEX(ShardKeyPattern,
                 ExtractShardKeyFromIndexKeyData_WithMissingFieldsInIndex,
                 "Invariant failure.*matchEl") {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d"
                                       << "hashed"
                                       << "p.q.0" << 1));
    auto indexPattern = BSON("c.d"
                             << "hashed"
                             << "p.q" << 1 << "a.b" << 1);
    auto indexData = BSON("" << 123 << "" << BSON_ARRAY("1") << ""
                             << "ab_value");

    // The shard key requires 'p.q.0' field from the index but the index cannot provide it. In this
    // case we cannot extract full shard key and hence we hit an invariant.
    pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}});
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_WithArrayInIndex) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d"
                                       << "hashed"
                                       << "p.q" << 1));
    auto indexPattern = BSON("c.d"
                             << "hashed"
                             << "p" << 1 << "a.b" << 1);
    auto indexData = BSON("" << 123 << "" << 1 << "" << BSON_ARRAY("ab_value"));

    // A shard key field cannot have array values. When we encounter array value from an index, we
    // return an empty object.
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_WithArrayForNonShardKeyFields) {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d"
                                       << "hashed"
                                       << "p.q" << 1));
    auto indexPattern = BSON("c.d"
                             << "hashed"
                             << "p.q" << 1 << "a.b" << 1 << "arrayField" << 1);
    auto indexData = BSON("" << 123 << "" << BSONNULL << ""
                             << "ab_value"
                             << ""
                             << BSON_ARRAY("1"
                                           << "2"));
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSON("a.b"
                           << "ab_value"
                           << "c.d" << 123 << "p.q" << BSONNULL));
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_NonHashedIndexAndHashedShardKey) {
    ShardKeyPattern pattern(BSON("a.b"
                                 << "hashed"
                                 << "c.d" << 1.0 << "e.null" << 1));
    auto indexPattern = BSON("e.null" << 1 << "c.d" << 1.0 << "p"
                                      << "hashed"
                                      << "a.b" << 1);
    auto indexData = BSON("" << BSONNULL << ""
                             << BSON("field"
                                     << "subObject")
                             << "" << 1 << ""
                             << "ab_value");
    auto hashedValue = BSONElementHasher::hash64(BSON(""
                                                      << "ab_value")
                                                     .firstElement(),
                                                 BSONElementHasher::DEFAULT_HASH_SEED);
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}}),
                      BSON("a.b" << hashedValue << "c.d"
                                 << BSON("field"
                                         << "subObject")
                                 << "e.null" << BSONNULL));
}

DEATH_TEST_REGEX(ShardKeyPattern,
                 ExtractShardKeyFromIndexKeyData_HashedIndexAndNonHashedShardKey,
                 "Invariant failure.*isHashedPatternEl") {
    ShardKeyPattern pattern(BSON("a.b" << 1 << "c.d" << 1.0));
    auto indexPattern = BSON("c.d"
                             << "hashed"
                             << "p" << 1 << "a.b" << 1);
    auto indexData = BSON("" << 1 << "" << 1 << ""
                             << "ab_value");

    // The shard key needs raw value of 'c.d' but the index can only provide hashed values. There is
    // no way to re-construct the raw value from the hashed value. So we hit an invariant.
    pattern.extractShardKeyFromIndexKeyData({{indexData, indexPattern}});
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_FromMultipleIndexes) {
    ShardKeyPattern pattern(BSON("a.firstIndex" << 1 << "a.secondIndex" << 1.0 << "c.secondIndex"
                                                << "hashed"
                                                << "null" << 1));
    auto indexPattern1 = BSON("a.firstIndex" << 1 << "null" << 1);
    auto indexData1 = BSON(""
                           << "a_first_val"
                           << "" << BSONNULL);
    auto indexPattern2 = BSON("a.secondIndex" << 1 << "c.secondIndex" << 1);
    auto valueToHash = 23;
    auto indexData2 = BSON(""
                           << "a_sec_val"
                           << "" << valueToHash);
    auto hashedValue = BSONElementHasher::hash64(BSON("" << valueToHash).firstElement(),
                                                 BSONElementHasher::DEFAULT_HASH_SEED);

    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData(
                          {{indexData1, indexPattern1}, {indexData2, indexPattern2}}),
                      BSON("a.firstIndex"
                           << "a_first_val"
                           << "a.secondIndex"
                           << "a_sec_val"
                           << "c.secondIndex" << hashedValue << "null" << BSONNULL));
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromIndexKeyData_FromMultipleIndexesProvidingSameField) {
    ShardKeyPattern pattern(BSON("a.firstIndex" << 1 << "a.secondIndex" << 1.0 << "a.thirdIndex"
                                                << "hashed"
                                                << "null" << 1));
    auto indexPattern1 = BSON("a.firstIndex"
                              << "hashed"
                              << "null" << 1 << "a.thirdIndex" << 1);
    auto valueToHash = 23;
    auto indexData1 = BSON("" << 1 << "" << BSONNULL << "" << valueToHash);
    auto indexPattern2 = BSON("a.firstIndex" << 1 << "a.secondIndex" << 1 << "a.thirdIndex"
                                             << "hashed");

    auto indexData2 = BSON(""
                           << "non_hashed_value"
                           << ""
                           << "a_sec_val"
                           << "" << 1111);
    auto hashedValue = BSONElementHasher::hash64(BSON("" << valueToHash).firstElement(),
                                                 BSONElementHasher::DEFAULT_HASH_SEED);

    // 'a.firstIndex' and 'c.thirdIndex' are provided by both the indexes. For both fields we could
    // the index value that provides non-hashed value.
    ASSERT_BSONOBJ_EQ(pattern.extractShardKeyFromIndexKeyData(
                          {{indexData1, indexPattern1}, {indexData2, indexPattern2}}),
                      BSON("a.firstIndex"
                           << "non_hashed_value"
                           << "a.secondIndex"
                           << "a_sec_val"
                           << "a.thirdIndex" << hashedValue << "null" << BSONNULL));
}

BSONObj getDocumentKey(const BSONObj& obj, const ShardKeyPattern& shardKeyPattern) {
    return dotted_path_support::extractElementsBasedOnTemplate(obj, shardKeyPattern.toBSON());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromDocumentKey_Single) {
    ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("_id" << 10), shardKeyPattern)),
                      BSON("_id" << 10));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("_id" << 10 << "a" << 20), shardKeyPattern)),
                      BSON("_id" << 10));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("b" << 30), shardKeyPattern)),
                      BSON("_id" << BSONNULL));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("_id" << BSON_ARRAY(10)), shardKeyPattern)),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromDocumentKey_Compound) {
    ShardKeyPattern shardKeyPattern(BSON("a" << 1 << "b" << 1));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("a" << 10 << "b" << 20), shardKeyPattern)),
                      BSON("a" << 10 << "b" << 20));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          BSON("a" << 10 << "b" << 20 << "c" << 30), shardKeyPattern)),
                      BSON("a" << 10 << "b" << 20));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("b" << 20), shardKeyPattern)),
                      BSON("a" << BSONNULL << "b" << 20));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("c" << 30), shardKeyPattern)),
                      BSON("a" << BSONNULL << "b" << BSONNULL));
    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          BSON("a" << BSON_ARRAY(10) << "b" << 20), shardKeyPattern)),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromDocumentKey_Nested) {
    ShardKeyPattern shardKeyPattern(BSON("a.b" << 1 << "c" << 1));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:{b:10}, c:20}"), shardKeyPattern)),
                      fromjson("{'a.b':10, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{d:30}"), shardKeyPattern)),
                      fromjson("{'a.b':null, c:null}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:{b:10}}"), shardKeyPattern)),
                      fromjson("{'a.b':10, c:null}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{c:20}"), shardKeyPattern)),
                      fromjson("{'a.b':null, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:10, c:20}"), shardKeyPattern)),
                      fromjson("{'a.b':null, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:{d:10}, c:20}"), shardKeyPattern)),
                      fromjson("{'a.b':null, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:{b:10, d:30}, c:20}"), shardKeyPattern)),
                      fromjson("{'a.b':10, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          fromjson("{a:{b:{y:10,z:11}}, c:20, d:30}"), shardKeyPattern)),
                      fromjson("{'a.b':{y:10,z:11}, c:20}"));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(fromjson("{a:{b:[10,11]}, c:20, d:30}"), shardKeyPattern)),
                      BSONObj());
}

TEST_F(ShardKeyPatternTest, ExtractShardKeyFromDocumentKey_Hashed) {
    const std::string value = "12345";
    const BSONObj bsonValue = BSON("" << value);
    const long long hashValue =
        BSONElementHasher::hash64(bsonValue.firstElement(), BSONElementHasher::DEFAULT_HASH_SEED);

    const BSONObj nullBsonValue = BSON("" << BSONNULL);
    const long long nullHashValue = BSONElementHasher::hash64(nullBsonValue.firstElement(),
                                                              BSONElementHasher::DEFAULT_HASH_SEED);

    ShardKeyPattern shardKeyPattern(BSON("a.b"
                                         << "hashed"
                                         << "c" << 1));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          BSON("a" << BSON("b" << value) << "c" << 20), shardKeyPattern)),
                      BSON("a.b" << hashValue << "c" << 20));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          BSON("a" << BSON("b" << value << "z" << 11) << "c" << 20 << "d" << 30),
                          shardKeyPattern)),
                      BSON("a.b" << hashValue << "c" << 20));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(
                          getDocumentKey(BSON("c" << 20), shardKeyPattern)),
                      BSON("a.b" << nullHashValue << "c" << 20));

    ASSERT_BSONOBJ_EQ(shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
                          BSON("a" << BSON("z" << value) << "c" << 20), shardKeyPattern)),
                      BSON("a.b" << nullHashValue << "c" << 20));

    ASSERT_BSONOBJ_EQ(
        shardKeyPattern.extractShardKeyFromDocumentKey(getDocumentKey(
            BSON("a" << BSON("b" << BSON_ARRAY(value)) << "c" << 20), shardKeyPattern)),
        BSONObj());
}

}  // namespace
}  // namespace mongo
