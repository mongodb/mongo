/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BatchedCommandRequest, BasicInsert) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents"
                                        << insertArray
                                        << "writeConcern"
                                        << BSON("w" << 1)
                                        << "ordered"
                                        << true);

    for (auto docSeq : {false, true}) {
        BatchedCommandRequest insertRequest(BatchedCommandRequest::BatchType_Insert);
        insertRequest.parseRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest()->getNS().toString());
        ASSERT(!insertRequest.hasShardVersion());
    }
}

TEST(BatchedCommandRequest, InsertWithShardVersion) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    const OID epoch = OID::gen();

    BSONObj origInsertRequestObj = BSON("insert"
                                        << "test"
                                        << "documents"
                                        << insertArray
                                        << "writeConcern"
                                        << BSON("w" << 1)
                                        << "ordered"
                                        << true
                                        << "shardVersion"
                                        << BSON_ARRAY(Timestamp(1, 2) << epoch));

    for (auto docSeq : {false, true}) {
        BatchedCommandRequest insertRequest(BatchedCommandRequest::BatchType_Insert);
        insertRequest.parseRequest(toOpMsg("TestDB", origInsertRequestObj, docSeq));

        ASSERT_EQ("TestDB.test", insertRequest.getInsertRequest()->getNS().toString());
        ASSERT(insertRequest.hasShardVersion());
        ASSERT_EQ(ChunkVersion(1, 2, epoch).toString(), insertRequest.getShardVersion().toString());
    }
}

TEST(BatchedCommandRequest, InsertCloneWithId) {
    auto insertRequest = stdx::make_unique<BatchedInsertRequest>();
    insertRequest->setOrdered(true);
    insertRequest->setWriteConcern(BSON("w" << 2));
    insertRequest->addToDocuments(BSON("x" << 4));
    insertRequest->setShouldBypassValidation(true);

    BatchedCommandRequest batchedRequest(insertRequest.release());
    batchedRequest.setNS(NamespaceString("xyz.abc"));

    std::unique_ptr<BatchedCommandRequest> clonedRequest(
        BatchedCommandRequest::cloneWithIds(batchedRequest));

    ASSERT_EQ("xyz.abc", clonedRequest->getNS().toString());
    ASSERT_EQ("xyz.abc", clonedRequest->getTargetingNSS().toString());
    ASSERT_TRUE(clonedRequest->getOrdered());
    ASSERT_BSONOBJ_EQ(BSON("w" << 2), clonedRequest->getWriteConcern());
    ASSERT_TRUE(clonedRequest->shouldBypassValidation());

    auto* clonedInsert = clonedRequest->getInsertRequest();
    ASSERT_TRUE(clonedInsert != nullptr);

    auto insertDocs = clonedInsert->getDocuments();
    ASSERT_EQ(1u, insertDocs.size());

    const auto& insertDoc = insertDocs.front();
    ASSERT_EQ(jstOID, insertDoc["_id"].type());
    ASSERT_EQ(4, insertDoc["x"].numberLong());
}

}  // namespace
}  // namespace mongo
