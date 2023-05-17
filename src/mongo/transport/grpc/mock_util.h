/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include <map>
#include <string>

#include <boost/optional.hpp>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"

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
    }

private:
    Date_t _deadline;
    synchronized_value<boost::optional<::grpc::Status>> _cancellationStatus;
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

    auto cl = svcCtx->makeClient("MockGRPCTimer");
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
