/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/repl/network_interface_mock.h"

#include <map>

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    bool operator<(const ReplicationExecutor::RemoteCommandRequest& lhs,
                   const ReplicationExecutor::RemoteCommandRequest& rhs) {
        if (lhs.target < rhs.target)
            return true;
        if (rhs.target < lhs.target)
            return false;
        if (lhs.dbname < rhs.dbname)
            return true;
        if (rhs.dbname < lhs.dbname)
            return false;
        return lhs.cmdObj < rhs.cmdObj;
    }

    namespace {
        // Duplicated in real impl
        StatusWith<int> getTimeoutMillis(Date_t expDate, Date_t nowDate) {
            // check for timeout
            int timeout = 0;
            if (expDate != ReplicationExecutor::kNoExpirationDate) {
                timeout = expDate >= nowDate ? expDate - nowDate :
                                               ReplicationExecutor::kNoTimeout.total_milliseconds();
                if (timeout < 0 ) {
                    return StatusWith<int>(ErrorCodes::ExceededTimeLimit,
                                               str::stream() << "Went to run command,"
                                               " but it was too late. Expiration was set to "
                                                             << expDate);
                }
            }
            return StatusWith<int>(timeout);
        }

        ResponseStatus returnErrorCommmandProcessor(const ReplicationExecutor::RemoteCommandRequest&
                                                                                        request) {
            return ResponseStatus(ErrorCodes::NoSuchKey,
                                  "No command processor configured for this mock");
        }
    } //namespace

    NetworkInterfaceMock::NetworkInterfaceMock()  :
                    _simulatedNetworkLatencyMillis(0),
                    _helper(returnErrorCommmandProcessor) {
        StatusWith<Date_t> initialNow = dateFromISOString("2014-08-01T00:00:00Z");
        fassert(18653, initialNow.getStatus());
        _now = initialNow.getValue();
    }

    NetworkInterfaceMock::NetworkInterfaceMock(CommandProcessorFn fn)  :
                    _simulatedNetworkLatencyMillis(0),
                    _helper(fn) {
        StatusWith<Date_t> initialNow = dateFromISOString("2014-08-01T00:00:00Z");
        fassert(18655, initialNow.getStatus());
        _now = initialNow.getValue();
    }

    NetworkInterfaceMock::~NetworkInterfaceMock() {}

    void NetworkInterfaceMock::setExecutor(ReplicationExecutor* executor) {
        _executor = executor;
    }

    Date_t NetworkInterfaceMock::now() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _now;
    }

    void NetworkInterfaceMock::setNow(Date_t newNow) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(newNow.asInt64() > _now.asInt64());
        _now = newNow;
        _executor->signalWorkForTest();
        _timeElapsed.notify_all();
    }

    void NetworkInterfaceMock::incrementNow(Milliseconds inc) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(inc.total_milliseconds() > 0);
        _now += inc.total_milliseconds();
        _executor->signalWorkForTest();
        _timeElapsed.notify_all();
    }

    ResponseStatus NetworkInterfaceMock::runCommand(
            const ReplicationExecutor::RemoteCommandRequest& request) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        Date_t wakeupTime = _now + _simulatedNetworkLatencyMillis;
        while (wakeupTime < _now) {
            _timeElapsed.wait(lk);
        }

        StatusWith<int> toStatus = getTimeoutMillis(request.expirationDate, _now);
        if (!toStatus.isOK())
            return ResponseStatus(toStatus.getStatus());

        lk.unlock();
        return _helper(request);
    }

    void NetworkInterfaceMock::simulatedNetworkLatency(int millis) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _simulatedNetworkLatencyMillis = millis;
    }

    void NetworkInterfaceMock::runCallbackWithGlobalExclusiveLock(
            const stdx::function<void (OperationContext* txn)>& callback) {

        OperationContextNoop txn;
        callback(&txn);
    }
    NetworkInterfaceMockWithMap::NetworkInterfaceMockWithMap()
            : NetworkInterfaceMock(stdx::bind(&NetworkInterfaceMockWithMap::_getResponseFromMap,
                                              this,
                                              stdx::placeholders::_1)){
    }

    bool NetworkInterfaceMockWithMap::addResponse(
            const ReplicationExecutor::RemoteCommandRequest& request,
            const StatusWith<BSONObj>& response,
            bool isBlocked) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _responses.insert(std::make_pair(request,
                                                BlockableResponseStatus(
                                                     !response.isOK() ?
                                                            ResponseStatus(response.getStatus()) :
                                                            ResponseStatus(Response(
                                                                               response.getValue(),
                                                                               Milliseconds(0)))
                                                     , isBlocked))).second;
    }

    void NetworkInterfaceMockWithMap::unblockResponse(
            const ReplicationExecutor::RemoteCommandRequest& request) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        RequestResponseMap::iterator iter = _responses.find(request);
        if (_responses.end() == iter) {
            return;
        }
        if (iter->second.isBlocked) {
            iter->second.isBlocked = false;
            _someResponseUnblocked.notify_all();
        }
    }

    void NetworkInterfaceMockWithMap::unblockAll() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (RequestResponseMap::iterator iter = _responses.begin();
             iter != _responses.end();
             ++iter) {
            iter->second.isBlocked = false;
        }
        _someResponseUnblocked.notify_all();
    }

    ResponseStatus NetworkInterfaceMockWithMap::_getResponseFromMap(
                                const ReplicationExecutor::RemoteCommandRequest& request) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (1) {
            BlockableResponseStatus result = mapFindWithDefault(
                                        _responses,
                                        request,
                                        BlockableResponseStatus(
                                            ResponseStatus(
                                                 ErrorCodes::NoSuchKey,
                                                 str::stream() << "Could not find response for " <<
                                                 "Request(" << request.target.toString() << ", " <<
                                                 request.dbname << ", " << request.cmdObj << ')'),
                                            false));
            if (!result.isBlocked) {
                return result.response;
            }
            _someResponseUnblocked.wait(lk);
        }
    }

    NetworkInterfaceMockWithMap::BlockableResponseStatus::BlockableResponseStatus(
                                                const ResponseStatus& r,
                                                bool blocked) :
        response(r),
        isBlocked(blocked) {
    }

    std::string NetworkInterfaceMockWithMap::BlockableResponseStatus::toString() const {
        str::stream out;
        out <<  "BlockableResponseStatus -- isBlocked:" << isBlocked;
        if (response.isOK())
            out << " bson:" << response.getValue().data
                << " millis:"
                << response.getValue().elapsedMillis.total_milliseconds();
        else
            out << " error:" << response.getStatus().toString();
        return out;

    }
}  // namespace repl
}  // namespace mongo
