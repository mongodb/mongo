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
 * A trivially copyable class that callers pass to container_write methods to guarantee that the
 * caller has already asserted that it is possible to write to containers. This enables a
 * performance optimization of avoiding the need to repeatedly assert on each container write
 * operation.
 */
class CanAcceptContainerWritesGuarantee {
public:
    /**
     * Asserts that container write support is enabled and that the node can accept writes for
     * containers. Validating this createds CanAcceptContainerWritesGuarantee which can be passed
     * into container_write operations, allowing them to skip re-asserting on each successive
     * container write operation in a batch.
     */
    [[nodiscard]] static CanAcceptContainerWritesGuarantee assertCanAcceptContainerWrites(
        OperationContext* opCtx);

private:
    CanAcceptContainerWritesGuarantee() = default;  // Creation must pass through the factory.
};

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
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none,
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
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none,
              boost::optional<NonexistentKeyGuarantee> nkg = boost::none);

/**
 * Updates the value at the given key in the container and logs the operation in the oplog.
 * The key must already exist.
 */
Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              std::span<const char> value,
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none);

/**
 * Updates the value at the given key in the container and logs the operation in the oplog.
 * The key must already exist.
 */
Status update(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              std::span<const char> value,
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none);

/**
 * Removes from the given container and logs the operation in the oplog.
 */
Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              IntegerKeyedContainer& container,
              int64_t key,
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none);

/**
 * Removes from the given container and logs the operation in the oplog.
 */
Status remove(OperationContext* opCtx,
              RecoveryUnit& ru,
              StringKeyedContainer& container,
              std::span<const char> key,
              boost::optional<CanAcceptContainerWritesGuarantee> wg = boost::none);

}  // namespace mongo::container_write
