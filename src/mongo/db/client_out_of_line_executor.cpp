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


#include "mongo/db/client_out_of_line_executor.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/timer.h"

#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

// Per-instance implementation details that need not appear in the header.
class ClientOutOfLineExecutor::Impl {
public:
    /** Returns Info(), then suppresses to `Debug(2)` for a second. */
    logv2::SeveritySuppressor bumpedSeverity{
        Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
    Timer timer;
};

ClientOutOfLineExecutor::ClientOutOfLineExecutor() noexcept
    : _impl{std::make_unique<Impl>()}, _taskQueue{std::make_shared<QueueType>()} {}

ClientOutOfLineExecutor::~ClientOutOfLineExecutor() {
    if (!_requireShutdown.load())
        return;
    invariant(_isShutdown);
}

void ClientOutOfLineExecutor::shutdown() {
    ON_BLOCK_EXIT([this]() mutable { _isShutdown = true; });

    // Force producers to consume their tasks beyond this point.
    _taskQueue->closeProducerEnd();

    // Only call `tryPop()` when there's a task to pop to avoid lifetime issues with logging.
    auto toCollect = _taskQueue->getStats().queueDepth;
    while (toCollect) {
        auto task = _taskQueue->tryPop();
        invariant(task);
        (*task)(Status(ErrorCodes::ClientDisconnect, "Client's executor has stopped"));
        toCollect--;
    }
    invariant(toCollect == 0);
}

static const Client::Decoration<ClientOutOfLineExecutor> getClientExecutor =
    Client::declareDecoration<ClientOutOfLineExecutor>();

ClientOutOfLineExecutor* ClientOutOfLineExecutor::get(const Client* client) noexcept {
    return const_cast<ClientOutOfLineExecutor*>(&getClientExecutor(client));
}

void ClientOutOfLineExecutor::schedule(Task task) {
    getHandle().schedule(std::move(task));
}

void ClientOutOfLineExecutor::consumeAllTasks() noexcept {
    // This limit allows logging incidents that the executor consumes too much of the client's
    // time running scheduled tasks. The value is only used for debugging, and should be an
    // approximation of the acceptable overhead in the context of normal client operations.
    static constexpr auto kTimeLimit = Microseconds(30);

    _impl->timer.reset();

    while (auto maybeTask = _taskQueue->tryPop()) {
        auto task = std::move(*maybeTask);
        task(Status::OK());
    }

    auto elapsed = _impl->timer.elapsed();

    if (MONGO_unlikely(elapsed > kTimeLimit)) {
        LOGV2_DEBUG(4651401,
                    _impl->bumpedSeverity().toInt(),
                    "Client's executor exceeded time limit",
                    "elapsed"_attr = elapsed,
                    "limit"_attr = kTimeLimit);
    }
}

void ClientOutOfLineExecutor::QueueHandle::schedule(Task&& task) {
    ScopeGuard guard(
        [&task] { task(Status(ErrorCodes::CallbackCanceled, "Client no longer exists")); });

    if (auto queue = _weakQueue.lock()) {
        try {
            queue->push(std::move(task));
            guard.dismiss();
        } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
            // The destructor for the out-of-line executor has already been called.
        }
    }
}

namespace {

/**
 * The observer ensures that `ClientOutOfLineExecutor` is always stopped before the client object is
 * destroyed. This is necessary to guarantee that `ClientOutOfLineExecutor::shutdown()` is executed
 * before the client decorations are destroyed. See SERVER-48901 for more details.
 */
class ClientOutOfLineExecutorClientObserver final : public ServiceContext::ClientObserver {
    void onCreateClient(Client*) override {}
    void onDestroyClient(Client* client) override {
        ClientOutOfLineExecutor::get(client)->shutdown();
    }
    void onCreateOperationContext(OperationContext*) override {}
    void onDestroyOperationContext(OperationContext*) override {}
};

ServiceContext::ConstructorActionRegisterer
    registerClientOutOfLineExecutorClientObserverConstructor{
        "ClientOutOfLineExecutorClientObserverConstructor", [](ServiceContext* service) {
            service->registerClientObserver(
                std::make_unique<ClientOutOfLineExecutorClientObserver>());
        }};

}  // namespace
}  // namespace mongo
