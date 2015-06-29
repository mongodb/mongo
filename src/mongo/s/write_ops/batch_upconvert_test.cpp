/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/s/write_ops/batch_upconvert.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclientinterface.h"  // for write constants
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"

namespace {

using std::string;
using std::vector;

using namespace mongo;

TEST(WriteBatchUpconvert, BasicInsert) {
    // Tests that an insert message is correctly upconverted to a batch insert

    const string ns = "foo.bar";
    const BSONObj doc = BSON("hello"
                             << "world");

    Message insertMsg;
    BufBuilder insertMsgB;

    int reservedFlags = InsertOption_ContinueOnError;
    insertMsgB.appendNum(reservedFlags);
    insertMsgB.appendStr(ns);
    doc.appendSelfToBufBuilder(insertMsgB);
    insertMsg.setData(dbInsert, insertMsgB.buf(), insertMsgB.len());

    OwnedPointerVector<BatchedCommandRequest> requestsOwned;
    vector<BatchedCommandRequest*>& requests = requestsOwned.mutableVector();
    msgToBatchRequests(insertMsg, &requests);

    BatchedCommandRequest* request = requests.back();
    ASSERT_EQUALS(request->getBatchType(), BatchedCommandRequest::BatchType_Insert);
    string errMsg;
    ASSERT(request->isValid(&errMsg));
    ASSERT_EQUALS(request->getNS().toString(), ns);
    ASSERT(!request->getOrdered());
    ASSERT_EQUALS(request->sizeWriteOps(), 1u);
    bool isSameDoc = doc.woCompare(request->getInsertRequest()->getDocumentsAt(0)) == 0;
    ASSERT(isSameDoc);
    ASSERT(request->getWriteConcern().woCompare(WriteConcernOptions::Acknowledged) == 0);
}

TEST(WriteBatchUpconvert, BasicUpdate) {
    // Tests that an update message is correctly upconverted to a batch update

    const string ns = "foo.bar";
    const BSONObj query = BSON("hello"
                               << "world");
    const BSONObj update = BSON("$set" << BSON("hello"
                                               << "world"));

    Message updateMsg;
    BufBuilder updateMsgB;

    int reservedFlags = 0;
    updateMsgB.appendNum(reservedFlags);
    updateMsgB.appendStr(ns);
    updateMsgB.appendNum(UpdateOption_Upsert);
    query.appendSelfToBufBuilder(updateMsgB);
    update.appendSelfToBufBuilder(updateMsgB);
    updateMsg.setData(dbUpdate, updateMsgB.buf(), updateMsgB.len());

    OwnedPointerVector<BatchedCommandRequest> requestsOwned;
    vector<BatchedCommandRequest*>& requests = requestsOwned.mutableVector();
    msgToBatchRequests(updateMsg, &requests);

    BatchedCommandRequest* request = requests.back();
    ASSERT_EQUALS(request->getBatchType(), BatchedCommandRequest::BatchType_Update);
    string errMsg;
    ASSERT(request->isValid(&errMsg));
    ASSERT_EQUALS(request->getNS().toString(), ns);
    ASSERT_EQUALS(request->sizeWriteOps(), 1u);
    ASSERT(query.woCompare(request->getUpdateRequest()->getUpdatesAt(0)->getQuery()) == 0);
    ASSERT(update.woCompare(request->getUpdateRequest()->getUpdatesAt(0)->getUpdateExpr()) == 0);
    ASSERT(request->getUpdateRequest()->getUpdatesAt(0)->getUpsert());
    ASSERT(!request->getUpdateRequest()->getUpdatesAt(0)->getMulti());
    ASSERT(request->getWriteConcern().woCompare(WriteConcernOptions::Acknowledged) == 0);
}

TEST(WriteBatchUpconvert, BasicDelete) {
    // Tests that an remove message is correctly upconverted to a batch delete

    const string ns = "foo.bar";
    const BSONObj query = BSON("hello"
                               << "world");

    Message deleteMsg;
    BufBuilder deleteMsgB;

    int reservedFlags = 0;
    deleteMsgB.appendNum(reservedFlags);
    deleteMsgB.appendStr(ns);
    deleteMsgB.appendNum(RemoveOption_JustOne);
    query.appendSelfToBufBuilder(deleteMsgB);
    deleteMsg.setData(dbDelete, deleteMsgB.buf(), deleteMsgB.len());

    OwnedPointerVector<BatchedCommandRequest> requestsOwned;
    vector<BatchedCommandRequest*>& requests = requestsOwned.mutableVector();
    msgToBatchRequests(deleteMsg, &requests);

    BatchedCommandRequest* request = requests.back();
    ASSERT_EQUALS(request->getBatchType(), BatchedCommandRequest::BatchType_Delete);
    string errMsg;
    ASSERT(request->isValid(&errMsg));
    ASSERT_EQUALS(request->getNS().toString(), ns);
    ASSERT_EQUALS(request->sizeWriteOps(), 1u);
    ASSERT(query.woCompare(request->getDeleteRequest()->getDeletesAt(0)->getQuery()) == 0);
    ASSERT(request->getDeleteRequest()->getDeletesAt(0)->getLimit() == 1);
    ASSERT(request->getWriteConcern().woCompare(WriteConcernOptions::Acknowledged) == 0);
}
}
