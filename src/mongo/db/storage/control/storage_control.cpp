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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/control/storage_control.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/control/journal_flusher.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace StorageControl {

namespace {

bool areControlsStarted = false;

}  // namespace

void startStorageControls(ServiceContext* serviceContext, bool forTestOnly) {
    auto storageEngine = serviceContext->getStorageEngine();

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
    //
    // (Note: the ephemeral engine returns false for isDurable(), so we must be careful not to
    // disable it.)
    std::unique_ptr<JournalFlusher> journalFlusher = std::make_unique<JournalFlusher>(
        /*disablePeriodicFlushes*/ forTestOnly ||
        (!storageEngine->isDurable() && !storageEngine->isEphemeral()));
    journalFlusher->go();
    JournalFlusher::set(serviceContext, std::move(journalFlusher));

    areControlsStarted = true;
}

void stopStorageControls(ServiceContext* serviceContext, const Status& reason) {
    if (areControlsStarted) {
        JournalFlusher::get(serviceContext)->shutdown(reason);
    }
}

void triggerJournalFlush(ServiceContext* serviceContext) {
    JournalFlusher::get(serviceContext)->triggerJournalFlush();
}

void waitForJournalFlush(OperationContext* opCtx) {
    JournalFlusher::get(opCtx)->waitForJournalFlush();
}

void interruptJournalFlusherForReplStateChange(ServiceContext* serviceContext) {
    JournalFlusher::get(serviceContext)->interruptJournalFlusherForReplStateChange();
}

}  // namespace StorageControl

}  // namespace mongo
