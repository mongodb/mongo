// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Encapsulates metadata fields associated with an index build.
 */
struct IndexBuildInfo {
    /**
     * Creates an IndexBuildInfo with the given index spec and index ident, leaving the internal
     * idents unset. setInternalIdents() must be called prior to using the info to build an index.
     */
    IndexBuildInfo(BSONObj specObj, boost::optional<std::string> idxIdent);

    /**
     * Creates an IndexBuildInfo with the given index spec and index ident, and generates the
     * internal idents.
     */
    IndexBuildInfo(BSONObj specObj, std::string_view idxIdent, StorageEngine& storageEngine);

    /**
     * Creates an IndexBuildInfo with the index spec and generates both the index ident and the
     * internal idents.
     */
    IndexBuildInfo(BSONObj specObj, StorageEngine& storageEngine, const DatabaseName& dbName);

    /**
     * Extracts index name from the spec and returns it.
     */
    std::string_view getIndexName() const;

    /**
     * Generates new idents and initializes all member fields tracking idents of internal tables.
     */
    void setInternalIdents(StorageEngine& storageEngine);

    /**
     * Initializes all member fields tracking idents of temporary tables with the given
     * idents.
     */
    void setInternalIdents(boost::optional<std::string> sorterIdent,
                           boost::optional<std::string> sideWritesIdent,
                           boost::optional<std::string> skippedRecordsIdent,
                           boost::optional<std::string> constraintViolationsIdent);

    BSONObj toBSON() const;

    BSONObj spec;
    // Ident of the index table itself.
    std::string indexIdent;
    // Idents of temporary tables used during an index build. Some of these may or may not be used
    // depending on the index type and the index build method being used.
    boost::optional<std::string> sorterIdent;
    boost::optional<std::string> sideWritesIdent;
    boost::optional<std::string> skippedRecordsIdent;
    boost::optional<std::string> constraintViolationsIdent;
};

/**
 * Constructs IndexBuildInfo instances from the given index specs.
 */
std::vector<IndexBuildInfo> toIndexBuildInfoVec(const std::vector<BSONObj>& specs,
                                                StorageEngine& storageEngine,
                                                const DatabaseName& dbName);

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
