// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <span>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::container_write {

/**
 * A tag struct that callers pass to insert() to assert that the key being inserted is guaranteed
 * not to already exist in the container. This enables a performance optimization of doing blind
 * writes, but actually overwriting an existing key is unsafe for replication (a write that
 * overwrites an existing key will fail to replicate). Only pass this when the calling code can
 * guarantee no duplicate key will be written.
 */
struct NonexistentKeyGuarantee {};

/**
 * Inserts into the given container and logs the operation in the oplog. If nkg is provided, the
 * caller guarantees the key does not already exist; otherwise, the insert will be rejected if the
 * key exists.
 */
Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value,
              boost::optional<NonexistentKeyGuarantee> nkg = boost::none);

/**
 * Inserts into the given container and logs the operation in the oplog. If nkg is provided, the
 * caller guarantees the key does not already exist; otherwise, the insert will be rejected if the
 * key exists.
 */
Status insert(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value,
              boost::optional<NonexistentKeyGuarantee> nkg = boost::none);

/**
 * Updates the value at the given key in the container and logs the operation in the oplog.
 * The key must already exist.
 */
Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value);

/**
 * Updates the value at the given key in the container and logs the operation in the oplog.
 * The key must already exist.
 */
Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value);

/**
 * Removes from the given container and logs the operation in the oplog.
 */
Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key);

/**
 * Removes from the given container and logs the operation in the oplog.
 */
Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key);

}  // namespace mongo::container_write
