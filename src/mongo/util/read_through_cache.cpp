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

#include "mongo/platform/basic.h"

#include "mongo/util/read_through_cache.h"

#include "mongo/stdx/condition_variable.h"

namespace mongo {

ReadThroughCacheBase::ReadThroughCacheBase(Mutex& mutex,
                                           ServiceContext* service,
                                           ThreadPoolInterface& threadPool)
    : _serviceContext(service), _threadPool(threadPool), _mutex(mutex) {}

ReadThroughCacheBase::~ReadThroughCacheBase() = default;

struct ReadThroughCacheBase::CancelToken::TaskInfo {
    TaskInfo(ServiceContext* service, Mutex& mutex) : service(service), mutex(mutex) {}

    ServiceContext* const service;

    Mutex& mutex;
    Status cancelStatus{Status::OK()};
    OperationContext* opCtxToCancel{nullptr};
};

ReadThroughCacheBase::CancelToken::CancelToken(std::shared_ptr<TaskInfo> info)
    : _info(std::move(info)) {}

ReadThroughCacheBase::CancelToken::CancelToken(CancelToken&&) = default;

ReadThroughCacheBase::CancelToken::~CancelToken() = default;

void ReadThroughCacheBase::CancelToken::tryCancel() {
    stdx::lock_guard lg(_info->mutex);
    _info->cancelStatus =
        Status(ErrorCodes::ReadThroughCacheLookupCanceled, "Internal only: task canceled");
    if (_info->opCtxToCancel) {
        stdx::lock_guard clientLock(*_info->opCtxToCancel->getClient());
        _info->service->killOperation(clientLock, _info->opCtxToCancel, _info->cancelStatus.code());
    }
}

ReadThroughCacheBase::CancelToken ReadThroughCacheBase::_asyncWork(
    WorkWithOpContext work) noexcept {
    auto taskInfo = std::make_shared<CancelToken::TaskInfo>(_serviceContext, _cancelTokenMutex);

    _threadPool.schedule([work = std::move(work), taskInfo](Status status) mutable {
        if (!status.isOK()) {
            work(nullptr, status);
            return;
        }

        ThreadClient tc(taskInfo->service);
        auto opCtxHolder = tc->makeOperationContext();

        const auto cancelStatusAtTaskBegin = [&] {
            stdx::lock_guard lg(taskInfo->mutex);
            taskInfo->opCtxToCancel = opCtxHolder.get();
            return taskInfo->cancelStatus;
        }();

        ON_BLOCK_EXIT([&] {
            stdx::lock_guard lg(taskInfo->mutex);
            taskInfo->opCtxToCancel = nullptr;
        });

        work(taskInfo->opCtxToCancel, cancelStatusAtTaskBegin);
    });

    return CancelToken(std::move(taskInfo));
}

Date_t ReadThroughCacheBase::_now() {
    return _serviceContext->getFastClockSource()->now();
}

}  // namespace mongo
