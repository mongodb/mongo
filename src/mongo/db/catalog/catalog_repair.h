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

#include "mongo/bson/timestamp.h"
#include "mongo/db/storage/storage_engine.h"

namespace mongo {
namespace catalog_repair {

/**
 * Drop abandoned idents using two-phase drop at the stable timestamp. Idents may be needed for
 * reads between the oldest and stable timestamps. If successful, returns a ReconcileResult with
 * indexes that need to be rebuilt or builds that need to be restarted.
 *
 * Abandoned internal idents require special handling based on the context known only to the
 * caller. For example, on starting from a previous unclean shutdown, we would always drop all
 * unknown internal idents. If we started from a clean shutdown, the internal idents may contain
 * information for resuming index builds.
 */
StatusWith<StorageEngine::ReconcileResult> reconcileCatalogAndIdents(
    OperationContext* opCtx,
    StorageEngine* engine,
    Timestamp stableTs,
    StorageEngine::LastShutdownState lastShutdownState,
    bool forRepair);

}  // namespace catalog_repair
}  // namespace mongo
