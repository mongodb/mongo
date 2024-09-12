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
#include <initializer_list>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/query/write_ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/index_version.h"
#include "mongo/s/shard_version_factory.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(BatchedCommandRequest, BasicInsert) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents" << insertArray << "writeConcern"
                                        << BSON("w" << 1) << "ordered" << true);

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns_forTest());
        ASSERT(!insertRequest.hasShardVersion());
    }
}

TEST(BatchedCommandRequest, InsertWithShardVersion) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    const OID epoch = OID::gen();
    const Timestamp timestamp(2, 2);
    const Timestamp majorAndMinor(1, 2);

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents" << insertArray << "writeConcern"
                                        << BSON("w" << 1) << "ordered" << true << "shardVersion"
                                        << BSON("e" << epoch << "t" << timestamp << "v"
                                                    << majorAndMinor));

    for (auto docSeq : {false, true}) {
        const auto opMsgRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));
        const auto insertRequest(BatchedCommandRequest::parseInsert(opMsgRequest));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest().getNamespace().ns_forTest());
        ASSERT(insertRequest.hasShardVersion());
        ASSERT_EQ(ShardVersionFactory::make(ChunkVersion({epoch, timestamp}, {1, 2}),
                                            boost::optional<CollectionIndexes>(boost::none))
                      .toString(),
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

}  // namespace
}  // namespace mongo
