// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

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
 * 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag (the rollout gate) to be enabled,
 * and the cluster FCV to be at the latest version. The FCV requirement keeps the coordinator from
 * sending the new 'endOfTransactionScopes' field on the strict 'commitTransaction' command to a
 * shard that predates it and would reject the command.
 */
[[MONGO_MOD_PUBLIC]] bool isEndOfTransactionWritePathEnabled();

}  // namespace mongo
