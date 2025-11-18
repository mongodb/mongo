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
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/index_builds/multi_index_block.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUBLIC mongo {
/**
 * Creates an index if it does not already exist.
 */
Status createIndex(OperationContext* opCtx,
                   StringData ns,
                   const BSONObj& keys,
                   bool unique = false);

/**
 * Creates an index from a BSON spec, if it does not already exist.
 */
Status createIndexFromSpec(OperationContext* opCtx, StringData ns, const BSONObj& spec);

/**
 * Creates an index from a BSON spec, if it does not already exist. If `clock` is non-null, writes
 * will be timestamped using the given clock. If it is null, they will be written with a fixed
 * timestamp.
 */
Status createIndexFromSpec(OperationContext* opCtx,
                           VectorClockMutable* clock,
                           StringData ns,
                           const BSONObj& spec);

Status initializeMultiIndexBlock(OperationContext* opCtx,
                                 CollectionWriter& collection,
                                 MultiIndexBlock& indexer,
                                 const BSONObj& spec,
                                 MultiIndexBlock::OnInitFn onInit = MultiIndexBlock::kNoopOnInitFn);
}  // namespace MONGO_MOD_PUBLIC mongo
