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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/control/storage_control.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/checkpointer.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace StorageControl {

namespace {

bool areControlsStarted = false;
bool journalFlusherPaused = false;

}  // namespace

void startStorageControls(ServiceContext* serviceContext, bool forTestOnly) {
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
        std::unique_ptr<Checkpointer> checkpointer = std::make_unique<Checkpointer>();
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

void waitForJournalFlush(OperationContext* opCtx) {
    JournalFlusher::get(opCtx)->waitForJournalFlush();
}

void interruptJournalFlusherForReplStateChange(ServiceContext* serviceContext) {
    JournalFlusher::get(serviceContext)->interruptJournalFlusherForReplStateChange();
}

}  // namespace StorageControl

}  // namespace mongo
