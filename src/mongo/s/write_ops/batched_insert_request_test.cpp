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

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::unique_ptr;
using std::string;

namespace {

TEST(BatchedInsertRequest, Basic) {
    BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

    BSONObj origInsertRequestObj = BSON(
        BatchedInsertRequest::collName("test") << BatchedInsertRequest::documents() << insertArray
                                               << BatchedInsertRequest::writeConcern(BSON("w" << 1))
                                               << BatchedInsertRequest::ordered(true));

    string errMsg;
    BatchedInsertRequest request;
    ASSERT_TRUE(request.parseBSON("foo", origInsertRequestObj, &errMsg));

    ASSERT_EQ("foo.test", request.getNS().ns());

    ASSERT_EQUALS(origInsertRequestObj, request.toBSON());
}

TEST(BatchedInsertRequest, GenIDAll) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

    request.setNS(NamespaceString("foo.bar"));
    request.setOrdered(false);

    BSONObj insertA = BSON("a" << 1);
    BSONObj insertB = BSON("b" << 1);
    request.addToDocuments(insertA);
    request.addToDocuments(insertB);

    unique_ptr<BatchedCommandRequest> idCmdRequest;
    idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(idCmdRequest.get());

    BatchedInsertRequest* idRequest = idCmdRequest->getInsertRequest();
    ASSERT_EQUALS(idRequest->getNS().ns(), request.getNS().ns());
    ASSERT_EQUALS(idRequest->getOrdered(), request.getOrdered());

    ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);
    ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
}

TEST(BatchedInsertRequest, GenIDPartial) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

    request.setNS(NamespaceString("foo.bar"));
    request.setOrdered(false);

    BSONObj insertA = BSON("a" << 1);
    BSONObj insertB = BSON("b" << 1 << "_id" << 1);
    BSONObj insertC = BSON("c" << 1);
    request.addToDocuments(insertA);
    request.addToDocuments(insertB);
    request.addToDocuments(insertC);

    unique_ptr<BatchedCommandRequest> idCmdRequest;
    idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(idCmdRequest.get());

    BatchedInsertRequest* idRequest = idCmdRequest->getInsertRequest();
    ASSERT_EQUALS(idRequest->getNS().ns(), request.getNS().ns());
    ASSERT_EQUALS(idRequest->getOrdered(), request.getOrdered());

    ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);
    ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
    ASSERT(!idRequest->getDocumentsAt(2)["_id"].eoo());
    ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
}

TEST(BatchedInsertRequest, GenIDNone) {
    BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
    BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

    // We need to check for system.indexes namespace
    request.setNS(NamespaceString("foo.bar"));

    BSONObj insertA = BSON("_id" << 0 << "a" << 1);
    BSONObj insertB = BSON("b" << 1 << "_id" << 1);
    request.addToDocuments(insertA);
    request.addToDocuments(insertB);

    unique_ptr<BatchedCommandRequest> idCmdRequest;
    idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
    ASSERT(!idCmdRequest.get());
}

}  // namespace
}  // namespace mongo
