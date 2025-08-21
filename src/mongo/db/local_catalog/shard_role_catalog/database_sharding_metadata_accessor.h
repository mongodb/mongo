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

#include "mongo/db/database_name.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"

namespace mongo {

/**
 * Represents the in-memory metadata for a database in a sharded cluster. It contains information
 * about the database's primary shard, database version, and access type to the metadata.
 */
class DatabaseShardingMetadataAccessor {
public:
    DatabaseShardingMetadataAccessor(DatabaseName dbName);

    enum class AccessType {
        kReadAccess,  // Represents read-only access
        kWriteAccess  // Represents write-only access
    };

    // ---------------------------------------------------------------------
    //                               Accessors
    // ---------------------------------------------------------------------

    /**
     * Gets the database name.
     */
    const DatabaseName& getDbName() const {
        return _dbName;
    }

    /**
     * Gets the database version if available. Otherwise, boost::none.
     */
    boost::optional<DatabaseVersion> getDbVersion() const;

    /**
     * Gets the database's primary shard if available. Otherwise, boost::none.
     */
    boost::optional<ShardId> getDbPrimaryShard() const;

    /**
     * Checks if a movePrimary operation is in progress.
     */
    bool isMovePrimaryInProgress() const;

    // ---------------------------------------------------------------------
    //                               Mutators
    // ---------------------------------------------------------------------

    /**
     * Sets the database metadata (primary shard and database version). Requires `kWriteAccess`.
     */
    void setDbMetadata(const ShardId& dbPrimaryShard, const DatabaseVersion& dbVersion);

    /**
     * Clears the database metadata. Requires `kWriteAccess`.
     */
    void clearDbMetadata();

    /**
     * Sets the movePrimary operation flag to indicate it is in progress.
     */
    void setMovePrimaryInProgress();

    /**
     * Unsets the movePrimary operation flag, indicating that the operation is no longer in
     * progress.
     */
    void unsetMovePrimaryInProgress();

    /**
     * Switches between read, no-access and write access type to this component metadata.
     */
    void setAccessType(AccessType accessType);

    /**
     * UNSAFE: Sets the database metadata (primary shard and database version), bypassing the access
     * type. This is maintained for backward compatibility, where database metadata could be
     * installed without entering the critical section.
     */
    void setDbMetadata_UNSAFE(const ShardId& dbPrimaryShard, const DatabaseVersion& dbVersion);

private:
    const DatabaseName _dbName;

    // Represents the current access level to the database metadata. It controls whether operations
    // are allowed to read or modify the metadata.
    AccessType _accessType;

    // Cached metadata.
    boost::optional<ShardId> _dbPrimaryShard;
    boost::optional<DatabaseVersion> _dbVersion;
    bool _movePrimaryInProgress{false};
};

}  // namespace mongo
