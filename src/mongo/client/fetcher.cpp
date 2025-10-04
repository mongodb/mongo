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

#include "mongo/client/fetcher.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <mutex>
#include <ostream>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor


namespace mongo {

namespace {

using RemoteCommandCallbackArgs = executor::TaskExecutor::RemoteCommandCallbackArgs;
const char* kCursorFieldName = "cursor";
const char* kCursorIdFieldName = "id";
const char* kNamespaceFieldName = "ns";

const char* kFirstBatchFieldName = "firstBatch";
const char* kNextBatchFieldName = "nextBatch";
const char* kPostBatchResumeTokenFieldName = "postBatchResumeToken";

/**
 * Parses cursor response in command result for cursor ID, namespace and documents.
 * 'batchFieldName' will be 'firstBatch' for the initial remote command invocation and
 * 'nextBatch' for getMore.
 */
Status parseCursorResponse(const BSONObj& obj,
                           const std::string& batchFieldName,
                           Fetcher::QueryResponse* batchData) {
    invariant(obj.isOwned());
    invariant(batchFieldName == kFirstBatchFieldName || batchFieldName == kNextBatchFieldName);
    invariant(batchData);

    BSONElement cursorElement = obj.getField(kCursorFieldName);
    if (cursorElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName
                                    << "' field: " << obj);
    }
    if (!cursorElement.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream()
                          << "'" << kCursorFieldName << "' field must be an object: " << obj);
    }
    BSONObj cursorObj = cursorElement.Obj();

    BSONElement cursorIdElement = cursorObj.getField(kCursorIdFieldName);
    if (cursorIdElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << kCursorIdFieldName << "' field: " << obj);
    }
    if (cursorIdElement.type() != BSONType::numberLong) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kCursorIdFieldName
                                    << "' field must be a 'long' but was a '"
                                    << typeName(cursorIdElement.type()) << "': " << obj);
    }
    batchData->cursorId = cursorIdElement.numberLong();

    BSONElement namespaceElement = cursorObj.getField(kNamespaceFieldName);
    if (namespaceElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain "
                                    << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' field: " << obj);
    }
    if (namespaceElement.type() != BSONType::string) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' field must be a string: " << obj);
    }
    const NamespaceString tempNss = NamespaceStringUtil::deserialize(
        boost::none, namespaceElement.valueStringData(), SerializationContext::stateDefault());
    if (!tempNss.isValid()) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "'" << kCursorFieldName << "." << kNamespaceFieldName
                                    << "' contains an invalid namespace: " << obj);
    }
    batchData->nss = tempNss;

    BSONElement batchElement = cursorObj.getField(batchFieldName);
    if (batchElement.eoo()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "cursor response must contain '" << kCursorFieldName << "."
                                    << batchFieldName << "' field: " << obj);
    }
    if (!batchElement.isABSONObj()) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "'" << kCursorFieldName << "." << batchFieldName
                                    << "' field must be an array: " << obj);
    }
    BSONObj batchObj = batchElement.Obj();
    for (auto itemElement : batchObj) {
        if (!itemElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "found non-object " << itemElement << " in "
                                        << "'" << kCursorFieldName << "." << batchFieldName
                                        << "' field: " << obj);
        }
        batchData->documents.push_back(itemElement.Obj());
    }

    for (auto& doc : batchData->documents) {
        doc.shareOwnershipWith(obj);
    }

    BSONElement postBatchResumeToken = cursorObj.getField(kPostBatchResumeTokenFieldName);
    if (!postBatchResumeToken.eoo()) {
        if (postBatchResumeToken.type() != BSONType::object) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream()
                              << "'" << kCursorFieldName << "." << kPostBatchResumeTokenFieldName
                              << "' field must be of type object " << obj);
        }

        batchData->otherFields.postBatchResumeToken.emplace(postBatchResumeToken.Obj().getOwned());
    }


    return Status::OK();
}

}  // namespace

Fetcher::Fetcher(executor::TaskExecutor* executor,
                 const HostAndPort& source,
                 const DatabaseName& dbname,
                 const BSONObj& findCmdObj,
                 CallbackFn work,
                 const BSONObj& metadata,
                 Milliseconds findNetworkTimeout,
                 Milliseconds getMoreNetworkTimeout,
                 std::unique_ptr<mongo::RetryStrategy> firstCommandRetryStrategy,
                 transport::ConnectSSLMode sslMode)
    : _executor(executor),
      _source(source),
      _dbname(dbname),
      _cmdObj(findCmdObj.getOwned()),
      _metadata(metadata.getOwned()),
      _work(std::move(work)),
      _findNetworkTimeout(findNetworkTimeout),
      _getMoreNetworkTimeout(getMoreNetworkTimeout),
      _firstRemoteCommandScheduler(
          _executor,
          [&] {
              RemoteCommandRequest request(
                  _source, _dbname, _cmdObj, _metadata, nullptr, _findNetworkTimeout);
              request.sslMode = sslMode;
              return request;
          }(),
          [this](const auto& x) { return this->_callback(x, kFirstBatchFieldName); },
          std::move(firstCommandRetryStrategy)),
      _sslMode(sslMode) {
    uassert(ErrorCodes::BadValue, "callback function cannot be null", _work);
}

Fetcher::~Fetcher() {
    try {
        shutdown();
        _join();
    } catch (...) {
        reportFailedDestructor(MONGO_SOURCE_LOCATION());
    }
}

HostAndPort Fetcher::getSource() const {
    return _source;
}

BSONObj Fetcher::getCommandObject() const {
    return _cmdObj;
}

BSONObj Fetcher::getMetadataObject() const {
    return _metadata;
}

std::string Fetcher::toString() const {
    return getDiagnosticString();
}

std::string Fetcher::getDiagnosticString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "Fetcher";
    output << " source: " << _source.toString();
    output << " database: " << toStringForLogging(_dbname);
    output << " query: " << _cmdObj;
    output << " query metadata: " << _metadata;
    output << " active: " << _isActive(lk);
    output << " findNetworkTimeout: " << _findNetworkTimeout;
    output << " getMoreNetworkTimeout: " << _getMoreNetworkTimeout;
    output << " shutting down?: " << _isShuttingDown_inlock();
    output << " first: " << _first;
    output << " firstCommandScheduler: " << _firstRemoteCommandScheduler.toString();

    if (_getMoreCallbackHandle.isValid()) {
        output << " getMoreHandle.valid: " << _getMoreCallbackHandle.isValid();
        output << " getMoreHandle.cancelled: " << _getMoreCallbackHandle.isCanceled();
    }

    return output;
}

bool Fetcher::isActive() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isActive(lk);
}

bool Fetcher::_isActive(WithLock lk) const {
    return State::kRunning == _state || State::kShuttingDown == _state;
}

Status Fetcher::schedule() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            _state = State::kRunning;
            break;
        case State::kRunning:
            return Status(ErrorCodes::InternalError, "fetcher already started");
        case State::kShuttingDown:
            return Status(ErrorCodes::ShutdownInProgress, "fetcher shutting down");
        case State::kComplete:
            return Status(ErrorCodes::ShutdownInProgress, "fetcher completed");
    }

    auto status = _firstRemoteCommandScheduler.startup();
    if (!status.isOK()) {
        _state = State::kComplete;
        return status;
    }

    return Status::OK();
}

void Fetcher::shutdown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    switch (_state) {
        case State::kPreStart:
            // Transition directly from PreStart to Complete if not started yet.
            _state = State::kComplete;
            _completionPromise.emplaceValue();
            return;
        case State::kRunning:
            _state = State::kShuttingDown;
            break;
        case State::kShuttingDown:
        case State::kComplete:
            // Nothing to do if we are already in ShuttingDown or Complete state.
            return;
    }

    _firstRemoteCommandScheduler.shutdown();

    if (_getMoreCallbackHandle) {
        _executor->cancel(_getMoreCallbackHandle);
    }
}

Status Fetcher::join(Interruptible* interruptible) {
    try {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        interruptible->waitForConditionOrInterrupt(
            _condition, lk, [&]() { return !_isActive(lk); });
        return Status::OK();
    } catch (const DBException&) {
        return exceptionToStatus();
    }
}

void Fetcher::_join() {
    invariantStatusOK(join(Interruptible::notInterruptible()));
}

Fetcher::State Fetcher::getState_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _state;
}

bool Fetcher::_isShuttingDown() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isShuttingDown_inlock();
}

bool Fetcher::_isShuttingDown_inlock() const {
    return State::kShuttingDown == _state;
}

Status Fetcher::_scheduleGetMore(const BSONObj& cmdObj) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_isShuttingDown_inlock()) {
        return Status(ErrorCodes::CallbackCanceled,
                      "fetcher was shut down after previous batch was processed");
    }

    RemoteCommandRequest request(
        _source, _dbname, cmdObj, _metadata, nullptr, _getMoreNetworkTimeout);
    request.sslMode = _sslMode;

    StatusWith<executor::TaskExecutor::CallbackHandle> scheduleResult =
        _executor->scheduleRemoteCommand(
            request, [this](const auto& x) { return this->_callback(x, kNextBatchFieldName); });

    if (!scheduleResult.isOK()) {
        return scheduleResult.getStatus();
    }

    _getMoreCallbackHandle = scheduleResult.getValue();

    return Status::OK();
}

void Fetcher::_callback(const RemoteCommandCallbackArgs& rcbd, const char* batchFieldName) {
    QueryResponse batchData;
    NextAction nextAction = NextAction::kNoAction;

    ScopeGuard finishCallbackGuard([this, &batchData, &nextAction] {
        if (batchData.cursorId && !batchData.nss.isEmpty() &&
            nextAction != NextAction::kExitAndKeepCursorAlive) {
            _sendKillCursors(batchData.cursorId, batchData.nss);
        }
        _finishCallback();
    });

    if (!rcbd.response.isOK()) {
        _work(rcbd.response.status, nullptr, nullptr);
        return;
    }

    if (_isShuttingDown()) {
        _work(Status(ErrorCodes::CallbackCanceled, "fetcher shutting down"), nullptr, nullptr);
        return;
    }

    const BSONObj& queryResponseObj = rcbd.response.data;
    Status status = getStatusFromCommandResult(queryResponseObj);
    if (!status.isOK()) {
        _work(QueryResponseStatus(status, rcbd.response.getErrorLabels(), rcbd.request.target),
              nullptr,
              nullptr);
        return;
    }

    status = parseCursorResponse(queryResponseObj, batchFieldName, &batchData);
    if (!status.isOK()) {
        _work(status, nullptr, nullptr);
        return;
    }

    batchData.otherFields.metadata = rcbd.response.data;
    batchData.elapsed = rcbd.response.elapsed.value_or(Microseconds{0});
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        batchData.first = _first;
        _first = false;
    }

    if (!batchData.cursorId) {
        _work(batchData, &nextAction, nullptr);
        return;
    }

    nextAction = NextAction::kGetMore;

    BSONObjBuilder bob;
    _work(batchData, &nextAction, &bob);

    // Callback function _work may modify nextAction to request the fetcher
    // not to schedule a getMore command.
    if (nextAction != NextAction::kGetMore) {
        return;
    }

    // Callback function may also disable the fetching of additional data by not filling in the
    // BSONObjBuilder for the getMore command.
    auto cmdObj = bob.obj();
    if (cmdObj.isEmpty()) {
        return;
    }

    status = _scheduleGetMore(cmdObj);
    if (!status.isOK()) {
        nextAction = NextAction::kNoAction;
        _work(status, nullptr, nullptr);
        return;
    }

    finishCallbackGuard.dismiss();
}

void Fetcher::_sendKillCursors(const CursorId id, const NamespaceString& nss) {
    if (id) {
        auto logKillCursorsResult = [](const RemoteCommandCallbackArgs& args) {
            if (!args.response.isOK()) {
                LOGV2_WARNING(23918,
                              "killCursors command task failed",
                              "error"_attr = redact(args.response.status));
                return;
            }
            auto status = getStatusFromCommandResult(args.response.data);
            if (!status.isOK()) {
                LOGV2_WARNING(23919, "killCursors command failed", "error"_attr = redact(status));
            }
        };

        auto cmdObj = BSON("killCursors" << nss.coll() << "cursors" << BSON_ARRAY(id));
        RemoteCommandRequest request(_source, _dbname, cmdObj, nullptr);
        request.sslMode = _sslMode;

        auto scheduleResult = _executor->scheduleRemoteCommand(request, logKillCursorsResult);
        if (!scheduleResult.isOK()) {
            LOGV2_WARNING(23920,
                          "Failed to schedule killCursors command",
                          "error"_attr = redact(scheduleResult.getStatus()));
        }
    }
}
void Fetcher::_finishCallback() {
    // After running callback function, clear '_work' to release any resources that might be held by
    // this function object.
    // '_work' must be moved to a temporary copy and destroyed outside the lock in case there is any
    // logic that's invoked at the function object's destruction that might call into this Fetcher.
    // 'tempWork' must be declared before lock guard 'lk' so that it is destroyed outside the lock.
    Fetcher::CallbackFn tempWork;

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(State::kComplete != _state);
    _state = State::kComplete;
    _first = false;
    _condition.notify_all();

    _completionPromise.emplaceValue();

    invariant(_work);
    std::swap(_work, tempWork);
}

std::ostream& operator<<(std::ostream& os, const Fetcher::State& state) {
    switch (state) {
        case Fetcher::State::kPreStart:
            return os << "PreStart";
        case Fetcher::State::kRunning:
            return os << "Running";
        case Fetcher::State::kShuttingDown:
            return os << "ShuttingDown";
        case Fetcher::State::kComplete:
            return os << "Complete";
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
