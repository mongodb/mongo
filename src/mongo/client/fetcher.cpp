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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/client/fetcher.h"

#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
const char* kCursorFieldName = "cursor";
const char* kCursorIdFieldName = "id";
const char* kNamespaceFieldName = "ns";

const char* kFirstBatchFieldName = "firstBatch";
const char* kNextBatchFieldName = "nextBatch";

Status parseCursorResponseFromResponseObj(const BSONObj& responseObj,
                                          const std::string& batchFieldName,
                                          Fetcher::QueryResponse* batchData) {
    BSONElement cursorElement = responseObj.getField(kCursorFieldName);
    if (cursorElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName
                                    << "' field: " << responseObj);
    }
    if (!cursorElement.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName
                                    << "' field must be an object: " << responseObj);
    }
    BSONObj cursorObj = cursorElement.Obj();

    BSONElement cursorIdElement = cursorObj.getField(kCursorIdFieldName);
    if (cursorIdElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << kCursorIdFieldName << "' field: " << responseObj);
    }
    if (cursorIdElement.type() != mongo::NumberLong) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kCursorIdFieldName
                                    << "' field must be a 'long' but was a '"
                                    << typeName(cursorIdElement.type()) << "': " << responseObj);
    }
    batchData->cursorId = cursorIdElement.numberLong();

    BSONElement namespaceElement = cursorObj.getField(kNamespaceFieldName);
    if (namespaceElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain "
                                    << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' field: " << responseObj);
    }
    if (namespaceElement.type() != mongo::String) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' field must be a string: " << responseObj);
    }
    NamespaceString tempNss(namespaceElement.valuestrsafe());
    if (!tempNss.isValid()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' contains an invalid namespace: " << responseObj);
    }
    batchData->nss = tempNss;

    BSONElement batchElement = cursorObj.getField(batchFieldName);
    if (batchElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << batchFieldName << "' field: " << responseObj);
    }
    if (!batchElement.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << batchFieldName
                                    << "' field must be an array: " << responseObj);
    }
    BSONObj batchObj = batchElement.Obj();
    for (auto itemElement : batchObj) {
        if (!itemElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "found non-object " << itemElement << " in "
                                        << "'" << kCursorFieldName << "." << batchFieldName
                                        << "' field: " << responseObj);
        }
        batchData->documents.push_back(itemElement.Obj().getOwned());
    }

    return Status::OK();
}

/**
 * Extracts the CursorId and array of results from a Message representing an OP_REPLY. Returns a
 * non-OK status if Message does not represent a well-formed OP_REPLY.
 */
StatusWith<std::tuple<CursorId, std::vector<BSONObj>>> getBatchFromReply(const Message* response) {
    auto header = response->header();
    if (header.getNetworkOp() != mongo::opReply) {
        return {ErrorCodes::ProtocolError,
                str::stream() << "Expected to be decoding an OP_REPLY but got "
                              << mongo::networkOpToString(header.getNetworkOp())};
    }

    if ((header.dataLen() < 0) ||
        (static_cast<std::size_t>(header.dataLen()) > mongo::MaxMessageSizeBytes)) {
        return {ErrorCodes::InvalidLength,
                str::stream() << "Received message has invalid length field with value "
                              << header.dataLen()};
    }

    QueryResult::View qr = response->header().view2ptr();

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
                                  << qr.getNReturned() << "' - expected 1"};
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

    const int32_t nReturned = qr.getNReturned();
    std::vector<BSONObj> batch;
    batch.reserve(qr.getNReturned());

    int32_t nParsed = 0;
    Validated<BSONObj> nextObj;
    while (!cdrc.empty() && nParsed < nReturned) {
        auto readStatus = cdrc.readAndAdvance(&nextObj);
        if (!readStatus.isOK()) {
            return readStatus;
        }
        ++nParsed;
        batch.emplace_back(nextObj.val.getOwned());
    }
    if (nParsed != nReturned) {
        return {ErrorCodes::InvalidLength,
                str::stream() << "Count of documents in OP_REPLY message (" << nParsed
                              << ") did not match the value specified in the nReturned field ("
                              << nReturned << ")"};
    }

    return {std::make_tuple(qr.getCursorId(), std::move(batch))};
}

Status parseCursorResponseFromRawMessage(const Message* message,
                                         Fetcher::QueryResponse* batchData) {
    auto batchStatus = getBatchFromReply(message);
    if (!batchStatus.isOK()) {
        return batchStatus.getStatus();
    }

    std::tie(batchData->cursorId, batchData->documents) = batchStatus.getValue();
    return Status::OK();
}

/**
 * Parses cursor response in command result for cursor ID, namespace and documents.
 * 'batchFieldName' will be 'firstBatch' for the initial remote command invocation and 'nextBatch'
 * for getMore.
 */
Status parseCursorResponse(const RemoteCommandResponse& response,
                           const std::string& batchFieldName,
                           Fetcher::QueryResponse* batchData) {
    invariant(batchFieldName == kFirstBatchFieldName || batchFieldName == kNextBatchFieldName);
    invariant(batchData);

    // If we are talking to a 3.0 mongod, then the response will have come back as an OP_QUERY, and
    // we'll need to parse the raw message to populate 'batchData'. Otherwise, we ran a find or
    // getMore command, and need to parse the BSON that is returned from those commands.
    Status status = getStatusFromCommandResult(response.data);
    if (status.isOK()) {
        return parseCursorResponseFromResponseObj(response.data, batchFieldName, batchData);
    } else if (status.code() == ErrorCodes::ReceivedOpReplyMessage) {
        auto ns = response.data["ns"];
        if (!ns) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "expected 'ns' field to be present in response: "
                                  << response.data};
        }
        if (ns.type() != String) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "expected 'ns' field to be a string, was "
                                  << typeName(ns.type()) << ": " << response.data};
        }
        auto nss = NamespaceString(ns.String());
        if (!nss.isValid()) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "invalid 'ns' field in response: " << response.data};
        }
        batchData->nss = nss;
        return parseCursorResponseFromRawMessage(response.message.get(), batchData);
    } else {
        return status;
    }
}

}  // namespace

Fetcher::Fetcher(executor::TaskExecutor* executor,
                 const HostAndPort& source,
                 const std::string& dbname,
                 const BSONObj& findCmdObj,
                 const CallbackFn& work,
                 const BSONObj& metadata)
    : Fetcher(
          executor, source, dbname, findCmdObj, work, metadata, RemoteCommandRequest::kNoTimeout) {}

Fetcher::Fetcher(executor::TaskExecutor* executor,
                 const HostAndPort& source,
                 const std::string& dbname,
                 const BSONObj& findCmdObj,
                 const CallbackFn& work,
                 const BSONObj& metadata,
                 Milliseconds timeout)
    : _executor(executor),
      _source(source),
      _dbname(dbname),
      _cmdObj(findCmdObj.getOwned()),
      _metadata(metadata.getOwned()),
      _work(work),
      _active(false),
      _first(true),
      _remoteCommandCallbackHandle(),
      _timeout(timeout) {
    uassert(ErrorCodes::BadValue, "null replication executor", executor);
    uassert(ErrorCodes::BadValue, "database name cannot be empty", !dbname.empty());
    uassert(ErrorCodes::BadValue, "command object cannot be empty", !findCmdObj.isEmpty());
    uassert(ErrorCodes::BadValue, "callback function cannot be null", work);
}

Fetcher::~Fetcher() {
    DESTRUCTOR_GUARD(cancel(); wait(););
}

std::string Fetcher::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "Fetcher";
    output << " executor: " << _executor->getDiagnosticString();
    output << " source: " << _source.toString();
    output << " database: " << _dbname;
    output << " query: " << _cmdObj;
    output << " query metadata: " << _metadata;
    output << " active: " << _active;
    output << " timeout: " << _timeout;
    return output;
}

bool Fetcher::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _active;
}

Status Fetcher::schedule() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_active) {
        return Status(ErrorCodes::IllegalOperation, "fetcher already scheduled");
    }
    return _schedule_inlock(_cmdObj, kFirstBatchFieldName);
}

void Fetcher::cancel() {
    executor::TaskExecutor::CallbackHandle remoteCommandCallbackHandle;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (!_active) {
            return;
        }

        remoteCommandCallbackHandle = _remoteCommandCallbackHandle;
    }

    invariant(remoteCommandCallbackHandle.isValid());
    _executor->cancel(remoteCommandCallbackHandle);
}

void Fetcher::wait() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _condition.wait(lk, [this]() { return !_active; });
}

Status Fetcher::_schedule_inlock(const BSONObj& cmdObj, const char* batchFieldName) {
    StatusWith<executor::TaskExecutor::CallbackHandle> scheduleResult =
        _executor->scheduleRemoteCommand(
            RemoteCommandRequest(_source, _dbname, cmdObj, _metadata, _timeout),
            stdx::bind(&Fetcher::_callback, this, stdx::placeholders::_1, batchFieldName));

    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _active = true;
    _remoteCommandCallbackHandle = scheduleResult.getValue();
    return Status::OK();
}

void Fetcher::_callback(const RemoteCommandCallbackArgs& rcbd, const char* batchFieldName) {
    if (!rcbd.response.isOK()) {
        _work(StatusWith<Fetcher::QueryResponse>(rcbd.response.getStatus()), nullptr, nullptr);
        _finishCallback();
        return;
    }

    QueryResponse batchData;
    auto status = parseCursorResponse(rcbd.response.getValue(), batchFieldName, &batchData);
    if (!status.isOK()) {
        _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
        _finishCallback();
        return;
    }

    batchData.otherFields.metadata = std::move(rcbd.response.getValue().metadata);
    batchData.elapsedMillis = rcbd.response.getValue().elapsedMillis;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        batchData.first = _first;
        _first = false;
    }

    NextAction nextAction = NextAction::kNoAction;

    if (!batchData.cursorId) {
        _work(StatusWith<QueryResponse>(batchData), &nextAction, nullptr);
        _finishCallback();
        return;
    }

    nextAction = NextAction::kGetMore;

    BSONObjBuilder bob;
    _work(StatusWith<QueryResponse>(batchData), &nextAction, &bob);

    // Callback function _work may modify nextAction to request the fetcher
    // not to schedule a getMore command.
    if (nextAction != NextAction::kGetMore) {
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }

    // Callback function may also disable the fetching of additional data by not filling in the
    // BSONObjBuilder for the getMore command.
    auto cmdObj = bob.obj();
    if (cmdObj.isEmpty()) {
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        status = _schedule_inlock(cmdObj, kNextBatchFieldName);
    }
    if (!status.isOK()) {
        nextAction = NextAction::kNoAction;
        _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
        _sendKillCursors(batchData.cursorId, batchData.nss);
        _finishCallback();
        return;
    }
}

void Fetcher::_sendKillCursors(const CursorId id, const NamespaceString& nss) {
    if (id) {
        auto logKillCursorsResult = [](const RemoteCommandCallbackArgs& args) {
            if (!args.response.isOK()) {
                warning() << "killCursors command task failed: " << args.response.getStatus();
                return;
            }
            auto status = getStatusFromCommandResult(args.response.getValue().data);
            if (!status.isOK()) {
                warning() << "killCursors command failed: " << status;
            }
        };
        auto cmdObj = BSON("killCursors" << nss.coll() << "cursors" << BSON_ARRAY(id));
        auto scheduleResult = _executor->scheduleRemoteCommand(
            RemoteCommandRequest(_source, _dbname, cmdObj), logKillCursorsResult);
        if (!scheduleResult.isOK()) {
            warning() << "failed to schedule killCursors command: " << scheduleResult.getStatus();
        }
    }
}
void Fetcher::_finishCallback() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _active = false;
    _first = false;
    _condition.notify_all();
}

}  // namespace mongo
