// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class OperationContext;

/**
 * Wrapper class around an OperationContext that calls markKilled(ErrorCodes::Interrupted) when the
 * supplied CancellationToken is canceled.
 *
 * This class is useful for having an OperationContext be interrupted when a CancellationToken is
 * canceled. Note that OperationContext::getCancellationToken() is instead useful for having a
 * CancellationToken be canceled when an OperationContext is interrupted. The combination of the two
 * enables bridging between OperationContext interruption and CancellationToken cancellation
 * arbitrarily.
 *
 * IMPORTANT: Executors are allowed to refuse work. markKilled(ErrorCodes::Interrupted) won't be
 * called when the supplied CancellationToken is canceled if the task executor has already been shut
 * down, for example. Use a task executor bound to the process lifetime if you must guarantee that
 * the OperationContext is interrupted when the CancellationToken is canceled.
 */
class CancelableOperationContext {
public:
    CancelableOperationContext(ServiceContext::UniqueOperationContext opCtx,
                               const CancellationToken& cancelToken,
                               ExecutorPtr executor);

    CancelableOperationContext(const CancelableOperationContext&) = delete;
    CancelableOperationContext& operator=(const CancelableOperationContext&) = delete;

    CancelableOperationContext(CancelableOperationContext&&) = delete;
    CancelableOperationContext& operator=(CancelableOperationContext&&) = delete;

    ~CancelableOperationContext();

    OperationContext* get() const noexcept {
        return _opCtx.get();
    }

    OperationContext* operator->() const noexcept {
        return get();
    }

    OperationContext& operator*() const noexcept {
        return *get();
    }

private:
    struct SharedBlock {
        Atomic<bool> done{false};
    };

    const std::shared_ptr<SharedBlock> _sharedBlock;
    const ServiceContext::UniqueOperationContext _opCtx;
    const SemiFuture<void> _markKilledFinished;
};

/**
 * A factory to create CancelableOperationContext objects that use the same CancelationToken and
 * executor.
 */
class CancelableOperationContextFactory {
public:
    CancelableOperationContextFactory(CancellationToken cancelToken, ExecutorPtr executor)
        : _cancelToken{std::move(cancelToken)}, _executor{std::move(executor)} {}

    CancelableOperationContext makeOperationContext(Client* client) const {
        return CancelableOperationContext{client->makeOperationContext(), _cancelToken, _executor};
    }

private:
    const CancellationToken _cancelToken;
    const ExecutorPtr _executor;
};

}  // namespace mongo
