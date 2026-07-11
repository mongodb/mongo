// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * Every collection / index persisted by the server has a corresponding table in the storage engine.
 * An 'ident' uniquely identifies the storage engine table for a collection or index.
 *
 * Simple wrapper around the 'ident' string.
 */
class Ident {
public:
    explicit Ident(std::string_view ident) : _ident(std::string{ident}) {}

    const std::string& getIdent() const {
        return _ident;
    }

private:
    const std::string _ident;
};

namespace ident {
using namespace std::literals::string_view_literals;
// Hardcoded idents should follow a "internal-camelCase" format. kSizeStore and kMdbCatalog
// predate this convention so they don't follow it, but future additions should.

// The size storer and catalog have hardcoded idents as we need to be able to open them before we
// can look up idents in the catalog.
constexpr inline std::string_view kSizeStorer = "sizeStorer"sv;
constexpr inline std::string_view kMdbCatalog = "_mdb_catalog"sv;

// Replicated fast count use hardcoded idents to avoid consulting the catalog when checking for
// existence on stepup.
constexpr inline std::string_view kFastCountMetadataStore = "internal-fastCountMetadataStore"sv;
constexpr inline std::string_view kFastCountMetadataStoreTimestamps =
    "internal-fastCountMetadataStoreTimestamps"sv;

/**
 * By default, a storage engine table is uniquely identified by an 'ident' that comes in 1 of 4
 * forms - dependent on the 'directoryPerDB' and 'directoryForIndexes' parameters.
 *      Neither:                 <collection|index>-<unique identifier>
 *      directoryPerDB:          <db>/<collection|index>-<unique identifier>
 *      directoryForIndexes:     <collection|index>/<unique identifier>
 *      directoryPerDB and directoryForIndexes:
 *                                <db>/<collection|index>/<unique identifier>
 * <collection|index> is a placeholder for either the string 'collection' or string 'index'.
 *
 * As of 8.2, the <unique identifier> of an ident is a generated UUID. In previous versions, the
 * <unique identifier> is a combination of '<counter>-<random number>'.
 *
 * The 'generateNew<Collection|Index>Ident()' methods produce a new, unique ident for a
 * 'collection|index' table. Default method for generating user-data table idents.
 */
std::string generateNewCollectionIdent(
    const DatabaseName& dbName,
    bool directoryPerDB,
    bool directoryForIndexes,
    const boost::optional<std::string_view>& optIdentUniqueTag = boost::none);

std::string generateNewIndexIdent(
    const DatabaseName& dbName,
    bool directoryPerDB,
    bool directoryForIndexes,
    const boost::optional<std::string_view>& optIdentUniqueTag = boost::none);

/**
 * Marking an ident as internal implies the underlying data is subject to different handling by the
 * server than that of standard collections and indexes.
 *
 * Generates a unique ident tagged with an 'internal-' prefix. Returns an ident in the form of
 * 'internal-<identStem><unique identifier>'.
 */
std::string generateNewInternalIdent(std::string_view identStem = ""sv);

/**
 * Returns an ident in the form of 'internal-<identStem>-<indexUniqueTag>' or
 * '<db>/internal-<identStem>-<indexUniqueTag>' when 'indexIdent' contains a db component.
 */
std::string generateNewInternalIndexBuildIdent(std::string_view identStem,
                                               std::string_view indexIdent);

/**
 * Returns the ident for the tracking table of a resumable primary-driven index build.
 * Format: 'internal-indexBuild-<buildUUID>'.
 */
std::string generateNewIndexBuildIdent(const UUID& buildUUID);

/**
 * Assumes 'ident' is a well-formed ident for a collection, returns the unique identifier component
 * of the ident.
 */
std::string_view getCollectionIdentUniqueTag(std::string_view ident,
                                             const DatabaseName& dbName,
                                             bool directoryPerDB,
                                             bool directoryForIndexes);

/**
 * Assumes 'ident' is a well-formed ident for an index, returns the unique identifier component
 * of the ident.
 */
std::string_view getIndexIdentUniqueTag(std::string_view ident,
                                        const DatabaseName& dbName,
                                        bool directoryPerDB,
                                        bool directoryForIndexes);

/**
 * Returns true if the ident specifies a basic "collection" or "index" table type.
 */
bool isCollectionOrIndexIdent(std::string_view ident);

/**
 * True if the ident contains the 'internal-<identStem>' prefix.
 */
bool isInternalIdent(std::string_view ident, std::string_view identStem = ""sv);

/**
 * Returns true if the ident is for one of the replicated fastcount containers.
 */
bool isReplicatedFastCountIdent(std::string_view ident);

bool isCollectionIdent(std::string_view ident);

bool isIndexIdent(std::string_view ident);

/**
 * Validates that the tag does not contain any characters which would be special when interpreted as
 * a path.
 */
bool validateTag(std::string_view uniqueTag);

/**
 * Returns false if the string is definitely not a well-formed ident or would be unsafe to interpret
 * as a path component. Returns true if it is something which syntactically could be an ident.
 * Creating an ident which this returns true for may still fail due to the filesystem imposing
 * additional restrictions (e.g. on Windows) or the maximum path length being exceeded.
 */
bool isValidIdent(std::string_view ident);

/**
 * Returns the directory component of the ident, which is the prefix before the last '/'.
 * Returns an empty string when the ident has no directory component.
 * Supplying an ill-formed ident will trigger a uassert.
 */
std::string_view getDirectory(std::string_view ident);

/**
 * When idents are generated with 'directoryPerDB', the name of the database is encoded within the
 * ident. Idents must be capable of conversion into valid filesystem path components to guarantee
 * correct mapping from the server to the file that holds the storage engine table.
 *
 * Given a 'dbName', generates 'dbName' as a string that is escaped so that it can be used as a
 * valid path component in an ident/ file system path.
 */
std::string createDBNamePathComponent(const DatabaseName& dbName);

}  // namespace ident

}  // namespace mongo
