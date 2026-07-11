// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/cancelable_operation_context.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

#include <mutex>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

CancelableOperationContext::CancelableOperationContext(ServiceContext::UniqueOperationContext opCtx,
                                                       const CancellationToken& cancelToken,
                                                       ExecutorPtr executor)
    : _sharedBlock{std::make_shared<SharedBlock>()},
      _opCtx{std::move(opCtx)},
      _markKilledFinished{[&] {
          if (cancelToken.isCanceled()) {
              // This thread owns _opCtx so it isn't necessary to acquire the Client mutex.
              _opCtx->markKilled(ErrorCodes::Interrupted);
              return makeReadyFutureWith([] {}).semi();
          }

          return cancelToken.onCancel()
              .thenRunOn(std::move(executor))
              .then([sharedBlock = _sharedBlock, opCtx = _opCtx.get()] {
                  if (!sharedBlock->done.swap(true)) {
                      std::lock_guard<Client> lk(*opCtx->getClient());
                      opCtx->markKilled(ErrorCodes::Interrupted);
                  }
              })
              .semi();
      }()} {}

CancelableOperationContext::~CancelableOperationContext() {
    if (_sharedBlock->done.swap(true)) {
        // _sharedBlock->done was already true so our onCancel() continuation must have started to
        // run. We must wait for it to finish running to avoid markKilled() being called while the
        // OperationContext is being destructed.
        _markKilledFinished.wait();
    }
}

}  // namespace mongo
