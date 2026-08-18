// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/oplog_entry_gen.h"

namespace mongo {
namespace repl {

/**
 * Increments the internode document-hash mismatch counter for 'opType'.
 */
void incrementDocumentHashMismatchCount(OpTypeEnum opType);

}  // namespace repl
}  // namespace mongo
