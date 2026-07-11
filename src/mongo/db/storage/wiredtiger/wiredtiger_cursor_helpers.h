// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/modules.h"

#include <wiredtiger.h>

namespace mongo {

class PseudoRandom;

// Helper functions that wrap around their respective WT_CURSOR data modification calls. These are
// used to change the RecoveryUnit's state when a write has been performed on the snapshot.
int wiredTigerCursorInsert(WiredTigerRecoveryUnit&, WT_CURSOR* cursor);
int wiredTigerCursorModify(WiredTigerRecoveryUnit&,
                           WT_CURSOR* cursor,
                           WT_MODIFY* entries,
                           int nentries);
int wiredTigerCursorUpdate(WiredTigerRecoveryUnit&, WT_CURSOR* cursor);
int wiredTigerCursorRemove(WiredTigerRecoveryUnit&, WT_CURSOR* cursor);

/**
 * Chooses the WT cursor `allowOverwrite` parameter for a write. When the caller's default is
 * non-blind and `providerAllowsBlindWrite` is true (e.g. non-primary oplog application where
 * the primary has already validated the write), samples true with probability
 * gWiredTigerBlindWriteRatio. Otherwise returns `defaultOverwrite` so existing behavior is
 * preserved.
 */
bool chooseBlindWriteOverwrite(bool defaultOverwrite,
                               bool providerAllowsBlindWrite,
                               PseudoRandom& prng);

}  // namespace mongo
