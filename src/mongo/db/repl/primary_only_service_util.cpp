/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
    stdx::lock_guard<stdx::mutex> lg(_mutex);
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

                stdx::lock_guard<stdx::mutex> lg(_mutex);
                if (!_completionPromise.getFuture().isReady()) {
                    _completionPromise.setError(ex.toStatus());
                }

                throw;
            }

            stdx::lock_guard<stdx::mutex> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.emplaceValue();
            }

            return status;
        })
        .semi();
}

}  // namespace mongo
