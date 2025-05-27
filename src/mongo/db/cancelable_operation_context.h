/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/out_of_line_executor.h"

#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>

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
        AtomicWord<bool> done{false};
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
