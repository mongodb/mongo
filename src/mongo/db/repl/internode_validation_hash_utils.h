// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {
namespace repl {

/**
 * Computes a per-document hash to be stored on the oplog entry for continuous internode
 * validation.
 *
 * Public so that offline validation can compute the same hash over a collection's documents.
 */
[[MONGO_MOD_PUBLIC]] int64_t computeDocValidationHash(const BSONObj& doc);

/**
 * Computes the same hash over a document that is already held as a range of bytes.
 */
[[MONGO_MOD_PUBLIC]] int64_t computeDocValidationHash(ConstDataRange doc);

/**
 * Computes the validation hash for an update. The pre-image and post-image are hashed independently
 * and XOR-ed together so the value composes with the delta-based collection validation hash:
 * XOR-ing removes the pre-image's contribution and adds the post-image's.
 */
int64_t computeUpdateValidationHash(const BSONObj& preImage, const BSONObj& postImage);

/**
 * Returns true if continuous internode validation per document is enabled for the given
 * OperationContext. This indicates whether per-document validation hashes should be computed
 * and stored on the oplog entries.
 */
bool isContinuousInternodeValidationPerDocumentEnabled(OperationContext* opCtx);

/**
 * Returns true if continuous internode validation per collection is enabled for the given
 * OperationContext. This indicates whether per-collection validation hashes should be accumulated
 * from the per-document hashes on the oplog entries, and requires per-document validation to be
 * enabled too.
 */
bool isContinuousInternodeValidationPerCollectionEnabled(OperationContext* opCtx);
}  // namespace repl
}  // namespace mongo
