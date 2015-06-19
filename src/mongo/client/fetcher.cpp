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

#include "mongo/client/fetcher.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

    const char* kCursorFieldName = "cursor";
    const char* kCursorIdFieldName = "id";
    const char* kNamespaceFieldName = "ns";

    const char* kFirstBatchFieldName = "firstBatch";
    const char* kNextBatchFieldName = "nextBatch";

    /**
     * Parses cursor response in command result for cursor ID, namespace and documents.
     * 'batchFieldName' will be 'firstBatch' for the initial remote command invocation and
     * 'nextBatch' for getMore.
     */
    Status parseCursorResponse(const BSONObj& obj,
                               const std::string& batchFieldName,
                               Fetcher::QueryResponse* batchData) {
        invariant(batchFieldName == kFirstBatchFieldName || batchFieldName == kNextBatchFieldName);
        invariant(batchData);

        BSONElement cursorElement = obj.getField(kCursorFieldName);
        if (cursorElement.eoo()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "cursor response must contain '" << kCursorFieldName <<
                          "' field: " << obj);
        }
        if (!cursorElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "'" << kCursorFieldName << "' field must be an object: " << obj);
        }
        BSONObj cursorObj = cursorElement.Obj();

        BSONElement cursorIdElement = cursorObj.getField(kCursorIdFieldName);
        if (cursorIdElement.eoo()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "cursor response must contain '" << kCursorFieldName << "." <<
                          kCursorIdFieldName << "' field: " << obj);
        }
        if (!(cursorIdElement.type() == mongo::NumberLong ||
                cursorIdElement.type() == mongo::NumberInt)) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "'" << kCursorFieldName << "." << kCursorIdFieldName <<
                          "' field must be a integral number of type 'int' or 'long' but was a '"
                          << typeName(cursorIdElement.type()) << "': " << obj);
        }
        batchData->cursorId = cursorIdElement.numberLong();

        BSONElement namespaceElement = cursorObj.getField(kNamespaceFieldName);
        if (namespaceElement.eoo()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "cursor response must contain " <<
                          "'" << kCursorFieldName << "." << kNamespaceFieldName << "' field: " <<
                          obj);
        }
        if (namespaceElement.type() != mongo::String) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "'" << kCursorFieldName << "." << kNamespaceFieldName <<
                          "' field must be a string: " << obj);
        }
        NamespaceString tempNss(namespaceElement.valuestrsafe());
        if (!tempNss.isValid()) {
            return Status(ErrorCodes::BadValue, str::stream() <<
                          "'" << kCursorFieldName << "." << kNamespaceFieldName <<
                          "' contains an invalid namespace: " << obj);
        }
        batchData->nss = tempNss;

        BSONElement batchElement = cursorObj.getField(batchFieldName);
        if (batchElement.eoo()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "cursor response must contain '" << kCursorFieldName << "." <<
                          batchFieldName << "' field: " << obj);
        }
        if (!batchElement.isABSONObj()) {
            return Status(ErrorCodes::FailedToParse, str::stream() <<
                          "'" << kCursorFieldName << "." << batchFieldName <<
                          "' field must be an array: " << obj);
        }
        BSONObj batchObj = batchElement.Obj();
        for (auto itemElement : batchObj) {
            if (!itemElement.isABSONObj()) {
                return Status(ErrorCodes::FailedToParse, str::stream() <<
                              "found non-object " << itemElement << " in " <<
                              "'" << kCursorFieldName << "." << batchFieldName << "' field: " <<
                              obj);
            }
            batchData->documents.push_back(itemElement.Obj().getOwned());
        }

        return Status::OK();
    }

    Status parseReplResponse(const BSONObj& obj) {
        return Status::OK();
    }
    
} // namespace

    Fetcher::QueryResponse::QueryResponse(CursorId theCursorId,
                                          const NamespaceString& theNss,
                                          Documents theDocuments)
        : cursorId(theCursorId),
          nss(theNss),
          documents(theDocuments) { }

    Fetcher::Fetcher(executor::TaskExecutor* executor,
                     const HostAndPort& source,
                     const std::string& dbname,
                     const BSONObj& findCmdObj,
                     const CallbackFn& work)
        : _executor(executor),
          _source(source),
          _dbname(dbname),
          _cmdObj(findCmdObj.getOwned()),
          _work(work),
          _active(false),
          _remoteCommandCallbackHandle() {

        uassert(ErrorCodes::BadValue, "null replication executor", executor);
        uassert(ErrorCodes::BadValue, "database name cannot be empty", !dbname.empty());
        uassert(ErrorCodes::BadValue, "command object cannot be empty", !findCmdObj.isEmpty());
        uassert(ErrorCodes::BadValue, "callback function cannot be null", work);
    }

    Fetcher::~Fetcher() {
        DESTRUCTOR_GUARD(
            cancel();
            wait();
        );
    }

    std::string Fetcher::getDiagnosticString() const {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        str::stream output;
        output << "Fetcher";
        output << " executor: " << _executor->getDiagnosticString();
        output << " source: " << _source.toString();
        output << " database: " << _dbname;
        output << " query: " << _cmdObj;
        output << " active: " << _active;
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
                RemoteCommandRequest(_source, _dbname, cmdObj),
                stdx::bind(&Fetcher::_callback, this, stdx::placeholders::_1, batchFieldName));

        if (!scheduleResult.isOK()) {
            return scheduleResult.getStatus();
        }

        _active = true;
        _remoteCommandCallbackHandle = scheduleResult.getValue();
        return Status::OK();
    }

    void Fetcher::_callback(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcbd,
                            const char* batchFieldName) {

        if (!rcbd.response.isOK()) {
            _work(StatusWith<Fetcher::QueryResponse>(rcbd.response.getStatus()), nullptr, nullptr);
            _finishCallback();
            return;
        }

        const BSONObj& queryResponseObj = rcbd.response.getValue().data;
        Status status = getStatusFromCommandResult(queryResponseObj);
        if (!status.isOK()) {
            _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
            _finishCallback();
            return;
        }

        status = parseReplResponse(queryResponseObj);
        if (!status.isOK()) {
            _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
            _finishCallback();
            return;
        }

        QueryResponse batchData;
        status = parseCursorResponse(queryResponseObj, batchFieldName, &batchData);
        if (!status.isOK()) {
            _work(StatusWith<Fetcher::QueryResponse>(status), nullptr, nullptr);
            _finishCallback();
            return;
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
            _finishCallback();
            return;
        }

        // Callback function may also disable the fetching of additional data by not filling in the
        // BSONObjBuilder for the getMore command.
        auto cmdObj = bob.obj();
        if (cmdObj.isEmpty()) {
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
            _finishCallback();
            return;
        }
    }

    void Fetcher::_finishCallback() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _active = false;
        _condition.notify_all();
    }

} // namespace mongo
