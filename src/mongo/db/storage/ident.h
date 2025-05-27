/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/database_name.h"

#include <string>

namespace mongo {

/**
 * Every collection / index persisted by the server has a corresponding table in the storage engine.
 * An 'ident' uniquely identifies the storage engine table for a collection or index.
 *
 * Simple wrapper around the 'ident' string.
 */
class Ident {
public:
    explicit Ident(StringData ident) : _ident(ident.toString()) {}
    virtual ~Ident() = default;

    const std::string& getIdent() const {
        return _ident;
    }

protected:
    const std::string _ident;
};

namespace ident {
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
 * TODO SERVER-102875: Consolidate default generated idents (for collections and indexes)
 * where 'directoryPerDB' and 'directoryForIndexes' are automatically obtained to
 * ensure idents are stored in their intended locations.
 *
 * The 'generateNew<Collection|Index>Ident()' methods produce a new, unique ident for a
 * 'collection|index' table. Default method for generating user-data table idents.
 */
std::string generateNewCollectionIdent(const DatabaseName& dbName,
                                       bool directoryPerDB,
                                       bool directoryForIndexes);

std::string generateNewIndexIdent(const DatabaseName& dbName,
                                  bool directoryPerDB,
                                  bool directoryForIndexes);

/**
 * Marking an ident as internal implies the underlying data is subject to different handling by the
 * server than that of standard collections and indexes.
 *
 * Generates a unique ident tagged with an 'internal-' prefix. Returns in ident in the form of
 * 'internal-<identStem><unique identifier>'.
 */
std::string generateNewInternalIdent(StringData identStem = ""_sd);

/**
 * Returns true if the ident specifies a basic "collection" or "index" table type.
 */
bool isCollectionOrIndexIdent(StringData ident);

/**
 * True if the ident contains the 'internal-<identStem>' prefix.
 */
bool isInternalIdent(StringData ident, StringData identStem = ""_sd);

bool isCollectionIdent(StringData ident);

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
