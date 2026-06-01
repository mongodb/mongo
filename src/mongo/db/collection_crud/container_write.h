/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/modules.h"

#include <span>

#include <boost/optional.hpp>

MONGO_MOD_PUBLIC;

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
