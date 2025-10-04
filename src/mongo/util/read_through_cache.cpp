/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/read_through_cache.h"

#include "mongo/db/client.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/scopeguard.h"

#include <utility>

namespace mongo {

ReadThroughCacheBase::ReadThroughCacheBase(Service* service, ThreadPoolInterface& threadPool)
    : _service(service), _threadPool(threadPool) {}

ReadThroughCacheBase::~ReadThroughCacheBase() = default;

struct ReadThroughCacheBase::CancelToken::TaskInfo {
    TaskInfo(Service* service, stdx::mutex& cancelTokenMutex)
        : service(service), cancelTokenMutex(cancelTokenMutex) {}

    Service* const service;

    stdx::mutex& cancelTokenMutex;
    Status cancelStatus{Status::OK()};
    OperationContext* opCtxToCancel{nullptr};
};

ReadThroughCacheBase::CancelToken::CancelToken(std::shared_ptr<TaskInfo> info)
    : _info(std::move(info)) {}

ReadThroughCacheBase::CancelToken::CancelToken(CancelToken&&) = default;

ReadThroughCacheBase::CancelToken::~CancelToken() = default;

void ReadThroughCacheBase::CancelToken::tryCancel() {
    stdx::lock_guard lg(_info->cancelTokenMutex);
    _info->cancelStatus =
        Status(ErrorCodes::ReadThroughCacheLookupCanceled, "Internal only: task canceled");
    if (_info->opCtxToCancel) {
        ClientLock clientLock(_info->opCtxToCancel->getClient());
        _info->service->getServiceContext()->killOperation(
            clientLock, _info->opCtxToCancel, _info->cancelStatus.code());
    }
}

ReadThroughCacheBase::CancelToken ReadThroughCacheBase::_asyncWork(
    WorkWithOpContext work) noexcept {
    auto taskInfo = std::make_shared<CancelToken::TaskInfo>(_service, _cancelTokensMutex);

    _threadPool.schedule(
        [work = std::move(work), taskInfo](Status cancelStatusAtTaskBegin) mutable {
            if (!cancelStatusAtTaskBegin.isOK()) {
                work(nullptr, cancelStatusAtTaskBegin);
                return;
            }

            // TODO(SERVER-74659): Please revisit if this thread could be made killable.
            ThreadClient tc(taskInfo->service, ClientOperationKillableByStepdown{false});
            auto opCtxHolder = tc->makeOperationContext();

            cancelStatusAtTaskBegin = [&] {
                stdx::lock_guard lg(taskInfo->cancelTokenMutex);
                taskInfo->opCtxToCancel = opCtxHolder.get();
                return taskInfo->cancelStatus;
            }();

            ON_BLOCK_EXIT([&] {
                stdx::lock_guard lg(taskInfo->cancelTokenMutex);
                taskInfo->opCtxToCancel = nullptr;
            });

            work(taskInfo->opCtxToCancel, cancelStatusAtTaskBegin);
        });

    return CancelToken(std::move(taskInfo));
}

Date_t ReadThroughCacheBase::_now() {
    return _service->getServiceContext()->getFastClockSource()->now();
}

}  // namespace mongo
