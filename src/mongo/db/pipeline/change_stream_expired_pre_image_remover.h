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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <mutex>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Manages the conditions under which periodic pre-image removal runs on a node.
 */
class MONGO_MOD_PUBLIC ChangeStreamExpiredPreImagesRemoverService
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
     * Starts the pre-images removal job in case replicated truncates are disabled. Does nothing in
     * case replicated truncates are enabled.
     */
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) override;

    void onStepUpBegin(OperationContext* opCtx, long long term) override {}

    /**
     * Starts the pre-images removal job in case replicated truncates are enabled. Does nothing in
     * case replicated truncates are disabled.
     */
    void onStepUpComplete(OperationContext* opCtx, long long term) override;

    /**
     * Stops the pre-images removal job in case replicated truncates are enabled. Does nothing in
     * case replicated truncates are disabled.
     */
    void onStepDown() override;

    void onRollbackBegin() override {}

    void onBecomeArbiter() override {}

    /**
     * Stops the pre-images removal job regardless of whether replicated truncates are used.
     */
    void onShutdown() override;

    /**
     * Can be called during FCV upgrades or downgrades. Stops/starts the periodic job as needed
     * given the new FCV.
     */
    void onFCVChange(OperationContext* opCtx,
                     const ServerGlobalParams::FCVSnapshot& newFcvSnapshot);

    std::string getServiceName() const final {
        return "ChangeStreamExpiredPreImagesRemoverService";
    }

    /**
     * Returns the context of the periodic job, if currently running.
     */
    struct PreImagesRemovalJobContext {
        /**
         * Internal id of the job. Every job instance gets a different id.
         */
        int64_t id = 0;

        /**
         * Whether or not the job uses replicated truncates. Populated exactly once during job
         * creation, and constant afterwards.
         */
        bool usesReplicatedTruncates = false;

        bool operator==(const PreImagesRemovalJobContext& other) const {
            return id == other.id && usesReplicatedTruncates == other.usesReplicatedTruncates;
        }
    };
    boost::optional<PreImagesRemovalJobContext> getJobContext_forTest() const;

private:
    /**
     * Starts the pre-images removal job. Must be called while holding the mutex '_mutex'.
     * If a previous instance of the pre-images removal job is still running, it will be stopped
     * before starting a new instance of the job.
     */
    void _startChangeStreamExpiredPreImagesRemoverServiceJob(WithLock lk,
                                                             OperationContext* opCtx,
                                                             bool useReplicatedTruncates);

    /**
     * Stops the pre-images removal job if it is running. Must be called while holding the mutex
     * '_mutex'. It is not an error to call this method if the job is not currently running.
     */
    void _stopChangeStreamExpiredPreImagesRemoverServiceJob(WithLock);

    struct PeriodicJobState {
        /**
         * Job for periodic pre-images removal.
         */
        PeriodicJobAnchor job;

        /**
         * Job context, containing metadata about the job's id and the job's usage of replicated
         * truncates.
         */
        PreImagesRemovalJobContext context;
    };

    /**
     * Protects '_periodicJob', '_nextJobId' and '_isPrimary'.
     */
    mutable std::mutex _mutex;

    /**
     * State of the currently running periodic job, if active. Otherwise empty.
     */
    boost::optional<PeriodicJobState> _periodicJob;

    /**
     * Id that will be assigned to the next instance of the periodic job. Incremented by one for
     * every new job instance created. Uses mainly for testing to tell different instances of the
     * job apart from each other.
     */
    int64_t _nextJobId = 1;

    /**
     * Flag indicating if the node is the current primary of the replica set. Updated on every call
     * to 'onStepUpComplete()' and 'onStepDown()', and cleared in 'onShutdown()'.
     */
    bool _isPrimary = false;
};
}  // namespace mongo
