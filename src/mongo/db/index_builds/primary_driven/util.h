/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/database_name.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/primary_driven/registry.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <boost/optional.hpp>

MONGO_MOD_PUBLIC;
namespace mongo::index_builds::primary_driven {

Registry& registry(ServiceContext* svcCtx);

/**
 * Handles the start of a primary-driven index build. Adds it to the catalog and the registry,
 * creates its internal tables, and (if primary) writes the oplog entry.
 */
Status start(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             std::vector<IndexBuildInfo> indexes);

/**
 * Handles the commit of a primary-driven index build. Removes it from the catalog and the registry,
 * drops its internal tables, and (if primary) writes the oplog entry.
 */
Status commit(OperationContext* opCtx,
              DatabaseName dbName,
              const UUID& collectionUUID,
              const UUID& buildUUID,
              const std::vector<IndexBuildInfo>& indexes,
              const std::vector<boost::optional<MultikeyPaths>>& multikey);

/**
 * Handles the abort of a primary-driven index build. Removes it from the catalog and the registry,
 * drops its internal tables, and (if primary) writes the oplog entry.
 */
Status abort(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             const std::vector<IndexBuildInfo>& indexes,
             const Status& cause);

}  // namespace mongo::index_builds::primary_driven
