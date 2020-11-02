/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

#include "mongo/executor/async_request_executor.h"

#include "mongo/db/client_strand.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeRunningAsyncRequestExecutorTask);

AsyncRequestExecutor::AsyncRequestExecutor(std::string name) : _name(std::move(name)) {
    ThreadPool::Options options;
    options.minThreads = 0;
    options.maxThreads = 1;
    _pool = std::make_unique<ThreadPool>(std::move(options));
    _pool->startup();
    LOGV2_DEBUG(
        4910801, kDiagnosticLogLevel, "Started asynchronous request executor", "name"_attr = _name);
}

AsyncRequestExecutor::~AsyncRequestExecutor() {
    _pool->shutdown();
    _pool->join();
    LOGV2_DEBUG(
        4910802, kDiagnosticLogLevel, "Stopped asynchronous request executor", "name"_attr = _name);
}

Future<void> AsyncRequestExecutor::schedule(std::shared_ptr<RequestExecutionContext> rec) {
    auto opCtx = rec->getOpCtx();
    auto [promise, future] = makePromiseFuture<void>();

    // `this` remains valid as it owns the instance of thread pool.
    _pool->schedule([this,
                     strand = ClientStrand::get(opCtx->getClient()),
                     promise = std::move(promise),
                     rec = std::move(rec)](Status status) mutable {
        hangBeforeRunningAsyncRequestExecutorTask.pauseWhileSet();
        strand->run([&] {
            promise.setWith([&] {
                if (MONGO_unlikely(!status.isOK()))
                    return status.withContext("Unable to schedule asynchronous request");

                auto opCtx = rec->getOpCtx();
                if (opCtx->isKillPending())
                    return Status(opCtx->getKillStatus(), "Asynchronous operation was interrupted");

                return handleRequest(std::move(rec));
            });
        });
    });

    return std::move(future);
}

}  // namespace mongo
