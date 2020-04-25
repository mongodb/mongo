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
#include "mongo/util/background.h"

namespace mongo {

namespace StorageControl {

void startStorageControls(ServiceContext* serviceContext) {
    auto storageEngine = serviceContext->getStorageEngine();

    // Instantiate a thread to periodically, and upon request, flush writes to disk.
    if (!storageEngine->isEphemeral() && storageEngine->isDurable()) {
        std::unique_ptr<JournalFlusher> journalFlusher = std::make_unique<JournalFlusher>();
        journalFlusher->go();
        JournalFlusher::set(serviceContext, std::move(journalFlusher));
    }
}

void stopStorageControls(ServiceContext* serviceContext) {
    auto storageEngine = serviceContext->getStorageEngine();

    if (!storageEngine->isEphemeral() && storageEngine->isDurable()) {
        JournalFlusher::get(serviceContext)->shutdown();
    }
}

void triggerJournalFlush(ServiceContext* serviceContext) {
    auto storageEngine = serviceContext->getStorageEngine();

    if (!storageEngine->isEphemeral() && storageEngine->isDurable()) {
        JournalFlusher::get(serviceContext)->triggerJournalFlush();
    }
}

void waitForJournalFlush(OperationContext* opCtx) {
    auto serviceContext = opCtx->getServiceContext();
    auto storageEngine = serviceContext->getStorageEngine();

    if (!storageEngine->isEphemeral() && storageEngine->isDurable()) {
        JournalFlusher::get(serviceContext)->waitForJournalFlush();
    } else {
        opCtx->recoveryUnit()->waitUntilDurable(opCtx);
    }
}

void interruptJournalFlusherForReplStateChange(ServiceContext* serviceContext) {
    auto storageEngine = serviceContext->getStorageEngine();

    if (!storageEngine->isEphemeral() && storageEngine->isDurable()) {
        JournalFlusher::get(serviceContext)->interruptJournalFlusherForReplStateChange();
    }
}

}  // namespace StorageControl

}  // namespace mongo
