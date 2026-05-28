/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {

class ShardCatalogHistoryCleanupControl;

class MONGO_MOD_PARENT_PRIVATE ShardCatalogHistoryCleanupRunGuard {
public:
    ShardCatalogHistoryCleanupRunGuard() = default;
    explicit ShardCatalogHistoryCleanupRunGuard(ShardCatalogHistoryCleanupControl* ctrl)
        : _ctrl(ctrl) {}
    ~ShardCatalogHistoryCleanupRunGuard();
    explicit operator bool() const {
        return _ctrl != nullptr;
    }

private:
    ShardCatalogHistoryCleanupControl* _ctrl = nullptr;
};

/**
 * The class controls execution flow of the shard catalog cleanup task, available
 * through decoration. It allows to stop and resume the cleanup task.
 */
class MONGO_MOD_PARENT_PRIVATE ShardCatalogHistoryCleanupControl {
public:
    static ShardCatalogHistoryCleanupControl& get(ServiceContext* serviceContext);
    static ShardCatalogHistoryCleanupControl& get(OperationContext* opCtx);

    void pause();
    void resume();
    ShardCatalogHistoryCleanupRunGuard tryAcquireRun();

private:
    friend class ShardCatalogHistoryCleanupRunGuard;
    void _releaseRun();

    std::mutex _mutex;
    stdx::condition_variable _cv;
    bool _paused{false};
    bool _running{false};
};

}  // namespace mongo
