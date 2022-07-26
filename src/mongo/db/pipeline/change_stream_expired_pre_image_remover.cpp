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


#include "mongo/platform/basic.h"

#include "change_stream_expired_pre_image_remover.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/idle_thread_block.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {
class ChangeStreamExpiredPreImagesRemover;

const auto getChangeStreamExpiredPreImagesRemover =
    ServiceContext::declareDecoration<std::unique_ptr<ChangeStreamExpiredPreImagesRemover>>();

/**
 * A periodic background job that removes expired change stream pre-image documents from the
 * 'system.preimages' collection. The period of the job is controlled by the server parameter
 * "expiredChangeStreamPreImageRemovalJobSleepSecs".
 */
class ChangeStreamExpiredPreImagesRemover : public BackgroundJob {
public:
    explicit ChangeStreamExpiredPreImagesRemover() : BackgroundJob(false /* selfDelete */) {}

    /**
     * Retrieves ChangeStreamExpiredPreImagesRemover from the service context 'serviceCtx'.
     */
    static ChangeStreamExpiredPreImagesRemover* get(ServiceContext* serviceCtx) {
        return getChangeStreamExpiredPreImagesRemover(serviceCtx).get();
    }

    /**
     * Sets ChangeStreamExpiredPreImagesRemover 'preImagesRemover' to the service context
     * 'serviceCtx'.
     */
    static void set(ServiceContext* serviceCtx,
                    std::unique_ptr<ChangeStreamExpiredPreImagesRemover> preImagesRemover) {
        auto& changeStreamExpiredPreImagesRemover =
            getChangeStreamExpiredPreImagesRemover(serviceCtx);
        if (changeStreamExpiredPreImagesRemover) {
            invariant(!changeStreamExpiredPreImagesRemover->running(),
                      "Tried to reset the ChangeStreamExpiredPreImagesRemover without shutting "
                      "down the original instance.");
        }

        invariant(preImagesRemover);
        changeStreamExpiredPreImagesRemover = std::move(preImagesRemover);
    }

    std::string name() const {
        return "ChangeStreamExpiredPreImagesRemover";
    }

    void run() {
        ThreadClient tc(name(), getGlobalServiceContext());
        AuthorizationSession::get(cc())->grantInternalAuthorization(&cc());

        {
            stdx::lock_guard<Client> lk(*tc.get());
            tc.get()->setSystemOperationKillableByStepdown(lk);
        }

        while (true) {
            LOGV2_DEBUG(6278517, 3, "Thread awake");
            auto iterationStartTime = Date_t::now();
            ChangeStreamPreImagesCollectionManager::performExpiredChangeStreamPreImagesRemovalPass(
                tc.get());
            {
                // Wait until either gExpiredChangeStreamPreImageRemovalJobSleepSecs passes or a
                // shutdown is requested.
                auto deadline = iterationStartTime +
                    Seconds(gExpiredChangeStreamPreImageRemovalJobSleepSecs.load());
                stdx::unique_lock<Latch> lk(_stateMutex);

                MONGO_IDLE_THREAD_BLOCK;
                _shuttingDownCV.wait_until(
                    lk, deadline.toSystemTimePoint(), [&] { return _shuttingDown; });

                if (_shuttingDown) {
                    return;
                }
            }
        }
    }

    /**
     * Signals the thread to quit and then waits until it does.
     */
    void shutdown() {
        LOGV2(6278515, "Shutting down Change Stream Expired Pre-images Remover thread");
        {
            stdx::lock_guard<Latch> lk(_stateMutex);
            _shuttingDown = true;
        }
        _shuttingDownCV.notify_one();
        wait();
        LOGV2(6278516, "Finished shutting down Change Stream Expired Pre-images Remover thread");
    }

private:
    // Protects the state below.
    mutable Mutex _stateMutex = MONGO_MAKE_LATCH("ChangeStreamExpiredPreImagesRemoverStateMutex");

    // Signaled to wake up the thread, if the thread is waiting. The thread will check whether
    // _shuttingDown is set and stop accordingly.
    mutable stdx::condition_variable _shuttingDownCV;

    bool _shuttingDown = false;
};
}  // namespace

void startChangeStreamExpiredPreImagesRemover(ServiceContext* serviceContext) {
    std::unique_ptr<ChangeStreamExpiredPreImagesRemover> changeStreamExpiredPreImagesRemover =
        std::make_unique<ChangeStreamExpiredPreImagesRemover>();
    changeStreamExpiredPreImagesRemover->go();
    ChangeStreamExpiredPreImagesRemover::set(serviceContext,
                                             std::move(changeStreamExpiredPreImagesRemover));
}

void shutdownChangeStreamExpiredPreImagesRemover(ServiceContext* serviceContext) {
    ChangeStreamExpiredPreImagesRemover* changeStreamExpiredPreImagesRemover =
        ChangeStreamExpiredPreImagesRemover::get(serviceContext);
    // We allow the ChangeStreamExpiredPreImagesRemover not to be set in case shutdown occurs before
    // the thread has been initialized.
    if (changeStreamExpiredPreImagesRemover) {
        changeStreamExpiredPreImagesRemover->shutdown();
    }
}
}  // namespace mongo
