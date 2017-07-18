/**
 *    Copyright (C) 2013-2015 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/ops/write_ops_parsers_test_helpers.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(BatchedInsertRequest, Basic) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON(
        BatchedInsertRequest::collName("test") << BatchedInsertRequest::documents() << insertArray);

    for (auto docSeq : {false, true}) {
        BatchedInsertRequest request;
        request.parseRequest(toOpMsg("foo", origInsertRequestObj, docSeq));

        ASSERT_EQ("foo.test", request.getNS().ns());

        ASSERT_BSONOBJ_EQ(origInsertRequestObj, request.toBSON());
    }
}

TEST(BatchedInsertRequest, GenIDAll) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    cmdRequest.setNS(NamespaceString("foo.bar"));
    {
        write_ops::WriteCommandBase writeCommandBase;
        writeCommandBase.setOrdered(false);
        writeCommandBase.setBypassDocumentValidation(true);
        cmdRequest.setWriteCommandBase(std::move(writeCommandBase));
    }
    cmdRequest.setWriteConcern(BSON("w"
                                    << "majority"
                                    << "wtimeout"
                                    << 30000));

    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
    request.addToDocuments(BSON("a" << 1));
    request.addToDocuments(BSON("b" << 1));

    const std::unique_ptr<BatchedCommandRequest> idCmdRequest(
        BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(idCmdRequest.get());
    ASSERT_EQUALS(cmdRequest.getNS().ns(), idCmdRequest->getNS().ns());
    ASSERT_EQUALS(cmdRequest.getWriteCommandBase().getOrdered(),
                  idCmdRequest->getWriteCommandBase().getOrdered());
    ASSERT_EQUALS(cmdRequest.getWriteCommandBase().getBypassDocumentValidation(),
                  idCmdRequest->getWriteCommandBase().getBypassDocumentValidation());
    ASSERT_BSONOBJ_EQ(cmdRequest.getWriteConcern(), idCmdRequest->getWriteConcern());

    auto* const idRequest = idCmdRequest->getInsertRequest();

    ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
    ASSERT(!idRequest->getDocumentsAt(0)["a"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);

    ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
    ASSERT(!idRequest->getDocumentsAt(1)["b"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
}

TEST(BatchedInsertRequest, GenIDPartial) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    cmdRequest.setNS(NamespaceString("foo.bar"));

    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
    request.addToDocuments(BSON("a" << 1));
    request.addToDocuments(BSON("b" << 1 << "_id" << 1));
    request.addToDocuments(BSON("c" << 1));

    const std::unique_ptr<BatchedCommandRequest> idCmdRequest(
        BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(idCmdRequest.get());
    ASSERT_EQUALS(cmdRequest.getNS().ns(), idCmdRequest->getNS().ns());

    auto* const idRequest = idCmdRequest->getInsertRequest();

    ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);

    ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);

    ASSERT(!idRequest->getDocumentsAt(2)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
}

TEST(BatchedInsertRequest, GenIDNone) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    cmdRequest.setNS(NamespaceString("foo.bar"));

    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();
    request.addToDocuments(BSON("_id" << 0 << "a" << 1));
    request.addToDocuments(BSON("b" << 1 << "_id" << 1));

    const std::unique_ptr<BatchedCommandRequest> idCmdRequest(
        BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(!idCmdRequest.get());
}

}  // namespace
}  // namespace mongo
