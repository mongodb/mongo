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

#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/external_data_source_scope_guard.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

void applyConcernsAndReadPreference(OperationContext* opCtx, const ClientCursor& cursor);

struct CursorLocks {
    CursorLocks(OperationContext* opCtx, const NamespaceString& nss, ClientCursorPin& cursorPin);

    // A reference to the shared_ptr so that we drop the virtual collections (via the
    // destructor) after deleting our cursors and releasing our read locks.
    std::shared_ptr<ExternalDataSourceScopeGuard> extDataSourceScopeGuard;
    boost::optional<AutoGetCollectionForReadMaybeLockFree> readLock;
    boost::optional<AutoStatsTracker> statsTracker;
    boost::optional<HandleTransactionResourcesFromStasher> txnResourcesHandler;
};

}  // namespace mongo
