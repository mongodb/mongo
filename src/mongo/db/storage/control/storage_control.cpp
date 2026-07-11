// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/control/storage_control.h"

#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo::StorageControl {

namespace {

bool areControlsStarted = false;
bool journalFlusherPaused = false;

}  // namespace

void startStorageControls(ServiceContext* serviceContext,
                          bool forTestOnly,
                          std::unique_ptr<CheckpointSchedulePolicy> policy) {
    auto storageEngine = serviceContext->getStorageEngine();
    invariant(!areControlsStarted);

    // Instantiate a thread to periodically, and upon request, flush writes to disk.
    //
    // Persisted storage engines that have a journal should periodically flush the journal to disk
    // to avoid risking much user data loss across a server crash if the user is not doing {j: true}
    // writes often.
    //
    // Non-durable, i.e. no journal, storage engines should only flush upon request because
    // waitUntilDurable() will perform a checkpoint and checkpoints are costly. Periodic flushes
    // will be disabled and only requests will provoke a flush.
    //
    // Ephemeral engines are not durable -- waitUntilDurable() returns early -- but frequent updates
    // to replication's JournalListener in the waitUntilDurable() code may help update replication
    // timestamps more quickly.
    if (journalFlusherPaused) {
        // This is a restart and the JournalListener was paused. Resume the existing JournalFlusher.
        JournalFlusher::get(serviceContext)->resume();
        journalFlusherPaused = false;
    } else {
        std::unique_ptr<JournalFlusher> journalFlusher = std::make_unique<JournalFlusher>(
            /*disablePeriodicFlushes*/ forTestOnly);
        JournalFlusher::set(serviceContext, std::move(journalFlusher));
        JournalFlusher::get(serviceContext)->go();
    }

    if (storageEngine->supportsCheckpoints() && !storageEngine->isEphemeral() &&
        !storageGlobalParams.queryableBackupMode) {
        auto checkpointer = std::make_unique<Checkpointer>(policy ? std::move(policy)
                                                                  : createFixedIntervalPolicy());
        checkpointer->go();
        Checkpointer::set(serviceContext, std::move(checkpointer));
    }

    areControlsStarted = true;
}

void stopStorageControls(ServiceContext* serviceContext, const Status& reason, bool forRestart) {
    if (areControlsStarted) {
        if (forRestart) {
            // Pausing instead of shutting down the journal flusher for restart.
            JournalFlusher::get(serviceContext)->pause();
            journalFlusherPaused = true;
        } else {
            JournalFlusher::get(serviceContext)->shutdown(reason);
        }

        auto checkpointer = Checkpointer::get(serviceContext);
        if (checkpointer) {
            checkpointer->shutdown(reason);
        }

        areControlsStarted = false;
    } else {
        // The JournalFlusher was not resumed after being paused with forRestart.
        invariant(!journalFlusherPaused);
        // Cannot stop storage controls for restart if the controls were not started or were already
        // stopped.
        invariant(!forRestart);
    }
}
}  // namespace mongo::StorageControl
