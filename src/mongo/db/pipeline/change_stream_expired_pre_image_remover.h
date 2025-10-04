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

#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

/**
 * Manages the conditions under which periodic pre-image removal runs on a node.
 */
class ChangeStreamExpiredPreImagesRemoverService
    : public ReplicaSetAwareService<ChangeStreamExpiredPreImagesRemoverService> {
public:
    ChangeStreamExpiredPreImagesRemoverService() = default;

    /**
     * Obtains the service-wide instance.
     */
    static ChangeStreamExpiredPreImagesRemoverService* get(ServiceContext* serviceContext);
    static ChangeStreamExpiredPreImagesRemoverService* get(OperationContext* opCtx);

    void onStartup(OperationContext* opCtx) override {}

    void onSetCurrentConfig(OperationContext* opCtx) override {}

    /**
     * Controls when/if the periodic pre-image removal job starts up.
     */
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) override;

    void onStepUpBegin(OperationContext* opCtx, long long term) override {}

    void onStepUpComplete(OperationContext* opCtx, long long term) override {}

    void onStepDown() override {}

    void onRollbackBegin() override {}

    void onBecomeArbiter() override {}

    void onShutdown() override;

    inline std::string getServiceName() const final {
        return "ChangeStreamExpiredPreImagesRemoverService";
    }

    bool startedPeriodicJob_forTest() {
        stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
        return _periodicJob.isValid();
    }

private:
    stdx::mutex _mutex;
    PeriodicJobAnchor _periodicJob;
};
}  // namespace mongo
