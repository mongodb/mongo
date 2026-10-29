// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/process_health/health_check_status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

namespace mongo {

namespace process_health {
/**
 * A deadline future wraps an inputFuture and returns an outputFuture where:
 * - the outputFuture is set to the result of the inputFuture if the inputFuture finishes within
 * timeout.
 * - otherwise the outputFuture returns an error.
 */
template <typename ResultStatus>
class DeadlineFuture : public std::enable_shared_from_this<DeadlineFuture<ResultStatus>> {
public:
    static std::shared_ptr<DeadlineFuture<ResultStatus>> create(
        std::shared_ptr<executor::TaskExecutor> executor,
        Future<ResultStatus> inputFuture,
        Milliseconds timeout) {
        auto instance = std::shared_ptr<DeadlineFuture<ResultStatus>>(
            new DeadlineFuture<ResultStatus>(executor));
        instance->init(instance, std::move(inputFuture), timeout);
        return instance;
    }

    ~DeadlineFuture() {
        {
            auto lk = std::lock_guard(_mutex);
            if (_timeoutCbHandle) {
                _executor->cancel(_timeoutCbHandle.get());
            }
            // The _executor holds the shared ptr on this, the callback will set the promise.
            invariant(get().isReady());
        }
    }

    SharedSemiFuture<ResultStatus> get() const {
        return _outputFuturePromise->getFuture();
    }

private:
    DeadlineFuture(std::shared_ptr<executor::TaskExecutor> executor) : _executor(executor) {}

    void init(std::shared_ptr<DeadlineFuture<ResultStatus>> self,
              Future<ResultStatus> inputFuture,
              Milliseconds timeout) {
        _outputFuturePromise = std::make_unique<SharedPromise<ResultStatus>>();

        auto swCbHandle = _executor->scheduleWorkAt(
            _executor->now() + timeout,
            [this, self](const executor::TaskExecutor::CallbackArgs& cbData) {
                auto lk = std::lock_guard(_mutex);
                if (!cbData.status.isOK()) {
                    if (!get().isReady()) {
                        _outputFuturePromise->setError(cbData.status);
                    }
                    return;
                }

                if (!get().isReady()) {
                    _outputFuturePromise->setError(
                        Status(ErrorCodes::ExceededTimeLimit, "Deadline future timed out"));
                }
            });

        {
            auto lk = std::lock_guard(_mutex);
            if (!swCbHandle.isOK() && !get().isReady()) {
                _outputFuturePromise->setError(swCbHandle.getStatus());
                return;
            }
        }

        _timeoutCbHandle = swCbHandle.getValue();

        _inputFuture =
            std::move(inputFuture).onCompletion([this, self](StatusWith<ResultStatus> status) {
                auto lk = std::lock_guard(_mutex);
                _executor->cancel(_timeoutCbHandle.get());
                _timeoutCbHandle = boost::none;
                if (!get().isReady()) {
                    _outputFuturePromise->setFrom(status);
                }
                return status;
            });
    }

private:
    const std::shared_ptr<executor::TaskExecutor> _executor;

    mutable std::mutex _mutex;
    Future<ResultStatus> _inputFuture;
    boost::optional<executor::TaskExecutor::CallbackHandle> _timeoutCbHandle;
    std::unique_ptr<SharedPromise<ResultStatus>> _outputFuturePromise;
};
}  // namespace process_health
}  // namespace mongo
