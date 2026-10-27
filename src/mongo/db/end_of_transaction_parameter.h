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
 * Returns whether the end-of-transaction write-path augmentation is currently enabled. This is the
 * case only when the 'featureFlagEndOfTransactionChangeEventSynthesis' feature flag and the
 * 'allowEndOfTransaction' server parameter are both enabled. When disabled, transaction commits do
 * not write end-of-transaction accounting metadata into the oplog, so no endOfTransaction change
 * stream events can be produced from those commits.
 */
[[MONGO_MOD_PUBLIC]] bool isEndOfTransactionWritePathEnabled();

}  // namespace mongo
