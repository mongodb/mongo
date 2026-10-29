// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/primary_only_service_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/client.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <mutex>
#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {

namespace {

// Set the exponential delay after evaluating the condition 'until' within the 'run' method.
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

DefaultPrimaryOnlyServiceInstance::~DefaultPrimaryOnlyServiceInstance() {
    invariant(_completionPromise.getFuture().isReady());
}

SharedSemiFuture<void> DefaultPrimaryOnlyServiceInstance::getCompletionFuture() {
    return _completionPromise.getFuture();
}

void DefaultPrimaryOnlyServiceInstance::interrupt(Status status) noexcept {
    LOGV2_DEBUG(6663100,
                1,
                "DefaultPrimaryOnlyServiceInstance interrupted",
                "instance"_attr = getInstanceName(),
                "reason"_attr = redact(status));

    // Resolve any unresolved promises to avoid hanging.
    std::lock_guard<std::mutex> lg(_mutex);
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

SemiFuture<void> DefaultPrimaryOnlyServiceInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            return AsyncTry([this, executor, token] { return _runImpl(executor, token); })
                .until([this, token](Status status) { return status.isOK() || token.isCanceled(); })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, executor, token, anchor = shared_from_this()](const Status& status) {
            if (!status.isOK()) {
                LOGV2_ERROR(6663101,
                            "Error executing DefaultPrimaryOnlyServiceInstance",
                            "instance"_attr = getInstanceName(),
                            "error"_attr = redact(status));

                // Nothing else to do, the _completionPromise will be cancelled once the coordinator
                // is interrupted, because the only reasons to stop forward progress in this node is
                // because of a stepdown happened or the coordinator was canceled.
                dassert((token.isCanceled() && status.isA<ErrorCategory::CancellationError>()) ||
                        status.isA<ErrorCategory::NotPrimaryError>());
                return status;
            }

            try {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                _removeStateDocument(opCtx);
            } catch (DBException& ex) {
                LOGV2_WARNING(
                    6663103,
                    "Failed to remove state document from DefaultPrimaryOnlyServiceInstance",
                    "instance"_attr = getInstanceName(),
                    "error"_attr = redact(ex));
                ex.addContext(str::stream()
                              << "Failed to remove state document from " << getInstanceName());

                std::lock_guard<std::mutex> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(ex.toStatus());
                }

                throw;
            }

            std::lock_guard<std::mutex> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.emplaceValue();
            }

            return status;
        })
        .semi();
}

}  // namespace mongo
