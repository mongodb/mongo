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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/version_context.h"

#include <string>
#include <vector>

namespace mongo {

class StorageEngine;
class DatabaseName;

/**
 * Encapsulates metadata fields associated with an index build.
 */
struct IndexBuildInfo {
    IndexBuildInfo(BSONObj specObj, boost::optional<std::string> idxIdent);

    /**
     * Generates new idents and initializes all ident-related member fields.
     * TODO SERVER-106716: Remove VersionContext parameter
     */
    IndexBuildInfo(BSONObj specObj,
                   StorageEngine& storageEngine,
                   const DatabaseName& dbName,
                   const VersionContext& vCtx);

    /**
     * Extracts index name from the spec and returns it.
     */
    StringData getIndexName() const;

    /**
     * Generates new idents and initializes all member fields tracking idents of temporary tables.
     * TODO SERVER-106716: Remove VersionContext parameter
     */
    void setInternalIdents(StorageEngine& storageEngine, const VersionContext& vCtx);

    /**
     * Initializes all member fields tracking idents of temporary tables with the given idents.
     */
    void setInternalIdents(boost::optional<std::string> sorterIdent,
                           boost::optional<std::string> sideWritesIdent,
                           boost::optional<std::string> skippedRecordsTrackerIdent,
                           boost::optional<std::string> constraintViolationsTrackerIdent);

    BSONObj toBSON() const;

    BSONObj spec;
    // Ident of the index table itself.
    std::string indexIdent;
    // Storage options that can affect indexIdent generation.
    bool directoryPerDB = false;
    bool directoryForIndexes = false;
    // Idents of temporary tables used during an index build. Some of these may or may not be used
    // depending on the index type and the index build method being used.
    boost::optional<std::string> sorterIdent;
    boost::optional<std::string> sideWritesIdent;
    boost::optional<std::string> skippedRecordsTrackerIdent;
    boost::optional<std::string> constraintViolationsTrackerIdent;
};

/**
 * Constructs IndexBuildInfo instances from the given index specs.
 * TODO SERVER-106716: Remove VersionContext parameter
 */
std::vector<IndexBuildInfo> toIndexBuildInfoVec(const std::vector<BSONObj>& specs,
                                                StorageEngine& storageEngine,
                                                const DatabaseName& dbName,
                                                const VersionContext& vCtx);

/**
 * Same as above, but does not populate the ident fields in the IndexBuildInfo instances.
 */
std::vector<IndexBuildInfo> toIndexBuildInfoVec(const std::vector<BSONObj>& specs);

/**
 * Returns the index names from the given list of IndexBuildInfo instances.
 */
std::vector<std::string> toIndexNames(const std::vector<IndexBuildInfo>& indexes);

/**
 * Returns the index specs from the given list of IndexBuildInfo instances.
 */
std::vector<BSONObj> toIndexSpecs(const std::vector<IndexBuildInfo>& indexes);

}  // namespace mongo
