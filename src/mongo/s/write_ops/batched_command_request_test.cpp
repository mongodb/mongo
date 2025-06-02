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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/write_ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <vector>

namespace mongo {
namespace {

TEST(BatchedCommandRequest, BasicInsert) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON("insert" << "test"
                                                 << "documents" << insertArray << "writeConcern"
                                                 << BSON("w" << 1) << "ordered" << true);

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ(insertRequest.getBatchType(), BatchedCommandRequest::BatchType_Insert);
        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns_forTest());
        ASSERT_EQ(std::int64_t(1),
                  std::get<std::int64_t>(insertRequest.getInsertRequest().getWriteConcern()->w));
        ASSERT_TRUE(insertRequest.getInsertRequest().getOrdered());
        ASSERT(!insertRequest.hasShardVersion());
    }
}

TEST(BatchedCommandRequest, InsertWithShardVersion) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    const OID epoch = OID::gen();
    const Timestamp timestamp(2, 2);
    const Timestamp majorAndMinor(1, 2);

    BSONObj origInsertRequestObj =
        BSON("insert" << "test"
                      << "documents" << insertArray << "writeConcern" << BSON("w" << 1) << "ordered"
                      << true << "shardVersion"
                      << BSON("e" << epoch << "t" << timestamp << "v" << majorAndMinor));

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ(insertRequest.getBatchType(), BatchedCommandRequest::BatchType_Insert);
        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns_forTest());
        ASSERT(insertRequest.hasShardVersion());
        ASSERT_EQ(ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 2})).toString(),
                  insertRequest.getShardVersion().toString());
    }
}

TEST(BatchedCommandRequest, InsertCloneWithIds) {
    BatchedCommandRequest batchedRequest([&] {
        write_ops::InsertCommandRequest insertOp(
            NamespaceString::createNamespaceString_forTest("xyz.abc"));
        insertOp.setWriteCommandRequestBase([] {
            write_ops::WriteCommandRequestBase wcb;
            wcb.setOrdered(true);
            wcb.setBypassDocumentValidation(true);
            return wcb;
        }());
        insertOp.setDocuments({BSON("x" << 1), BSON("x" << 2)});
        return insertOp;
    }());

    const auto clonedRequest(BatchedCommandRequest::cloneInsertWithIds(std::move(batchedRequest)));

    ASSERT_EQ(clonedRequest.getBatchType(), BatchedCommandRequest::BatchType_Insert);
    ASSERT_EQ("xyz.abc", clonedRequest.getNS().ns_forTest());
    ASSERT(clonedRequest.getWriteCommandRequestBase().getOrdered());
    ASSERT(clonedRequest.getWriteCommandRequestBase().getBypassDocumentValidation());

    const auto& insertDocs = clonedRequest.getInsertRequest().getDocuments();
    ASSERT_EQ(2u, insertDocs.size());

    ASSERT_EQ(jstOID, insertDocs[0]["_id"].type());
    ASSERT_EQ(1, insertDocs[0]["x"].numberLong());

    ASSERT_EQ(jstOID, insertDocs[1]["_id"].type());
    ASSERT_EQ(2, insertDocs[1]["x"].numberLong());
}

TEST(BatchedCommandRequest, BasicUpdate) {
    BSONObj q = BSON("value" << 1);
    BSONObj u = BSON("value" << 2);
    BSONArray updateArray = BSON_ARRAY(BSON("q" << q << "u" << u));

    BSONObj origUpdateRequestObj =
        BSON("update" << "test"
                      << "updates" << updateArray << "writeConcern" << BSON("w" << 2));

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origUpdateRequestObj, docSeq));
        const auto updateRequest(BatchedCommandRequest::parseUpdate(opMsgRequest));

        ASSERT_EQ(updateRequest.getBatchType(), BatchedCommandRequest::BatchType_Update);
        ASSERT_EQ("TestDB.test", updateRequest.getUpdateRequest().getNamespace().ns_forTest());
        ASSERT(!updateRequest.hasShardVersion());
        ASSERT_TRUE(updateRequest.getUpdateRequest().getOrdered());
        ASSERT_EQ(std::int64_t(2),
                  std::get<std::int64_t>(updateRequest.getUpdateRequest().getWriteConcern()->w));

        const auto& updates = updateRequest.getUpdateRequest().getUpdates();
        ASSERT_EQ(1, updates.size());
        ASSERT_EQ(boost::none, updates[0].getC());
        ASSERT_BSONOBJ_EQ(q, updates[0].getQ());
        ASSERT_EQ(write_ops::UpdateModification::Type::kReplacement, updates[0].getU().type());
        ASSERT_FALSE(updates[0].getUpsert());
        ASSERT_FALSE(updates[0].getMulti());
        ASSERT_EQ(boost::none, updates[0].getSort());
        ASSERT_BSONOBJ_EQ(BSONObj(), updates[0].getHint());
    }
}

TEST(BatchedCommandRequest, MultiUpdate) {
    BSONObj q0 = BSON("value" << 1);
    BSONObj u0 = BSON("value" << 2);
    BSONObj c0 = BSON("test" << 0 << "foo"
                             << "bar");

    BSONObj q1 = BSON("value" << 42);
    std::vector<BSONObj> u1{BSON("$set" << BSON("value1" << 1 << "value2" << 2)),
                            BSON("$project" << BSON("foo" << 1 << "bar" << 1)),
                            BSON("$unset" << BSON("a" << 1 << "b" << 0))};
    BSONObj s1 = BSON("sortField1" << 1 << "sortField2" << -1);

    BSONObj q2 = BSON("value" << "test");
    BSONObj u2 = BSON("$set" << BSON("foo" << "bar") << "$inc" << BSON("qux" << 12));

    BSONArray updateArray =
        BSON_ARRAY(BSON("q" << q0 << "u" << u0 << "c" << c0)
                   << BSON("q" << q1 << "u" << u1 << "multi" << true << "sort" << s1)
                   << BSON("q" << q2 << "u" << u2));

    BSONObj origUpdateRequestObj = BSON("update" << "testUpdate"
                                                 << "updates" << updateArray << "writeConcern"
                                                 << BSON("w" << 1) << "ordered" << true);

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origUpdateRequestObj, docSeq));
        const auto updateRequest(BatchedCommandRequest::parseUpdate(opMsgRequest));

        ASSERT_EQ(updateRequest.getBatchType(), BatchedCommandRequest::BatchType_Update);
        ASSERT_EQ("TestDB.testUpdate",
                  updateRequest.getUpdateRequest().getNamespace().ns_forTest());
        ASSERT(!updateRequest.hasShardVersion());
        ASSERT_TRUE(updateRequest.getUpdateRequest().getOrdered());
        ASSERT_EQ(std::int64_t(1),
                  std::get<std::int64_t>(updateRequest.getUpdateRequest().getWriteConcern()->w));

        const auto& updates = updateRequest.getUpdateRequest().getUpdates();
        ASSERT_EQ(3, updates.size());
        ASSERT_BSONOBJ_EQ(c0, *updates[0].getC());
        ASSERT_BSONOBJ_EQ(q0, updates[0].getQ());
        ASSERT_EQ(write_ops::UpdateModification::Type::kReplacement, updates[0].getU().type());
        ASSERT_FALSE(updates[0].getUpsert());
        ASSERT_FALSE(updates[0].getMulti());
        ASSERT_EQ(boost::none, updates[0].getSort());
        ASSERT_BSONOBJ_EQ(BSONObj(), updates[0].getHint());
        ASSERT_EQ(boost::none, updates[0].getCollation());

        ASSERT_EQ(boost::none, updates[1].getC());
        ASSERT_BSONOBJ_EQ(q1, updates[1].getQ());
        ASSERT_EQ(write_ops::UpdateModification::Type::kPipeline, updates[1].getU().type());
        ASSERT_EQ(3, updates[1].getU().getUpdatePipeline().size());
        ASSERT_FALSE(updates[1].getUpsert());
        ASSERT_TRUE(updates[1].getMulti());
        ASSERT_BSONOBJ_EQ(s1, *updates[1].getSort());
        ASSERT_BSONOBJ_EQ(BSONObj(), updates[1].getHint());
        ASSERT_EQ(boost::none, updates[1].getCollation());

        ASSERT_EQ(boost::none, updates[2].getC());
        ASSERT_BSONOBJ_EQ(q2, updates[2].getQ());
        ASSERT_EQ(write_ops::UpdateModification::Type::kModifier, updates[2].getU().type());
        ASSERT_BSONOBJ_EQ(u2, updates[2].getU().getUpdateModifier());
        ASSERT_FALSE(updates[2].getUpsert());
        ASSERT_FALSE(updates[2].getMulti());
        ASSERT_EQ(boost::none, updates[2].getSort());
        ASSERT_BSONOBJ_EQ(BSONObj(), updates[2].getHint());
        ASSERT_EQ(boost::none, updates[2].getCollation());
    }
}


}  // namespace
}  // namespace mongo
