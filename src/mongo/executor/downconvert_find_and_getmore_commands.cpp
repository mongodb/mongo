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

#include "mongo/executor/downconvert_find_and_getmore_commands.h"

#include <memory>
#include <string>
#include <tuple>

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/status_with.h"
#include "mongo/client/constants.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace executor {

namespace {

StatusWith<std::tuple<CursorId, BSONArray>> getBatchFromReply(std::int32_t requestId,
                                                              const Message& response) {
    auto header = response.header();
    if (header.getNetworkOp() != mongo::opReply) {
        return {ErrorCodes::ProtocolError,
                str::stream() << "Expected to be decoding an OP_REPLY but got "
                              << mongo::networkOpToString(header.getNetworkOp())};
    }

    if (header.getResponseToMsgId() != requestId) {
        return {ErrorCodes::ProtocolError,
                str::stream() << "responseTo field of OP_REPLY header with value '"
                              << header.getResponseToMsgId()
                              << "' does not match requestId '"
                              << requestId
                              << "'"};
    }

    if ((header.dataLen() < 0) ||
        (static_cast<std::size_t>(header.dataLen()) > mongo::MaxMessageSizeBytes)) {
        return {ErrorCodes::InvalidLength,
                str::stream() << "Received message has invalid length field with value "
                              << header.dataLen()};
    }

    QueryResult::View qr = response.header().view2ptr();

    auto resultFlags = qr.getResultFlags();

    if (resultFlags & ResultFlag_CursorNotFound) {
        return {ErrorCodes::CursorNotFound,
                str::stream() << "Cursor with id '" << qr.getCursorId() << "' not found"};
    }

    // Use CDRC directly instead of DocumentRange as DocumentRange has a throwing API.
    ConstDataRangeCursor cdrc{qr.data(), qr.data() + header.dataLen()};

    if (resultFlags & ResultFlag_ErrSet) {
        if (qr.getNReturned() != 1) {
            return {ErrorCodes::BadValue,
                    str::stream() << "ResultFlag_ErrSet flag set on reply, but nReturned was '"
                                  << qr.getNReturned()
                                  << "' - expected 1"};
        }
        // Convert error document to a Status.
        // Will throw if first document is invalid BSON.
        auto first = cdrc.readAndAdvance<Validated<BSONObj>>();
        if (!first.isOK()) {
            return first.getStatus();
        }

        // Convert error document to a status.
        return getStatusFromCommandResult(first.getValue());
    }

    Validated<BSONObj> nextObj;
    BSONArrayBuilder batch;
    while (!cdrc.empty() && batch.arrSize() < qr.getNReturned()) {
        auto readStatus = cdrc.readAndAdvance(&nextObj);
        if (!readStatus.isOK()) {
            return readStatus;
        }
        batch.append(nextObj.val);
    }
    if (qr.getNReturned() != batch.arrSize()) {
        return {ErrorCodes::InvalidLength,
                str::stream() << "Count of documents in OP_REPLY message (" << batch.arrSize()
                              << ") did not match the value specified in the nReturned field ("
                              << qr.getNReturned()
                              << ")"};
    }

    return {std::make_tuple(qr.getCursorId(), batch.arr())};
}

}  // namespace

StatusWith<Message> downconvertFindCommandRequest(const RemoteCommandRequest& request) {
    const auto& cmdObj = request.cmdObj;
    const NamespaceString nss(request.dbname, cmdObj.firstElement().String());
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name: " << nss.ns()};
    }

    const std::string& ns = nss.ns();

    // It is a little heavy handed to use QueryRequest to convert the command object to
    // query() arguments but we get validation and consistent behavior with the find
    // command implementation on the remote server.
    auto qrStatus = QueryRequest::makeFromFindCommand(nss, cmdObj, false);
    if (!qrStatus.isOK()) {
        return qrStatus.getStatus();
    }

    auto qr = std::move(qrStatus.getValue());

    // We are downconverting a find command, and find command can only have ntoreturn
    // if it was generated by mongos.
    invariant(!qr->getNToReturn());
    Query query(qr->getFilter());
    if (!qr->getSort().isEmpty()) {
        query.sort(qr->getSort());
    }
    if (!qr->getHint().isEmpty()) {
        query.hint(qr->getHint());
    }
    if (!qr->getMin().isEmpty()) {
        query.minKey(qr->getMin());
    }
    if (!qr->getMax().isEmpty()) {
        query.minKey(qr->getMax());
    }
    if (qr->isExplain()) {
        query.explain();
    }
    if (qr->isSnapshot()) {
        query.snapshot();
    }

    const int nToReturn = qr->getLimit().value_or(0) * -1;
    const int nToSkip = qr->getSkip().value_or(0);
    const BSONObj* fieldsToReturn = &qr->getProj();
    int queryOptions = qr->getOptions();  // non-const so we can set slaveOk if we need to
    const int batchSize = qr->getBatchSize().value_or(0);

    const int nextBatchSize = [batchSize, nToReturn]() {
        if (nToReturn == 0)
            return batchSize;
        if (batchSize == 0)
            return nToReturn;
        return batchSize < nToReturn ? batchSize : nToReturn;
    }();

    // We can't downconvert all metadata, since we aren't sending a command, but we do need to
    // downconvert $secondaryOk to the slaveOK bit.
    auto ssm = rpc::ServerSelectionMetadata::readFromMetadata(
        request.metadata.getField(rpc::ServerSelectionMetadata::fieldName()));
    if (!ssm.isOK()) {
        return ssm.getStatus();
    }
    if (ssm.getValue().isSecondaryOk()) {
        queryOptions |= mongo::QueryOption_SlaveOk;
    }

    Message message;
    assembleQueryRequest(
        ns, query.obj, nextBatchSize, nToSkip, fieldsToReturn, queryOptions, message);

    return {std::move(message)};
}

StatusWith<RemoteCommandResponse> upconvertLegacyQueryResponse(std::int32_t requestId,
                                                               StringData cursorNamespace,
                                                               const Message& response) {
    auto swBatch = getBatchFromReply(requestId, response);
    if (!swBatch.isOK()) {
        return swBatch.getStatus();
    }

    BSONArray batch;
    CursorId cursorId;
    std::tie(cursorId, batch) = std::move(swBatch.getValue());

    BSONObjBuilder result;
    appendCursorResponseObject(cursorId, cursorNamespace, std::move(batch), &result);
    // Using Command::appendCommandStatus would create a circular dep, so it's simpler to just do
    // this.
    result.append("ok", 1.0);

    RemoteCommandResponse upconvertedResponse;
    upconvertedResponse.data = result.obj();

    return {std::move(upconvertedResponse)};
}

StatusWith<Message> downconvertGetMoreCommandRequest(const RemoteCommandRequest& request) {
    auto swGetMoreRequest = GetMoreRequest::parseFromBSON(request.dbname, request.cmdObj);
    if (!swGetMoreRequest.isOK()) {
        return swGetMoreRequest.getStatus();
    }

    auto getMoreRequest = std::move(swGetMoreRequest.getValue());

    BufBuilder b;
    b.appendNum(std::int32_t{0});  // reserved bits
    b.appendStr(getMoreRequest.nss.ns());
    // Without this static cast, we will append batchSize as an int64 and get an invalid message.
    b.appendNum(static_cast<std::int32_t>(getMoreRequest.batchSize.value_or(0)));
    b.appendNum(getMoreRequest.cursorid);
    Message m;
    m.setData(dbGetMore, b.buf(), b.len());

    return {std::move(m)};
}

StatusWith<RemoteCommandResponse> upconvertLegacyGetMoreResponse(std::int32_t requestId,
                                                                 StringData cursorNamespace,
                                                                 const Message& response) {
    auto swBatch = getBatchFromReply(requestId, response);
    if (!swBatch.isOK()) {
        return swBatch.getStatus();
    }

    BSONArray batch;
    CursorId cursorId;

    std::tie(cursorId, batch) = std::move(swBatch.getValue());

    BSONObjBuilder result;
    appendGetMoreResponseObject(cursorId, cursorNamespace, std::move(batch), &result);
    result.append("ok", 1.0);

    RemoteCommandResponse resp;
    resp.data = result.obj();

    return {std::move(resp)};
}

}  // namespace mongo
}  // namespace executor
