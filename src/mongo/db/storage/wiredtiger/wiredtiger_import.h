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

#pragma once

#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {
struct WTimportArgs {
    // Just the base name, no "table:" nor "file:" prefix. No ".wt" suffix.
    std::string ident;
    // When querying WT metadata for "table:<ident>"
    std::string tableMetadata;
    // When querying WT metadata for "file:<ident>.wt"
    std::string fileMetadata;
};

struct WTIndexImportArgs final : WTimportArgs {
    std::string indexName;
};

struct CollectionImportMetadata {
    WTimportArgs collection;
    mongo::NamespaceString ns;
    // An _mdb_catalog document.
    mongo::BSONObj catalogObject;
    long long numRecords;
    long long dataSize;
    std::vector<WTIndexImportArgs> indexes;
};

/**
 * After opening a backup cursor on a donor replica set and copying the donor's files to a local
 * temp directory, use this function to roll back to the backup cursor's checkpoint timestamp and
 * retrieve collection metadata.
 */
std::vector<CollectionImportMetadata> wiredTigerRollbackToStableAndGetMetadata(
    OperationContext* opCtx, const std::string& importPath, const UUID& migrationId);

/**
 * When preparing to import a collection within a WUOW, use RecoveryUnit::registerChange to
 * update number/size of records when the import commits.
 */
std::unique_ptr<RecoveryUnit::Change> makeCountsChange(
    RecordStore* recordStore, const CollectionImportMetadata& collectionMetadata);
}  // namespace mongo
