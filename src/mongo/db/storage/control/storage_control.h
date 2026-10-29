// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/checkpoint_schedule_policy.h"
#include "mongo/util/modules.h"

#include <memory>

[[MONGO_MOD_PUBLIC]];

/**
 * Helper functions to manipulate independent processes that perform actions against the storage
 * engine.
 */
namespace mongo::StorageControl {
/**
 * Responsible for initializing independent processes for replication that interact with the storage
 * layer.
 *
 * Instantiates the JournalFlusher to flush writes to disk periodically and upon request. If
 * 'forTestOnly' is set, then the JournalFlusher will only run upon request so as not to disrupt
 * unit test expectations. If stopStorageControls() has been called with forRestart set, this
 * resumes the paused JournalFlusher.
 *
 * Safe to call again after stopStorageControls() has been called, to restart any processes that
 * were stopped.
 */
void startStorageControls(ServiceContext* serviceContext,
                          bool forTestOnly = false,
                          std::unique_ptr<CheckpointSchedulePolicy> policy = nullptr);

/**
 * Stops the processes begun by startStorageControls() and relays the reason to them.
 * Call this with forRestart set when we need to stop and restart storage controls without shutting
 * down the server. Using forRestart avoids destroying and recreating the JournalFlusher instance
 * which is a decoration on the global service context and is expected to be valid for the
 * lifetime of the service context.
 *
 * The JournalFlusher is paused when forRestart is set or shut down otherwise.
 *
 * Safe to call multiple times, whether or not startStorageControls() has been called.
 * startStorageControls() must be called after to resume the JournalFlusher if forRestart is used.
 */
void stopStorageControls(ServiceContext* serviceContext, const Status& reason, bool forRestart);
}  // namespace mongo::StorageControl
