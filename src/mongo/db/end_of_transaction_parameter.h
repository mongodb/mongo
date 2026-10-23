// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {

/**
 * Returns whether the 'allowEndOfTransaction' server parameter is currently enabled. When
 * disabled, transaction commits do not write end-of-transaction accounting metadata into the
 * oplog, so no endOfTransaction change stream events can be produced from those commits.
 */
[[MONGO_MOD_PUBLIC]] bool isAllowEndOfTransactionEnabled();

}  // namespace mongo
