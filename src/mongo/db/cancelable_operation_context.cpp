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
                      stdx::lock_guard<Client> lk(*opCtx->getClient());
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
