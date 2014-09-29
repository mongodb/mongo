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

#include "mongo/db/repl/network_interface_impl.h"

#include <boost/thread.hpp>
#include <sstream>

#include "mongo/client/connpool.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

    static const size_t kNumThreads = 8;

    NetworkInterfaceImpl::NetworkInterfaceImpl() : _isExecutorRunnable(false), _inShutdown(false) {}

    NetworkInterfaceImpl::~NetworkInterfaceImpl() { }

    void NetworkInterfaceImpl::startup() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(!_inShutdown);
        if (!_threads.empty()) {
            return;
        }
        for (size_t i = 0; i < kNumThreads; ++i) {
            _threads.push_back(
                    boost::shared_ptr<boost::thread>(
                            new boost::thread(
                                    stdx::bind(&NetworkInterfaceImpl::_consumeNetworkRequests,
                                               this))));
        }
    }

    void NetworkInterfaceImpl::shutdown() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        _inShutdown = true;
        _hasPending.notify_all();
        lk.unlock();
        std::for_each(_threads.begin(),
                      _threads.end(),
                      stdx::bind(&boost::thread::join, stdx::placeholders::_1));
    }

    void NetworkInterfaceImpl::signalWorkAvailable() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _signalWorkAvailable_inlock();
    }

    void NetworkInterfaceImpl::_signalWorkAvailable_inlock() {
        if (!_isExecutorRunnable) {
            _isExecutorRunnable = true;
            _isExecutorRunnableCondition.notify_one();
        }
    }

    void NetworkInterfaceImpl::waitForWork() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_isExecutorRunnable) {
            _isExecutorRunnableCondition.wait(lk);
        }
        _isExecutorRunnable = false;
    }

    void NetworkInterfaceImpl::waitForWorkUntil(Date_t when) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_isExecutorRunnable) {
            const Milliseconds waitTime(when - now());
            if (waitTime <= Milliseconds(0)) {
                break;
            }
            _isExecutorRunnableCondition.timed_wait(lk, waitTime);
        }
        _isExecutorRunnable = false;
    }

    void NetworkInterfaceImpl::_consumeNetworkRequests() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_inShutdown) {
            if (_pending.empty()) {
                _hasPending.wait(lk);
                continue;
            }
            CommandData todo = _pending.front();
            _pending.pop_front();
            lk.unlock();
            todo.onFinish(_runCommand(todo.request));
            lk.lock();
            _signalWorkAvailable_inlock();
        }
    }

    void NetworkInterfaceImpl::startCommand(
            const ReplicationExecutor::CallbackHandle& cbHandle,
            const ReplicationExecutor::RemoteCommandRequest& request,
            const RemoteCommandCompletionFn& onFinish) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        _pending.push_back(CommandData());
        CommandData& cd = _pending.back();
        cd.cbHandle = cbHandle;
        cd.request = request;
        cd.onFinish = onFinish;
        _hasPending.notify_one();
    }

    void NetworkInterfaceImpl::cancelCommand(const ReplicationExecutor::CallbackHandle& cbHandle) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        CommandDataList::iterator iter;
        for (iter = _pending.begin(); iter != _pending.end(); ++iter) {
            if (iter->cbHandle == cbHandle) {
                break;
            }
        }
        if (iter == _pending.end()) {
            return;
        }
        const RemoteCommandCompletionFn onFinish = iter->onFinish;
        _pending.erase(iter);
        lk.unlock();
        onFinish(ResponseStatus(ErrorCodes::CallbackCanceled, "Callback canceled"));
        lk.lock();
        _signalWorkAvailable_inlock();
    }

    Date_t NetworkInterfaceImpl::now() {
        return curTimeMillis64();
    }

    namespace {
        // Duplicated in mock impl
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
    } //namespace

    ResponseStatus NetworkInterfaceImpl::_runCommand(
            const ReplicationExecutor::RemoteCommandRequest& request) {

        try {
            BSONObj output;

            StatusWith<int> timeoutStatus = getTimeoutMillis(request.expirationDate, now());
            if (!timeoutStatus.isOK())
                return ResponseStatus(timeoutStatus.getStatus());

            int timeout = timeoutStatus.getValue();
            Timer timer;
            ScopedDbConnection conn(request.target.toString(), timeout);
            conn->runCommand(request.dbname, request.cmdObj, output);
            conn.done();
            return ResponseStatus(Response(output, Milliseconds(timer.millis())));
        }
        catch (const DBException& ex) {
            return ResponseStatus(ex.toStatus());
        }
        catch (const std::exception& ex) {
            return ResponseStatus(
                    ErrorCodes::UnknownError,
                    mongoutils::str::stream() <<
                    "Sending command " << request.cmdObj << " on database " << request.dbname <<
                    " over network to " << request.target.toString() << " received exception " <<
                    ex.what());
        }
    }

    void NetworkInterfaceImpl::runCallbackWithGlobalExclusiveLock(
            const stdx::function<void (OperationContext*)>& callback) {

        std::ostringstream sb;
        sb << "repl" << boost::this_thread::get_id();
        Client::initThreadIfNotAlready(sb.str().c_str());
        OperationContextImpl txn;
        Lock::GlobalWrite lk(txn.lockState());
        callback(&txn);
    }

}  // namespace repl
} // namespace mongo
