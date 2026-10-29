// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/util/modules.h"

#include <list>
#include <set>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This is a subset of the collMod command options related to index modifications.
 *
 * Refer to CollModRequest in coll_mod.cpp for non-index collMod options.
 */
struct ParsedCollModIndexRequest {
    const IndexDescriptor* idx = nullptr;
    boost::optional<long long> indexExpireAfterSeconds;
    boost::optional<bool> indexHidden;
    boost::optional<bool> indexUnique;
    boost::optional<bool> indexPrepareUnique;
    boost::optional<bool> indexForceNonUnique;
};

/**
 * Performs the index modification described in "collModIndex" on the collection.
 *
 * Intended for use within a write conflict retry loop alongside other collMod operations.
 *
 * The appropriate collection locks should be acquired before calling this function.
 *
 * Used by collMod implementation only.
 */
void processCollModIndexRequest(OperationContext* opCtx,
                                Collection* writableColl,
                                const ParsedCollModIndexRequest& collModIndexRequest,
                                boost::optional<IndexCollModInfo>* indexCollModInfo,
                                BSONObjBuilder* result,
                                boost::optional<repl::OplogApplication::Mode> mode);

/**
 * Scans index to returns up to 16MB of RecordIds of duplicates.
 */
std::vector<std::vector<RecordId>> scanIndexForDuplicates(OperationContext* opCtx,
                                                          const Collection* coll,
                                                          const IndexDescriptor* idx);

/**
 * Builds a BSONArray of the violations with duplicate index keys and returns the formatted error
 * status for not being able to convert the index to unique.
 */
Status buildConvertUniqueErrorStatus(OperationContext* opCtx,
                                     const Collection* collection,
                                     const std::vector<std::vector<RecordId>>& duplicateRecords);

}  // namespace mongo
