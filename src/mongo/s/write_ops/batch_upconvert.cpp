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

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_delete_document.h"
#include "mongo/s/write_ops/batched_update_document.h"

namespace mongo {

using str::stream;
using std::string;
using std::unique_ptr;
using std::vector;

namespace {

// Batch inserts may get mapped to multiple batch requests, to avoid spilling MaxBSONObjSize
void msgToBatchInserts(const Message& insertMsg, vector<BatchedCommandRequest*>* insertRequests) {
    // Parsing DbMessage throws
    DbMessage dbMsg(insertMsg);
    NamespaceString nss(dbMsg.getns());

    // Continue-on-error == unordered
    bool coe = dbMsg.reservedField() & Reserved_InsertOption_ContinueOnError;
    bool ordered = !coe;

    while (insertRequests->empty() || dbMsg.moreJSObjs()) {
        // Collect docs for next batch, but don't exceed maximum size
        int totalInsertSize = 0;
        vector<BSONObj> docs;
        do {
            const char* prevObjMark = dbMsg.markGet();
            BSONObj nextObj = dbMsg.nextJsObj();
            if (totalInsertSize + nextObj.objsize() <= BSONObjMaxUserSize) {
                docs.push_back(nextObj);
                totalInsertSize += docs.back().objsize();
            } else {
                // Size limit exceeded, rollback to previous insert position
                dbMsg.markReset(prevObjMark);
                break;
            }
        } while (docs.size() < BatchedCommandRequest::kMaxWriteBatchSize && dbMsg.moreJSObjs());

        dassert(!docs.empty());

        // No exceptions from here on
        BatchedCommandRequest* request =
            new BatchedCommandRequest(BatchedCommandRequest::BatchType_Insert);
        request->setNS(nss);
        for (vector<BSONObj>::const_iterator it = docs.begin(); it != docs.end(); ++it) {
            request->getInsertRequest()->addToDocuments(*it);
        }
        request->setOrdered(ordered);
        request->setWriteConcern(WriteConcernOptions::Acknowledged);

        insertRequests->push_back(request);
    }
}

BatchedCommandRequest* msgToBatchUpdate(const Message& updateMsg) {
    // Parsing DbMessage throws
    DbMessage dbMsg(updateMsg);
    NamespaceString nss(dbMsg.getns());
    int flags = dbMsg.pullInt();
    bool upsert = flags & UpdateOption_Upsert;
    bool multi = flags & UpdateOption_Multi;
    const BSONObj query = dbMsg.nextJsObj();
    const BSONObj updateExpr = dbMsg.nextJsObj();

    // No exceptions from here on
    BatchedUpdateDocument* updateDoc = new BatchedUpdateDocument;
    updateDoc->setQuery(query);
    updateDoc->setUpdateExpr(updateExpr);
    updateDoc->setUpsert(upsert);
    updateDoc->setMulti(multi);

    BatchedCommandRequest* request =
        new BatchedCommandRequest(BatchedCommandRequest::BatchType_Update);
    request->setNS(nss);
    request->getUpdateRequest()->addToUpdates(updateDoc);
    request->setWriteConcern(WriteConcernOptions::Acknowledged);

    return request;
}

BatchedCommandRequest* msgToBatchDelete(const Message& deleteMsg) {
    // Parsing DbMessage throws
    DbMessage dbMsg(deleteMsg);
    NamespaceString nss(dbMsg.getns());
    int flags = dbMsg.pullInt();
    const BSONObj query = dbMsg.nextJsObj();
    int limit = (flags & RemoveOption_JustOne) ? 1 : 0;

    // No exceptions from here on
    BatchedDeleteDocument* deleteDoc = new BatchedDeleteDocument;
    deleteDoc->setLimit(limit);
    deleteDoc->setQuery(query);

    BatchedCommandRequest* request =
        new BatchedCommandRequest(BatchedCommandRequest::BatchType_Delete);
    request->setNS(nss);
    request->getDeleteRequest()->addToDeletes(deleteDoc);
    request->setWriteConcern(WriteConcernOptions::Acknowledged);

    return request;
}

void buildErrorFromResponse(const BatchedCommandResponse& response, WriteErrorDetail* error) {
    error->setErrCode(response.getErrCode());
    error->setErrMessage(response.getErrMessage());
}

}  // namespace

void msgToBatchRequests(const Message& msg, vector<BatchedCommandRequest*>* requests) {
    int opType = msg.operation();

    if (opType == dbInsert) {
        msgToBatchInserts(msg, requests);
    } else if (opType == dbUpdate) {
        requests->push_back(msgToBatchUpdate(msg));
    } else {
        dassert(opType == dbDelete);
        requests->push_back(msgToBatchDelete(msg));
    }
}

bool batchErrorToLastError(const BatchedCommandRequest& request,
                           const BatchedCommandResponse& response,
                           LastError* error) {
    unique_ptr<WriteErrorDetail> commandError;
    WriteErrorDetail* lastBatchError = NULL;

    if (!response.getOk()) {
        // Command-level error, all writes failed

        commandError.reset(new WriteErrorDetail);
        buildErrorFromResponse(response, commandError.get());
        lastBatchError = commandError.get();
    } else if (response.isErrDetailsSet()) {
        // The last error in the batch is always reported - this matches expected COE
        // semantics for insert batches. For updates and deletes, error is only reported
        // if the error was on the last item.

        const bool lastOpErrored = response.getErrDetails().back()->getIndex() ==
            static_cast<int>(request.sizeWriteOps() - 1);
        if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert || lastOpErrored) {
            lastBatchError = response.getErrDetails().back();
        }
    } else {
        // We don't care about write concern errors, these happen in legacy mode in GLE.
    }

    // Record an error if one exists
    if (lastBatchError) {
        string errMsg = lastBatchError->getErrMessage();
        error->setLastError(lastBatchError->getErrCode(),
                            errMsg.empty() ? "see code for details" : errMsg.c_str());
        return true;
    }

    // Record write stats otherwise
    // NOTE: For multi-write batches, our semantics change a little because we don't have
    // un-aggregated "n" stats.
    if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        BSONObj upsertedId;
        if (response.isUpsertDetailsSet()) {
            // Only report the very last item's upserted id if applicable
            if (response.getUpsertDetails().back()->getIndex() + 1 ==
                static_cast<int>(request.sizeWriteOps())) {
                upsertedId = response.getUpsertDetails().back()->getUpsertedID();
            }
        }

        int numUpserted = 0;
        if (response.isUpsertDetailsSet())
            numUpserted = response.sizeUpsertDetails();

        int numMatched = response.getN() - numUpserted;
        dassert(numMatched >= 0);

        // Wrap upserted id in "upserted" field
        BSONObj leUpsertedId;
        if (!upsertedId.isEmpty())
            leUpsertedId = upsertedId.firstElement().wrap(kUpsertedFieldName);

        error->recordUpdate(numMatched > 0, response.getN(), leUpsertedId);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Delete) {
        error->recordDelete(response.getN());
    }

    return false;
}

}  // namespace mongo
