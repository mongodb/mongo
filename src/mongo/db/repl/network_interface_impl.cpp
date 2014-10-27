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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/network_interface_impl.h"

#include <boost/thread.hpp>
#include <memory>
#include <sstream>

#include "mongo/client/connpool.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/connections.h"  // For ScopedConn::keepOpen
#include "mongo/platform/unordered_map.h"
#include "mongo/stdx/list.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/timer.h"

namespace mongo {
namespace repl {

namespace {
    const size_t kNumThreads = 8;
    Seconds kDefaultMaxIdleConnectionAge(60);

}  // namespace

    /**
     * Private pool of connections used by the network interface.
     *
     * Methods of the pool may be called from any thread, as they are synchronized internally.
     */
    class NetworkInterfaceImpl::ConnectionPool {
        MONGO_DISALLOW_COPYING(ConnectionPool);
    public:
        struct ConnectionInfo;

        /**
         * Options for configuring the pool.
         */
        struct Options {
            Options() : maxIdleConnectionAge(kDefaultMaxIdleConnectionAge) {}

            // Maximum age of an idle connection, as measured since last operation completion,
            // before it is reaped.
            Seconds maxIdleConnectionAge;
        };

        typedef stdx::list<ConnectionInfo> ConnectionList;
        typedef unordered_map<HostAndPort, ConnectionList> HostConnectionMap;

        /**
         * RAII class for connections from the pool.  To use the connection pool, instantiate one of
         * these with a pointer to the pool, the identity of the target node and the timeout for
         * network operations, use it like a pointer to a connection, and then call done() on
         * successful completion.  Failure to call done() will lead to the connection being reaped
         * when the holder goes out of scope.
         */
        class ConnectionPtr {
            MONGO_DISALLOW_COPYING(ConnectionPtr);
        public:
            /**
             * Constructs a ConnectionPtr referring to a connection to "target" drawn from "pool",
             * with the network timeout set to "timeout".
             *
             * Throws DBExceptions if the connection cannot be established.
             */
            ConnectionPtr(ConnectionPool* pool,
                          const HostAndPort& target,
                          Seconds timeout) :
                _pool(pool), _connInfo(pool->acquireConnection(target, timeout)) {}

            /**
             * Destructor reaps the connection if it wasn't already returned to the pool by calling
             * done().
             */
            ~ConnectionPtr() { if (_pool) _pool->destroyConnection(_connInfo); }

            /**
             * Releases the connection back to the pool from which it was drawn.
             */
            void done() { _pool->releaseConnection(_connInfo); _pool = NULL; }

            DBClientConnection& operator*();
            DBClientConnection* operator->();

        private:
            ConnectionPool* _pool;
            const ConnectionList::iterator _connInfo;
        };

        /**
         * Constructs a new connection pool, configured with the given options.
         */
        explicit ConnectionPool(const Options& options);

        ~ConnectionPool();

        /**
         * Acquires a connection to "target" with the given "timeout", or throws a DBException.
         * Intended for use by ConnectionPtr.
         */
        ConnectionList::iterator acquireConnection(const HostAndPort& target, Seconds timeout);

        /**
         * Releases a connection back into the pool.
         * Intended for use by ConnectionPtr.
         * Call this for connections that can safely be reused.
         */
        void releaseConnection(ConnectionList::iterator);

        /**
         * Destroys a connection previously acquired from the pool.
         * Intended for use by ConnectionPtr.
         * Call this for connections that cannot be reused.
         */
        void destroyConnection(ConnectionList::iterator);

        /**
         * Closes all connections currently in use, to ensure that the network threads
         * terminate promptly during shutdown.
         */
        void closeAllInUseConnections();

        /**
         * Reap all connections in the pool that have been there continuously since before
         * "date".
         */
        void cleanUpOlderThan(Date_t date);

    private:
        /**
         * Implementation of cleanUpOlderThan which assumes that _mutex is already held.
         */
        void cleanUpOlderThan_inlock(Date_t date);

        /**
         * Reaps connections in "hostConns" that were already in the pool as of "date".  Expects
         * _mutex to be held.
         */
        void cleanUpOlderThan_inlock(Date_t date, ConnectionList* hostConns);

        // Mutex guarding members of the connection pool
        boost::mutex _mutex;

        // Map from HostAndPort to free connections.
        HostConnectionMap _connections;
        ConnectionList _inUseConnections;

        // Options with which this pool was configured.
        Options _options;
    };

    /**
     * Information about a connection in the pool.
     */
    struct NetworkInterfaceImpl::ConnectionPool::ConnectionInfo {
        ConnectionInfo() : conn(NULL), lastEnteredPoolDate(0ULL) {}
        ConnectionInfo(DBClientConnection* theConn, Date_t date) :
            conn(theConn),
            lastEnteredPoolDate(date) {}

        /**
         * Returns true if the connection entered the pool  "date".
         */
        bool isNotNewerThan(Date_t date) { return lastEnteredPoolDate <= date; }

        // A connection in the pool.
        DBClientConnection* conn;

        // The date at which the connection "conn" was last put into the pool.
        Date_t lastEnteredPoolDate;
    };

    DBClientConnection& NetworkInterfaceImpl::ConnectionPool::ConnectionPtr::operator*() {
        return *_connInfo->conn;
    }

    DBClientConnection* NetworkInterfaceImpl::ConnectionPool::ConnectionPtr::operator->() {
        return _connInfo->conn;
    }

    NetworkInterfaceImpl::ConnectionPool::ConnectionPool(const Options& options) :
        _options(options) {}

    NetworkInterfaceImpl::ConnectionPool::~ConnectionPool() {
        cleanUpOlderThan(Date_t(~0ULL));
        invariant(_connections.empty());
        invariant(_inUseConnections.empty());
    }

    void NetworkInterfaceImpl::ConnectionPool::cleanUpOlderThan(Date_t date) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        cleanUpOlderThan_inlock(date);
    }

    void NetworkInterfaceImpl::ConnectionPool::cleanUpOlderThan_inlock(Date_t date) {
        HostConnectionMap::iterator hostConns = _connections.begin();
        while (hostConns != _connections.end()) {
            cleanUpOlderThan_inlock(date, &hostConns->second);
            if (hostConns->second.empty()) {
                _connections.erase(hostConns++);
            }
            else {
                ++hostConns;
            }
        }
    }

    void NetworkInterfaceImpl::ConnectionPool::cleanUpOlderThan_inlock(
            Date_t date,
            ConnectionList* hostConns) {
        ConnectionList::iterator iter = hostConns->begin();
        while (iter != hostConns->end()) {
            if (iter->isNotNewerThan(date)) {
                ConnectionList::iterator toDelete = iter++;
                delete toDelete->conn;
                hostConns->erase(toDelete);
            }
            else {
                ++iter;
            }
        }
    }

    void NetworkInterfaceImpl::ConnectionPool::closeAllInUseConnections() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        for (ConnectionList::iterator iter = _inUseConnections.begin();
             iter != _inUseConnections.end();
             ++iter) {

            iter->conn->port().shutdown();
        }
    }

    NetworkInterfaceImpl::ConnectionPool::ConnectionList::iterator
    NetworkInterfaceImpl::ConnectionPool::acquireConnection(
            const HostAndPort& target,
            Seconds timeout) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        for (HostConnectionMap::iterator hostConns;
             ((hostConns = _connections.find(target)) != _connections.end()) &&
                 !hostConns->second.empty();) {

            _inUseConnections.splice(_inUseConnections.begin(),
                                     hostConns->second,
                                     hostConns->second.begin());
            const ConnectionList::iterator candidate = _inUseConnections.begin();
            lk.unlock();
            try {
                if (candidate->conn->isStillConnected()) {
                    candidate->conn->setSoTimeout(timeout.total_seconds());
                    return candidate;
                }
            }
            catch (...) {
                lk.lock();
                _inUseConnections.erase(candidate);
                throw;
            }
            lk.lock();
            _inUseConnections.erase(candidate);
        }

        // No free connection in the pool; make a new one.
        lk.unlock();
        std::auto_ptr<DBClientConnection> conn(new DBClientConnection);
        conn->setSoTimeout(timeout.total_seconds());
        std::string errmsg;
        uassert(18915,
                str::stream() << "Failed attempt to connect to " << target.toString() << "; " <<
                errmsg,
                conn->connect(target, errmsg));
        conn->port().tag |= ScopedConn::keepOpen;
        if (getGlobalAuthorizationManager()->isAuthEnabled()) {
            uassert(ErrorCodes::AuthenticationFailed,
                    "Missing credentials for authenticating as internal user",
                    isInternalAuthSet());
            conn->auth(getInternalUserAuthParamsWithFallback());
        }
        lk.lock();
        return _inUseConnections.insert(
                _inUseConnections.begin(),
                ConnectionInfo(conn.release(), Date_t(0)));
    }

    void NetworkInterfaceImpl::ConnectionPool::releaseConnection(ConnectionList::iterator iter) {
        const Date_t now(curTimeMillis64());
        boost::lock_guard<boost::mutex> lk(_mutex);
        ConnectionList& hostConns = _connections[iter->conn->getServerHostAndPort()];
        cleanUpOlderThan_inlock(
                now - _options.maxIdleConnectionAge.total_seconds(),
                &hostConns);
        iter->lastEnteredPoolDate = now;
        hostConns.splice(hostConns.begin(), _inUseConnections, iter);
    }

    void NetworkInterfaceImpl::ConnectionPool::destroyConnection(ConnectionList::iterator iter) {
        boost::lock_guard<boost::mutex> lk(_mutex);
        delete iter->conn;
        _inUseConnections.erase(iter);
    }

    NetworkInterfaceImpl::NetworkInterfaceImpl() :
        _isExecutorRunnable(false),
        _inShutdown(false) {
        ConnectionPool::Options options;
        _connPool.reset(new ConnectionPool(options));
    }

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
        _connPool->closeAllInUseConnections();
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
            ResponseStatus result = _runCommand(todo.request);
            LOG(2) << "Network status of sending " << todo.request.cmdObj.firstElementFieldName() <<
                " to " << todo.request.target << " was " << result.getStatus();
            todo.onFinish(result);
            lk.lock();
            _signalWorkAvailable_inlock();
        }
    }

    void NetworkInterfaceImpl::startCommand(
            const ReplicationExecutor::CallbackHandle& cbHandle,
            const ReplicationExecutor::RemoteCommandRequest& request,
            const RemoteCommandCompletionFn& onFinish) {
        LOG(2) << "Scheduling " << request.cmdObj.firstElementFieldName() << " to " <<
            request.target;
        boost::lock_guard<boost::mutex> lk(_mutex);
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
        LOG(2) << "Canceled sending " << iter->request.cmdObj.firstElementFieldName() << " to " <<
            iter->request.target;
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

            Seconds timeout(timeoutStatus.getValue());
            Timer timer;
            ConnectionPool::ConnectionPtr conn(_connPool.get(), request.target, timeout);
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
