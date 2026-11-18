// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns whether the 'allowEndOfTransaction' server parameter is currently enabled. This is the
 * operator-facing kill switch; the write-path augmentation additionally requires the
 * 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag. Use
 * isEndOfTransactionWritePathEnabled() to decide whether to write end-of-transaction accounting
 * metadata.
 */
[[MONGO_MOD_PUBLIC]] bool isAllowEndOfTransactionEnabled();

/**
 * Returns whether end-of-transaction accounting metadata should be written to the oplog. This
 * requires the 'allowEndOfTransaction' server parameter (the operator kill switch) and the
 * 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag (the rollout gate) to be enabled.
 * It is not FCV-gated on master; FCV gating will be added when the feature is backported.
 */
[[MONGO_MOD_PUBLIC]] bool isEndOfTransactionWritePathEnabled();

/**
 * Refreshes the allowEndOfTransaction gauge whenever the server parameter's effective value
 * changes. Installed as the parameter's on_update callback. The callback ignores its value
 * argument and re-reads the parameter instead: concurrent updates can interleave the atomic store
 * and the callback, and re-reading lets the last callback to run converge the gauge on the latest
 * stored value.
 */
Status onUpdateAllowEndOfTransaction(bool value);

}  // namespace mongo
