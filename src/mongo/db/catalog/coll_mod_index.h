/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/coll_mod_gen.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/key_string.h"

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
                                AutoGetCollection* autoColl,
                                const ParsedCollModIndexRequest& collModIndexRequest,
                                boost::optional<IndexCollModInfo>* indexCollModInfo,
                                BSONObjBuilder* result,
                                boost::optional<repl::OplogApplication::Mode> mode);

/**
 * Scans index to return the record ids of duplicates.
 * Performs a scan on the whole index if 'firstKeyString' is not provided. Otherwise, only scans
 * documents with 'firstKeyString'.
 */
std::list<std::set<RecordId>> scanIndexForDuplicates(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    const IndexDescriptor* idx,
    boost::optional<KeyString::Value> firstKeyString = {});

/**
 * Builds a BSONArray of the violations with duplicate index keys and returns the formatted error
 * status for not being able to convert the index to unique.
 */
Status buildConvertUniqueErrorStatus(OperationContext* opCtx,
                                     const CollectionPtr& collection,
                                     const std::list<std::set<RecordId>>& duplicateRecordsList);

}  // namespace mongo
