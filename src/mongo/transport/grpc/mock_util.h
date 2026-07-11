// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

#include <map>
#include <string>

#include <boost/optional.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>

namespace mongo::transport::grpc {

/**
 * Class containing the shared cancellation state between a MockServerStream and its corresponding
 * MockClientStream. This mocks cases in which an RPC is terminated before the server's RPC handler
 * is able to return a status (e.g. explicit client/server cancellation or a network error).
 */
class MockCancellationState {
public:
    explicit MockCancellationState(Date_t deadline) : _deadline(deadline) {}

    Date_t getDeadline() const {
        return _deadline;
    }

    bool isCancelled() const {
        return _cancellationStatus->has_value() || isDeadlineExceeded();
    }

    bool isDeadlineExceeded() const {
        return getGlobalServiceContext()->getFastClockSource()->now() > _deadline;
    }

    boost::optional<::grpc::Status> getCancellationStatus() {
        if (auto status = _cancellationStatus.synchronize(); status->has_value()) {
            return *status;
        } else if (isDeadlineExceeded()) {
            return ::grpc::Status(::grpc::StatusCode::DEADLINE_EXCEEDED, "Deadline exceeded");
        } else {
            return boost::none;
        }
    }

    void cancel(::grpc::Status status) {
        auto statusGuard = _cancellationStatus.synchronize();
        if (statusGuard->has_value()) {
            return;
        }
        *statusGuard = std::move(status);

        cancelSource.cancel();
    }

    SemiFuture<void> onCancel() {
        return cancelSource.token().onCancel();
    }

private:
    Date_t _deadline;
    synchronized_value<boost::optional<::grpc::Status>> _cancellationStatus;

    CancellationSource cancelSource;
};

/**
 * Performs the provided lambda and returns its return value. If the lambda's execution is
 * interrupted due to the deadline being exceeded, this returns a default-constructed T instead.
 */
template <typename T, typename F>
T runWithDeadline(Date_t deadline, F f) {
    auto svcCtx = getGlobalServiceContext();
    if (svcCtx->getFastClockSource()->now() > deadline) {
        return T();
    }

    auto cl = svcCtx->getService()->makeClient("MockGRPCTimer");
    AlternativeClientRegion acr(cl);
    auto opCtx = cc().makeOperationContext();
    opCtx->setDeadlineByDate(deadline, ErrorCodes::ExceededTimeLimit);

    try {
        return f(opCtx.get());
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        return T();
    }
}
}  // namespace mongo::transport::grpc
