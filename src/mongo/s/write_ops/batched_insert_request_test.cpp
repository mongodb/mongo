/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/s/write_ops/batched_insert_request.h"

#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/s/write_ops/batched_command_request.h"

namespace {

    using namespace mongo;
    using std::auto_ptr;
    using std::string;

    TEST(RoundTrip, Normal) {
        BSONArray insertArray = BSON_ARRAY(BSON("a" << 1) << BSON("b" << 1));

        BSONObj writeConcernObj = BSON("w" << 1);

        // The BSON_ARRAY macro doesn't support Timestamps.
        BSONArrayBuilder arrBuilder;
        arrBuilder.appendTimestamp(OpTime(1,1).asDate());
        arrBuilder.append(OID::gen());
        BSONArray shardVersionArray = arrBuilder.arr();

        BSONObj origInsertRequestObj =
            BSON(BatchedInsertRequest::collName("test") <<
                 BatchedInsertRequest::documents() << insertArray <<
                 BatchedInsertRequest::writeConcern(writeConcernObj) <<
                 BatchedInsertRequest::ordered(true) <<
                 BatchedInsertRequest::metadata() << BSON(
                     BatchedRequestMetadata::shardName("shard0000") <<
                     BatchedRequestMetadata::shardVersion() << shardVersionArray <<
                     BatchedRequestMetadata::session(0)));

        string errMsg;
        BatchedInsertRequest request;
        bool ok = request.parseBSON(origInsertRequestObj, &errMsg);
        ASSERT_TRUE(ok);

        BSONObj genInsertRequestObj = request.toBSON();
        ASSERT_EQUALS(0, genInsertRequestObj.woCompare(origInsertRequestObj));
    }

    TEST(GenID, All) {

        BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

        request.setCollName("foo.bar");
        request.setOrdered(false);

        BSONObj insertA = BSON( "a" << 1 );
        BSONObj insertB = BSON( "b" << 1 );
        request.addToDocuments(insertA);
        request.addToDocuments(insertB);

        auto_ptr<BatchedCommandRequest> idCmdRequest;
        idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
        ASSERT(idCmdRequest.get());

        BatchedInsertRequest* idRequest = idCmdRequest->getInsertRequest();
        ASSERT_EQUALS(idRequest->getCollName(), request.getCollName());
        ASSERT_EQUALS(idRequest->getOrdered(), request.getOrdered());

        ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
        ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);
        ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
        ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
    }

    TEST(GenID, Partial) {

        BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

        request.setCollName("foo.bar");
        request.setOrdered(false);

        BSONObj insertA = BSON( "a" << 1 );
        BSONObj insertB = BSON( "b" << 1 << "_id" << 1 );
        BSONObj insertC = BSON( "c" << 1 );
        request.addToDocuments(insertA);
        request.addToDocuments(insertB);
        request.addToDocuments(insertC);

        auto_ptr<BatchedCommandRequest> idCmdRequest;
        idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
        ASSERT(idCmdRequest.get());

        BatchedInsertRequest* idRequest = idCmdRequest->getInsertRequest();
        ASSERT_EQUALS(idRequest->getCollName(), request.getCollName());
        ASSERT_EQUALS(idRequest->getOrdered(), request.getOrdered());

        ASSERT(!idRequest->getDocumentsAt(0)["_id"].eoo());
        ASSERT_EQUALS(idRequest->getDocumentsAt(0).nFields(), 2);
        ASSERT(!idRequest->getDocumentsAt(1)["_id"].eoo());
        ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
        ASSERT(!idRequest->getDocumentsAt(2)["_id"].eoo());
        ASSERT_EQUALS(idRequest->getDocumentsAt(1).nFields(), 2);
    }

    TEST(GenID, None) {

        BatchedCommandRequest cmdRequest(BatchedCommandRequest::BatchType_Insert);
        BatchedInsertRequest& request = *cmdRequest.getInsertRequest();

        // We need to check for system.indexes namespace
        request.setCollName("foo.bar");

        BSONObj insertA = BSON( "_id" << 0 << "a" << 1 );
        BSONObj insertB = BSON( "b" << 1 << "_id" << 1 );
        request.addToDocuments(insertA);
        request.addToDocuments(insertB);

        auto_ptr<BatchedCommandRequest> idCmdRequest;
        idCmdRequest.reset(BatchedCommandRequest::cloneWithIds(cmdRequest));
        ASSERT(!idCmdRequest.get());
    }


} // unnamed namespace
