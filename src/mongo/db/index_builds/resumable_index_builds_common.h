// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/index_builds/index_builds_common.h"
#include "mongo/db/index_builds/resumable_index_builds_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional.hpp>

namespace mongo::index_builds {

/**
 * Read and parse the index build resume state from the table at `ident`. Returns boost::none if the
 * table is missing, empty, or any record fails to parse.
 */
[[MONGO_MOD_PUBLIC]]
boost::optional<ResumeIndexInfo> readAndParseResumeIndexInfo(StorageEngine* engine,
                                                             OperationContext* opCtx,
                                                             const std::string& ident);

/**
 * Synthesizes an index build resume state, using default values for the state of each
 * index being built, given the registered metadata about the index build.
 */
ResumeIndexInfo synthesizeResumeIndexInfo(const UUID& buildUUID,
                                          IndexBuildPhaseEnum phase,
                                          const UUID& collectionUUID,
                                          const std::vector<IndexBuildInfo>& indexes);

/**
 * Returns the minimum `lastSpilledRecordId` across `indexes`, or boost::none if any index has no
 * `lastSpilledRecordId` (including when `indexes` is empty).
 */
boost::optional<RecordId> minLastSpilledRecordId(const std::vector<IndexStateInfo>& indexes);

}  // namespace mongo::index_builds
