// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <string>
#include <vector>

[[MONGO_MOD_PUBLIC]];
namespace mongo::index_builds {

/**
 * Aborts the index builds on the given collection that are building the given index names. Must be
 * called with the collection X lock held, and returns with that lock held (unless 'lock' declines
 * to re-acquire it).
 *
 * 'unlock' releases the caller's locks and 'lock' re-acquires them; the caller must treat anything
 * it held across the two as invalidated. 'lock' returns true to continue, or false to give up on
 * aborting (for example because the collection is gone, or this node is no longer primary); in this
 * case it returns immediately, index builds may remain, and the caller is left holding whatever
 * locks its own 'lock' implementation acquired before declining.
 */
void abort(OperationContext* opCtx,
           const NamespaceString& ns,
           const UUID& collectionUUID,
           const std::vector<std::string>& indexNames,
           const std::string& reason,
           const std::function<void()>& unlock,
           const std::function<bool()>& lock);

/**
 * Aborts every index build on the given collection. Must be called with the collection X lock held,
 * and returns with that lock held (unless 'lock' declines to re-acquire it).
 *
 * 'unlock' releases the caller's locks and 'lock' re-acquires them; the caller must treat anything
 * it held across the two as invalidated. 'lock' returns true to continue, or false to give up on
 * aborting (for example because the collection is gone, or this node is no longer primary); in this
 * case it returns immediately, index builds may remain, and the caller is left holding whatever
 * locks its own 'lock' implementation acquired before declining.
 */
void abort(OperationContext* opCtx,
           const NamespaceString& ns,
           const UUID& collectionUUID,
           const std::string& reason,
           const std::function<void()>& unlock,
           const std::function<bool()>& lock);

/**
 * Aborts every index build on every collection in the given database. Must be called with the
 * database X lock held, and returns with that lock held (unless 'lock' declines to re-acquire it).
 *
 * 'unlock' releases the caller's locks and 'lock' re-acquires them; the caller must treat anything
 * it held across the two as invalidated. 'lock' returns true to continue, or false to give up on
 * aborting (for example because the database is gone, or this node is no longer primary); in this
 * case it returns immediately, index builds may remain, and the caller is left holding whatever
 * locks its own 'lock' implementation acquired before declining.
 */
void abort(OperationContext* opCtx,
           const DatabaseName& dbName,
           const std::string& reason,
           const std::function<void()>& unlock,
           const std::function<bool()>& lock);

}  // namespace mongo::index_builds
