/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/repl/repl_worker_pool_thread_count.h"

#include "mongo/db/repl/repl_writer_thread_pool_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/util/processinfo.h"

#include <algorithm>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

size_t getMinThreadCountForReplWorkerPool() {
    return static_cast<size_t>(replWriterMinThreadCount);
}

size_t getThreadCountForReplWorkerPool() {
    return std::min(static_cast<size_t>(replWriterThreadCount),
                    static_cast<size_t>(2 * ProcessInfo::getNumAvailableCores()));
}

Status validateUpdateReplWriterThreadCount(const int count, const boost::optional<TenantId>&) {
    if (count < replWriterMinThreadCount) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterThreadCount must be greater or equal to '"
                                    << replWriterMinThreadCount
                                    << "', which is the current value of replWriterMinThreadCount");
    }
    if (!std::in_range<size_t>(count)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterThreadCount must be greater than 0'");
    }
    size_t newCount = static_cast<size_t>(count);
    size_t numCores = ProcessInfo::getNumAvailableCores();
    size_t maxThreads = 2 * numCores;
    if (newCount > maxThreads) {
        LOGV2_WARNING(11280003,
                      "replWriterThreadCount is set to higher than the max number of threads for "
                      "the writer pool, which is 2 * the number of cores available. The pool size "
                      "will be capped at 2 * the number of cores.",
                      "replWriterThreadCount"_attr = std::to_string(newCount),
                      "maxThreads"_attr = std::to_string(maxThreads),
                      "numCores"_attr = std::to_string(numCores));
    }

    return Status::OK();
}
Status validateUpdateReplWriterMinThreadCount(const int count, const boost::optional<TenantId>&) {
    if (!std::in_range<size_t>(count)) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterMinThreadCount must be greater than 0'");
    }
    size_t newCount = static_cast<size_t>(count);
    // May be replWriterThreadCount, or may be capped by the number of CPUs
    size_t poolActualSize = getThreadCountForReplWorkerPool();
    if (newCount > poolActualSize) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "replWriterMinThreadCount must be less than or equal to '"
                                    << poolActualSize
                                    << "', which is the current max threads for the thread pool");
    }
    return Status::OK();
}

Status onUpdateReplWriterThreadCount(const int) {
    // Reduce content pinned in cache by single oplog batch on small machines by reducing the number
    // of threads of ReplWriter to reduce the number of concurrent open WT transactions.

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
