/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

class TenantMigrationAccessBlockerExecutor {
public:
    TenantMigrationAccessBlockerExecutor() = default;
    static const ServiceContext::Decoration<TenantMigrationAccessBlockerExecutor> get;

    TenantMigrationAccessBlockerExecutor(const TenantMigrationAccessBlockerExecutor&) = delete;
    TenantMigrationAccessBlockerExecutor& operator=(const TenantMigrationAccessBlockerExecutor&) =
        delete;

    // Executor to schedule asynchronous read and write operations while the
    // tenant migration access blocker is in action. This provides migrated tenants isolation
    // from the non-migrated users. The executor is shared by all access blockers and is
    // to be destroyed when no access blockers exist.
    // If this to be migrated to a global executor PM-1809 additional protection from a
    // thundering herd of simultaneously unblocked operations is required.
    std::shared_ptr<executor::TaskExecutor> getOrCreateBlockedOperationsExecutor();

private:
    mutable Mutex _mutex;
    // See getOrCreateBlockedOperationsExecutor().
    std::weak_ptr<executor::TaskExecutor> _blockedOperationsExecutor;
};

}  // namespace mongo
