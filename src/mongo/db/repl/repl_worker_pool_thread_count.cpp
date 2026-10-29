// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/repl_worker_pool_thread_count.h"

#include "mongo/db/repl/repl_writer_thread_pool_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <algorithm>
#include <mutex>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

size_t getMinThreadCountForReplWorkerPool() {
    return static_cast<size_t>(replWriterMinThreadCount);
}

size_t getThreadCountForReplWorkerPool() {
    return std::min(static_cast<size_t>(replWriterThreadCount),
                    static_cast<size_t>(4 * ProcessInfo::getNumAvailableCores()));
}


namespace {
/* Mutex used to prevent concurrent setting of replWriterThreadCount and replWriterMinThreadCount.
 * This is the workflow of setting one of the parameters:
 * 1. validateUpdateXXX() is called before setting the param value. We lock the mutex.
 * 2. param is set.
 * 3. onUpdateXXX() is called after setting the param value. We unlock the mutex.
 */
std::mutex threadCountParamsMutex;
// We are using a boost::optional in order to hold the unique_lock between step 1 and 3.
boost::optional<std::unique_lock<std::mutex>> threadCountParamsLocker;
}  // namespace

Status validateUpdateReplWriterThreadCount(const int count, const boost::optional<TenantId>&) {
    // This range check must be the same as in the repl_writer_thread_pool_server_parameters.idl
    // file. We validate it here to avoid locking the mutex if the value is out of range.
    if (count <= 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for parameter replWriterThreadCount: "
                                       "must be greater than 0");
    }
    if (count > 256) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for parameter replWriterThreadCount: "
                                       "must be less than or equal to 256");
    }

    std::unique_lock<std::mutex> lk(threadCountParamsMutex);

    if (count < replWriterMinThreadCount) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterThreadCount must be greater or equal to '"
                                    << replWriterMinThreadCount
                                    << "', which is the current value of replWriterMinThreadCount");
    }

    size_t newCount = static_cast<size_t>(count);
    size_t numCores = ProcessInfo::getNumAvailableCores();
    size_t maxThreads = 4 * numCores;
    if (newCount > maxThreads) {
        LOGV2_WARNING(11280003,
                      "replWriterThreadCount is set to higher than the max number of threads for "
                      "the writer pool, which is 4 * the number of cores available. The pool size "
                      "will be capped at 4 * the number of cores.",
                      "replWriterThreadCount"_attr = std::to_string(newCount),
                      "maxThreads"_attr = std::to_string(maxThreads),
                      "numCores"_attr = std::to_string(numCores));
    }

    // Moving ownership of the lock while leaving the mutex locked
    threadCountParamsLocker.emplace(std::move(lk));
    return Status::OK();
}

Status validateUpdateReplWriterMinThreadCount(const int count, const boost::optional<TenantId>&) {
    // This range check must be the same as in the repl_writer_thread_pool_server_parameters.idl
    // file. We validate it here to avoid locking the mutex if the value is out of range.
    if (count < 0) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for parameter replWriterMinThreadCount: "
                                       "must be greater or equal to 0");
    }
    if (count > 256) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid value for parameter replWriterMinThreadCount: "
                                       "must be less than or equal to 256");
    }

    std::unique_lock<std::mutex> lk(threadCountParamsMutex);

    size_t newCount = static_cast<size_t>(count);
    // May be replWriterThreadCount, or may be capped by the number of CPUs
    size_t poolActualSize = getThreadCountForReplWorkerPool();
    if (newCount > poolActualSize) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterMinThreadCount must be less than or equal to '"
                                    << poolActualSize
                                    << "', which is the current max threads for the thread pool");
    }

    // Moving ownership of the lock while leaving the mutex locked
    threadCountParamsLocker.emplace(std::move(lk));
    return Status::OK();
}

Status onUpdateReplWriterThreadCount(const int) {
    // Reduce content pinned in cache by single oplog batch on small machines by reducing the number
    // of threads of ReplWriter to reduce the number of concurrent open WT transactions.

    // Here we adopt the ownership of the locker without locking the underlying mutex as it was
    // previously locked by validateUpdateReplWriterThreadCount().
    std::unique_lock<std::mutex> lk(std::move(threadCountParamsLocker.get()));
    threadCountParamsLocker.reset();

    if (hasGlobalServiceContext()) {
        // If the global service context is set, then we're past startup, so we need to update the
        // repl worker thread pool.
        auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());
        auto replWorkThreadPool = replCoord->getDbWorkThreadPool();
        if (replWorkThreadPool) {
            replWorkThreadPool->setMaxThreads(getThreadCountForReplWorkerPool());
        }
    }

    return Status::OK();
}

Status onUpdateReplWriterMinThreadCount(const int) {
    // Here we adopt the ownership of the locker without locking the underlying mutex as it was
    // previously locked by validateUpdateReplWriterMinThreadCount().
    std::unique_lock<std::mutex> lk(std::move(threadCountParamsLocker.get()));
    threadCountParamsLocker.reset();

    if (hasGlobalServiceContext()) {
        // If the global service context is set, then we're past startup, so we need to update the
        // repl worker thread pool.
        auto replCoord = ReplicationCoordinator::get(getGlobalServiceContext());
        auto replWorkThreadPool = replCoord->getDbWorkThreadPool();
        if (replWorkThreadPool) {
            replWorkThreadPool->setMinThreads(getMinThreadCountForReplWorkerPool());
        }
    }

    return Status::OK();
}

}  // namespace repl
}  // namespace mongo
