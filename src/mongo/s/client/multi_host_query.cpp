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

#include "mongo/s/client/multi_host_query.h"

#include "mongo/bson/util/builder.h"

namespace mongo {

using std::shared_ptr;
using std::make_pair;
using std::string;
using std::vector;

typedef stdx::unique_lock<stdx::mutex> boost_unique_lock;

HostThreadPool::HostThreadPool(int poolSize, bool scopeAllWork)
    : _scopeAllWork(scopeAllWork), _context(new PoolContext) {
    // All threads start as active to avoid races detecting idleness on thread startup -
    // the pool isn't idle until all threads have started waiting.
    _context->numActiveWorkers = poolSize;

    for (int i = 0; i < poolSize; ++i) {
        //
        // Each thread keeps a shared context allowing them to synchronize even if this
        // dispatching pool has already been disposed.
        //

        _threads.push_back(new stdx::thread(stdx::bind(&HostThreadPool::doWork, _context)));
    }
}

void HostThreadPool::schedule(Callback callback) {
    boost_unique_lock lk(_context->mutex);
    _context->scheduled.push_back(callback);
    _context->workScheduledCV.notify_one();
}

void HostThreadPool::doWork(std::shared_ptr<PoolContext> context) {
    while (true) {
        Callback callback;

        {
            boost_unique_lock lk(context->mutex);

            --context->numActiveWorkers;
            if (context->numActiveWorkers == 0)
                context->isIdleCV.notify_all();

            // Wait for work or until we're finished
            while (context->isPoolActive && context->scheduled.empty()) {
                context->workScheduledCV.wait(lk);
            }

            //
            // Either the pool is no longer active, or the queue has some work we should do
            //

            if (!context->isPoolActive)
                return;

            invariant(!context->scheduled.empty());
            callback = context->scheduled.front();
            context->scheduled.pop_front();

            ++context->numActiveWorkers;
        }

        callback();
    }
}

void HostThreadPool::waitUntilIdle() {
    boost_unique_lock lk(_context->mutex);
    while (_context->numActiveWorkers > 0) {
        _context->isIdleCV.wait(lk);
    }
}

HostThreadPool::~HostThreadPool() {
    // Boost can throw on notify(), join(), detach()

    {
        boost_unique_lock lk(_context->mutex);
        _context->isPoolActive = false;
        _context->scheduled.clear();
    }

    DESTRUCTOR_GUARD(_context->workScheduledCV.notify_all();)

    for (vector<stdx::thread*>::iterator it = _threads.begin(); it != _threads.end(); ++it) {
        if (_scopeAllWork) {
            DESTRUCTOR_GUARD((*it)->join();)
        } else {
            DESTRUCTOR_GUARD((*it)->detach();)
        }

        delete *it;
    }
}

HostThreadPools::HostThreadPools(int poolSize, bool scopeAllWork)
    : _poolSize(poolSize), _scopeAllWork(scopeAllWork) {}

void HostThreadPools::schedule(const ConnectionString& host, HostThreadPool::Callback callback) {
    boost_unique_lock lk(_mutex);

    HostPoolMap::iterator seenIt = _pools.find(host);
    if (seenIt == _pools.end()) {
        seenIt = _pools.insert(make_pair(host, new HostThreadPool(_poolSize, _scopeAllWork))).first;
    }

    seenIt->second->schedule(callback);
}

void HostThreadPools::waitUntilIdle(const ConnectionString& host) {
    // Note that this prevents the creation of any new pools - it is only intended to be used
    // for testing.

    boost_unique_lock lk(_mutex);

    HostPoolMap::iterator seenIt = _pools.find(host);
    if (seenIt == _pools.end())
        return;

    seenIt->second->waitUntilIdle();
}

HostThreadPools::~HostThreadPools() {
    boost_unique_lock lk(_mutex);
    for (HostPoolMap::iterator it = _pools.begin(); it != _pools.end(); ++it) {
        delete it->second;
    }
}

MultiHostQueryOp::MultiHostQueryOp(SystemEnv* systemEnv, HostThreadPools* hostThreads)
    : _systemEnv(systemEnv), _hostThreads(hostThreads) {}

StatusWith<DBClientCursor*> MultiHostQueryOp::queryAny(const vector<ConnectionString>& hosts,
                                                       const QuerySpec& query,
                                                       int timeoutMillis) {
    Date_t nowMillis = _systemEnv->currentTimeMillis();
    Date_t timeoutAtMillis = nowMillis + Milliseconds(timeoutMillis);

    // Send out all queries
    scheduleQuery(hosts, query, timeoutAtMillis);

    // Wait for them to come back
    return waitForNextResult(timeoutAtMillis);
}

void MultiHostQueryOp::scheduleQuery(const vector<ConnectionString>& hosts,
                                     const QuerySpec& query,
                                     Date_t timeoutAtMillis) {
    invariant(_pending.empty());

    for (vector<ConnectionString>::const_iterator it = hosts.begin(); it != hosts.end(); ++it) {
        const ConnectionString& host = *it;

        shared_ptr<PendingQueryContext> pendingOp(
            new PendingQueryContext(host, query, timeoutAtMillis, this));

        _pending.insert(make_pair(host, pendingOp));

        HostThreadPool::Callback callback =
            stdx::bind(&MultiHostQueryOp::PendingQueryContext::doBlockingQuery, pendingOp);

        _hostThreads->schedule(host, callback);
    }
}

StatusWith<DBClientCursor*> MultiHostQueryOp::waitForNextResult(Date_t timeoutAtMillis) {
    StatusWith<DBClientCursor*> nextResult(NULL);

    boost_unique_lock lk(_resultsMutex);
    while (!releaseResult_inlock(&nextResult)) {
        Date_t nowMillis = _systemEnv->currentTimeMillis();

        if (nowMillis >= timeoutAtMillis) {
            nextResult = StatusWith<DBClientCursor*>(combineErrorResults_inlock());
            break;
        }

        _nextResultCV.wait_for(lk, timeoutAtMillis - nowMillis);
    }

    dassert(!nextResult.isOK() || nextResult.getValue());
    return nextResult;
}

void MultiHostQueryOp::noteResult(const ConnectionString& host,
                                  StatusWith<DBClientCursor*> result) {
    boost_unique_lock lk(_resultsMutex);
    dassert(_results.find(host) == _results.end());
    _results.insert(make_pair(host, result));

    _nextResultCV.notify_one();
}

/**
 * The results in the result map have four states:
 * Nonexistent - query result still pending
 * Status::OK w/ pointer - successful query result, not yet released to user
 * Status::OK w/ NULL pointer - successful query result, user consumed the result
 * Status::Not OK - error during query
 *
 * This function returns true and the next result to release to the user (or an error
 * if there can be no successful results to release) or false to indicate the user
 * should keep waiting.
 */
bool MultiHostQueryOp::releaseResult_inlock(StatusWith<DBClientCursor*>* nextResult) {
    int numErrors = 0;
    int numReleased = 0;
    for (ResultMap::iterator it = _results.begin(); it != _results.end(); ++it) {
        StatusWith<DBClientCursor*>& result = it->second;

        if (result.isOK() && result.getValue() != NULL) {
            *nextResult = result;
            it->second = StatusWith<DBClientCursor*>(NULL);
            return true;
        } else if (result.isOK()) {
            ++numReleased;
        } else {
            ++numErrors;
        }
    }

    if (numErrors + numReleased == static_cast<int>(_pending.size())) {
        *nextResult = StatusWith<DBClientCursor*>(combineErrorResults_inlock());
        return true;
    }

    return false;
}

/**
 * Goes through the set of results and combines all non-OK results into a single Status.
 * If a single error is found, just returns that error.
 * If no non-OK results are found, assumes the cause is a timeout.
 */
Status MultiHostQueryOp::combineErrorResults_inlock() {
    ErrorCodes::Error code = ErrorCodes::OK;
    StringBuilder errMsg;
    // Whether we should include human-readable codes in the msg - we don't need them if we're
    // not aggregating multiple statuses together
    bool includeHRCodes = false;

    for (ResultMap::const_iterator it = _results.begin(); it != _results.end(); ++it) {
        const StatusWith<DBClientCursor*>& result = it->second;

        if (!result.isOK()) {
            if (code == ErrorCodes::OK) {
                code = result.getStatus().code();
            } else {
                if (!includeHRCodes) {
                    includeHRCodes = true;
                    // Fixup the single error message to include a code
                    errMsg.reset();
                    errMsg.append(Status(code, errMsg.str()).toString());
                }

                code = ErrorCodes::MultipleErrorsOccurred;
                errMsg.append(" :: and :: ");
            }

            errMsg.append(includeHRCodes ? result.getStatus().toString()
                                         : result.getStatus().reason());
            errMsg.append(string(", host ") + it->first.toString());
        }
    }

    if (code == ErrorCodes::OK) {
        return Status(ErrorCodes::NetworkTimeout, "no results were returned in time");
    }

    return Status(code, errMsg.str());
}

MultiHostQueryOp::PendingQueryContext::PendingQueryContext(const ConnectionString& host,
                                                           const QuerySpec& query,
                                                           const Date_t timeoutAtMillis,
                                                           MultiHostQueryOp* parentOp)
    : host(host), query(query), timeoutAtMillis(timeoutAtMillis), parentOp(parentOp) {}

void MultiHostQueryOp::PendingQueryContext::doBlockingQuery() {
    // This *NEEDS* to be around for as long as we're doing queries - i.e. as long as the
    // HostThreadPools is.
    MultiHostQueryOp::SystemEnv* systemEnv;

    // Extract means of doing query from the parent op
    {
        boost_unique_lock lk(parentMutex);

        if (!parentOp)
            return;

        systemEnv = parentOp->_systemEnv;
    }

    // Make sure we're not timed out
    Date_t nowMillis = systemEnv->currentTimeMillis();
    if (nowMillis >= timeoutAtMillis)
        return;

    // Do query
    StatusWith<DBClientCursor*> result = systemEnv->doBlockingQuery(host, query);

    // Push results back to parent op if it's still around
    {
        boost_unique_lock lk(parentMutex);

        if (parentOp)
            parentOp->noteResult(host, result);
        else if (result.isOK())
            delete result.getValue();
    }
}

MultiHostQueryOp::~MultiHostQueryOp() {
    //
    // Orphan all outstanding query contexts that haven't reported back - these will be gc'd
    // once all scheduled query callbacks are finished.
    //

    for (PendingMap::iterator it = _pending.begin(); it != _pending.end(); ++it) {
        shared_ptr<PendingQueryContext>& pendingContext = it->second;

        boost_unique_lock lk(pendingContext->parentMutex);
        pendingContext->parentOp = NULL;
    }

    //
    // Nobody else should be modifying _results now - callbacks don't have access to this op,
    // and other clients should know the op is going out of scope
    //

    for (ResultMap::iterator it = _results.begin(); it != _results.end(); ++it) {
        StatusWith<DBClientCursor*>& result = it->second;

        if (result.isOK()) {
            delete result.getValue();
        }
    }
}
}
