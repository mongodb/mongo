// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"

#include <cstdint>

namespace mongo {
namespace repl {

/**
 * Computes a per-document hash to be stored on the oplog entry for continuous internode
 * validation.
 */
int64_t computeDocValidationHash(const BSONObj& doc);

/**
 * Returns true if continuous internode validation per document is enabled for the given
 * OperationContext. This indicates whether per-document validation hashes should be computed
 * and stored on the oplog entries.
 */
bool isContinuousInternodeValidationPerDocumentEnabled(OperationContext* opCtx);
}  // namespace repl
}  // namespace mongo
