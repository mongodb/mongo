// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <string>
#include <vector>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];
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
             std::vector<IndexBuildInfo> indexes,
             boost::optional<std::string> indexBuildIdent);

/**
 * Handles the commit of a primary-driven index build. Removes it from the catalog and the registry,
 * drops its internal tables, and (if primary) writes the oplog entry.
 */
Status commit(OperationContext* opCtx,
              DatabaseName dbName,
              const UUID& collectionUUID,
              const UUID& buildUUID,
              const std::vector<IndexBuildInfo>& indexes,
              const std::vector<boost::optional<MultikeyPaths>>& multikey,
              boost::optional<std::string> indexBuildIdent);

/**
 * Handles the abort of a primary-driven index build. Removes it from the catalog and the registry,
 * drops its internal tables, and (if primary) writes the oplog entry.
 */
Status abort(OperationContext* opCtx,
             DatabaseName dbName,
             const UUID& collectionUUID,
             const UUID& buildUUID,
             const std::vector<IndexBuildInfo>& indexes,
             boost::optional<std::string> indexBuildIdent,
             const Status& cause);

/**
 * Handles retrieving the resume info of a primary-driven index build, if available.
 */
ResumeIndexInfo resumeInfo(OperationContext* opCtx,
                           const UUID& collectionUUID,
                           const UUID& buildUUID,
                           const std::vector<IndexBuildInfo>& indexes,
                           const std::string& ident);

/**
 * For each index, deletes any sorter table entries that fall outside the persisted ranges.
 */
void deleteSorterEntriesOutsideRanges(OperationContext* opCtx,
                                      const std::vector<IndexStateInfo>& resumeInfoIndexes);

/**
 * For each index, deletes all entries in the index table.
 */
void deleteAllIndexEntries(OperationContext* opCtx,
                           DatabaseName dbName,
                           const UUID& collectionUUID,
                           const std::vector<IndexBuildInfo>& indexes);

}  // namespace mongo::index_builds::primary_driven
