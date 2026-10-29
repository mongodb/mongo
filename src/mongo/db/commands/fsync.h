// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <mutex>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * Returns true if mongod is currently fsyncLocked.
 */
bool lockedForWriting();

/**
 * If the fsynclock thread has been created, shut it down.
 */
void shutdownFsyncLockThread();

/**
 * This is used to block oplogWriter and should never be acquired by others.
 */
extern std::mutex oplogWriterLockedFsync;

/**
 * This is used to block oplogApplier and should never be acquired by others.
 */
extern std::mutex oplogApplierLockedFsync;
}  // namespace mongo
