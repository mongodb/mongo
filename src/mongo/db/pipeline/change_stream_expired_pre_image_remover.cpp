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


#include "mongo/db/pipeline/change_stream_expired_pre_image_remover.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/change_stream_pre_image_util.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
const auto changeStreamExpiredPreImagesRemoverServiceDecorator =
    ServiceContext::declareDecoration<ChangeStreamExpiredPreImagesRemoverService>();
}  // namespace

const ReplicaSetAwareServiceRegistry::Registerer<ChangeStreamExpiredPreImagesRemoverService>
    changeStreamExpiredPreImagesRemoverServiceJobRegistryRegisterer(
        "ChangeStreamExpiredPreImagesRemoverService");


ChangeStreamExpiredPreImagesRemoverService* ChangeStreamExpiredPreImagesRemoverService::get(
    ServiceContext* serviceContext) {
    return &changeStreamExpiredPreImagesRemoverServiceDecorator(serviceContext);
}

ChangeStreamExpiredPreImagesRemoverService* ChangeStreamExpiredPreImagesRemoverService::get(
    OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ChangeStreamExpiredPreImagesRemoverService::onConsistentDataAvailable(OperationContext* opCtx,
                                                                           bool isMajority,
                                                                           bool isRollback) {
    // 'onConsistentDataAvailable()' is signaled both on initial startup and after rollbacks.
    // 'isRollback: false' signals data is consistent on initial startup. If the pre-image removal
    // job is to run, it should do so once data is consistent on startup.
    if (isRollback) {
        return;
    }

    // Check if replicated truncates are enabled right now.
    const bool useReplicatedTruncates =
        change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx);

    std::lock_guard<std::mutex> lk(_mutex);

    if (useReplicatedTruncates) {
        // If replicated truncates are enabled, the pre-images removal job is only executed on the
        // primary. It therefore needs to be started on every step-up, but not here.
        // If the removal job is still running because it was installed by a previous
        // 'onConsistentDataAvailable()' call, stop it now.
        if (_periodicJob.has_value() && !_periodicJob->context.usesReplicatedTruncates) {
            _stopChangeStreamExpiredPreImagesRemoverServiceJob(lk);
        }
    } else {
        _startChangeStreamExpiredPreImagesRemoverServiceJob(
            lk, opCtx, false /* useReplicatedTruncates */);
    }
}

void ChangeStreamExpiredPreImagesRemoverService::onStepUpComplete(OperationContext* opCtx,
                                                                  long long term) {
    // Check if replicated truncates are enabled right now.
    const bool useReplicatedTruncates =
        change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx);

    // If replicated truncates are enabled, the pre-images removal job is only executed on the
    // primary. Therefore it needs to be (re-)started on every step-up.
    // If replicated truncates are disabled, the pre-images removal job is executed locally and
    // independently on each node. It then does not need to be started here, but instead it is
    // started in in 'onConsistentDataAvailable()'.
    std::lock_guard<std::mutex> lk(_mutex);
    _isPrimary = true;
    if (useReplicatedTruncates) {
        _startChangeStreamExpiredPreImagesRemoverServiceJob(
            lk, opCtx, true /* useReplicatedTruncates */);
    }
}

void ChangeStreamExpiredPreImagesRemoverService::onStepDown() {
    // If replicated truncates are enabled, only the primary is allowed to execute the pre-images
    // removal job. Secondaries do not run the job. The job must therefore be stopped on step-down
    // when the node becomes a secondary.
    //
    // If replicated truncates are not enabled, the pre-images removal job is run locally and
    // independently on each node. It then does not need to be stopped on step-downs.

    // 'onStepDown()' can be called without a prior call to 'onStepUpComplete()', so we cannot
    // assume that '_useReplicatedTruncates' was already populated.
    std::lock_guard<std::mutex> lk(_mutex);
    _isPrimary = false;
    if (_periodicJob.has_value() && _periodicJob->context.usesReplicatedTruncates) {
        // If the removal job was installed while replicated truncates were still enabled, stop it.
        _stopChangeStreamExpiredPreImagesRemoverServiceJob(lk);
    }
}

void ChangeStreamExpiredPreImagesRemoverService::onShutdown() {
    // Unconditionally stop the pre-images removal job.
    std::lock_guard<std::mutex> lk(_mutex);
    _isPrimary = false;
    _stopChangeStreamExpiredPreImagesRemoverServiceJob(lk);
}

void ChangeStreamExpiredPreImagesRemoverService::onFCVChange(
    OperationContext* opCtx, const ServerGlobalParams::FCVSnapshot& newFcvSnapshot) {
    const bool useReplicatedTruncates =
        change_stream_pre_image_util::shouldUseReplicatedTruncatesForPreImages(opCtx,
                                                                               newFcvSnapshot);

    std::lock_guard<std::mutex> lk(_mutex);
    if (_periodicJob.has_value() &&
        _periodicJob->context.usesReplicatedTruncates != useReplicatedTruncates) {
        // The job is currently running, but the value of the feature flag has changed.
        if (useReplicatedTruncates && !_isPrimary) {
            // When using replicated truncates, only the primary should execute the removal job.
            // Stop it everywhere else.
            _stopChangeStreamExpiredPreImagesRemoverServiceJob(lk);
        } else {
            // When not using replicated truncates or when we are the primary, start the removal
            // job.
            _startChangeStreamExpiredPreImagesRemoverServiceJob(lk, opCtx, useReplicatedTruncates);
        }
    } else if (!_periodicJob.has_value()) {
        // Job is currently not running.
        if (!useReplicatedTruncates || _isPrimary) {
            // Start job.
            _startChangeStreamExpiredPreImagesRemoverServiceJob(lk, opCtx, useReplicatedTruncates);
        }
    }
}

boost::optional<ChangeStreamExpiredPreImagesRemoverService::PreImagesRemovalJobContext>
ChangeStreamExpiredPreImagesRemoverService::getJobContext_forTest() const {
    boost::optional<PreImagesRemovalJobContext> result;

    std::lock_guard<std::mutex> lk(_mutex);
    if (_periodicJob.has_value()) {
        result.emplace(_periodicJob->context);
    }
    return result;
}

void ChangeStreamExpiredPreImagesRemoverService::
    _startChangeStreamExpiredPreImagesRemoverServiceJob(WithLock lk,
                                                        OperationContext* opCtx,
                                                        bool useReplicatedTruncates) {
    if (gPreImageRemoverDisabled ||
        !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        // The removal job is disabled or should not run because it is a standalone.
        return;
    }

    if (_periodicJob.has_value() && !_periodicJob->context.usesReplicatedTruncates &&
        !useReplicatedTruncates) {
        // Job already running without using replicated truncates. No need to flush the state and
        // cancel the previous instance of the job.
        return;
    }

    // Stop any potential running instance of the job before restarting it.
    _stopChangeStreamExpiredPreImagesRemoverServiceJob(lk);

    tassert(12047104, "expecting no periodic job to be present", !_periodicJob.has_value());

    // Flush in-memory state of truncate markers, so they will be rebuilt the next time the removal
    // job executes.
    ChangeStreamPreImagesCollectionManager::get(opCtx).flushTruncateMarkers();

    tassert(12047101,
            "expecting no periodic pre-images removal job to be installed",
            !_periodicJob.has_value());

    auto periodicRunner = opCtx->getServiceContext()->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob changeStreamExpiredPreImagesRemoverServiceJob(
        "ChangeStreamExpiredPreImagesRemover",
        [useReplicatedTruncates](Client* client) {
            AuthorizationSession::get(client)->grantInternalAuthorization();
            ChangeStreamPreImagesCollectionManager::get(client->getServiceContext())
                .performExpiredChangeStreamPreImagesRemovalPass(client, useReplicatedTruncates);
        },
        Seconds(gExpiredChangeStreamPreImageRemovalJobSleepSecs.load()),
        true /*isKillableByStepdown*/);

    LOGV2(7080100,
          "Starting Change Stream Expired Pre-images Remover thread",
          "useReplicatedTruncates"_attr = useReplicatedTruncates);
    auto job = periodicRunner->makeJob(std::move(changeStreamExpiredPreImagesRemoverServiceJob));

    job.start();
    _periodicJob.emplace(
        std::move(job),
        PreImagesRemovalJobContext{.id = _nextJobId++,
                                   .usesReplicatedTruncates = useReplicatedTruncates});
}

void ChangeStreamExpiredPreImagesRemoverService::_stopChangeStreamExpiredPreImagesRemoverServiceJob(
    WithLock) {
    if (!_periodicJob.has_value()) {
        // No job currently running.
        return;
    }

    tassert(12047103, "expecting periodic job anchor to be valid", _periodicJob->job.isValid());

    LOGV2(6278511, "Shutting down the Change Stream Expired Pre-images Remover");

    // Reset the entire struct for the periodic job. This will stop the job before.
    _periodicJob.reset();
}

}  // namespace mongo
