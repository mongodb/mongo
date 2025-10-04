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


// IWYU pragma: no_include "cxxabi.h"
#include "change_stream_expired_pre_image_remover.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

#include <memory>
#include <mutex>
#include <string>
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

    if (gPreImageRemoverDisabled ||
        !repl::ReplicationCoordinator::get(opCtx)->getSettings().isReplSet()) {
        // The removal job is disabled or should not run because it is a standalone.
        return;
    }

    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (!_periodicJob.isValid()) {
        auto periodicRunner = opCtx->getServiceContext()->getPeriodicRunner();
        invariant(periodicRunner);

        PeriodicRunner::PeriodicJob changeStreamExpiredPreImagesRemoverServiceJob(
            "ChangeStreamExpiredPreImagesRemover",
            [](Client* client) {
                AuthorizationSession::get(client)->grantInternalAuthorization();
                ChangeStreamPreImagesCollectionManager::get(client->getServiceContext())
                    .performExpiredChangeStreamPreImagesRemovalPass(client);
            },
            Seconds(gExpiredChangeStreamPreImageRemovalJobSleepSecs.load()),
            true /*isKillableByStepdown*/);

        _periodicJob =
            periodicRunner->makeJob(std::move(changeStreamExpiredPreImagesRemoverServiceJob));
        LOGV2(7080100, "Starting Change Stream Expired Pre-images Remover thread");
        _periodicJob.start();
    }
}

void ChangeStreamExpiredPreImagesRemoverService::onShutdown() {
    stdx::lock_guard<stdx::mutex> scopedLock(_mutex);
    if (_periodicJob.isValid()) {
        LOGV2(6278511, "Shutting down the Change Stream Expired Pre-images Remover");
        _periodicJob.stop();
    }
}
}  // namespace mongo
