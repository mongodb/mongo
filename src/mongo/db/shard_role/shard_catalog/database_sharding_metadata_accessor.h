// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Represents the in-memory metadata for a database in a sharded cluster. It contains information
 * about the database's primary shard, database version, and access type to the metadata.
 */
class [[MONGO_MOD_PRIVATE]] DatabaseShardingMetadataAccessor {
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
    boost::optional<DatabaseVersion> getDbVersion(OperationContext* opCtx) const;

    /**
     * Gets the database's primary shard if available. Otherwise, boost::none.
     */
    boost::optional<ShardId> getDbPrimaryShard(OperationContext* opCtx) const;

    /**
     * Returns a monotonically increasing counter that is bumped on every mutation of the primary
     * shard / db version.
     */
    uint64_t getNumMetadataMutations() const {
        return _numMetadataMutations;
    }

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
    void setDbMetadata(OperationContext* opCtx,
                       const ShardId& dbPrimaryShard,
                       const DatabaseVersion& dbVersion);

    /**
     * Clears the database metadata. Requires `kWriteAccess`.
     */
    void clearDbMetadata(OperationContext* opCtx);

    /**
     * Sets the movePrimary operation flag to indicate it is in progress.
     */
    void setMovePrimaryInProgress(OperationContext* opCtx);

    /**
     * Unsets the movePrimary operation flag, indicating that the operation is no longer in
     * progress.
     */
    void unsetMovePrimaryInProgress(OperationContext* opCtx);

    /**
     * Switches between read, no-access and write access type to this component metadata.
     */
    void setAccessType(OperationContext* opCtx, AccessType accessType);

    /**
     * UNSAFE: Sets the database metadata (primary shard and database version), bypassing the access
     * type. This is maintained for backward compatibility, where database metadata could be
     * installed without entering the critical section.
     */
    void setDbMetadata_UNSAFE(OperationContext* opCtx,
                              const ShardId& dbPrimaryShard,
                              const DatabaseVersion& dbVersion);

private:
    const DatabaseName _dbName;

    // Represents the current access level to the database metadata. It controls whether operations
    // are allowed to read or modify the metadata.
    AccessType _accessType;

    // Cached metadata.
    boost::optional<ShardId> _dbPrimaryShard;
    boost::optional<DatabaseVersion> _dbVersion;
    bool _movePrimaryInProgress{false};

    // Monotonically increasing counter bumped on every write to `_dbPrimaryShard` / `_dbVersion`.
    uint64_t _numMetadataMutations{0};
};

/**
 * This is a decoration of the OperationContext. If getBypassReadDbMetadataAccess() returns true,
 * read accesses to authoritative database metadata are allowed while holding the critical section.
 * Likewise, if getBypassWriteDbMetadataAccess() returns true, writes to authoritative database
 * metadata are allowed without holding the critical section.
 *
 * Use BypassDatabaseMetadataAccess (with justification) to temporarily modify those values.
 */
class [[MONGO_MOD_PRIVATE]] OperationDatabaseMetadata {
    OperationDatabaseMetadata(const OperationDatabaseMetadata&) = delete;
    OperationDatabaseMetadata& operator=(const OperationDatabaseMetadata&) = delete;

public:
    OperationDatabaseMetadata() = default;
    ~OperationDatabaseMetadata() = default;

    static OperationDatabaseMetadata& get(OperationContext* opCtx);

    bool getBypassReadDbMetadataAccess() const {
        return _bypassReadDbMetadataAccess;
    }

    bool getBypassWriteDbMetadataAccess() const {
        return _bypassWriteDbMetadataAccess;
    }

private:
    friend class BypassDatabaseMetadataAccess;

    void setBypassReadDbMetadataAccess(bool set) {
        _bypassReadDbMetadataAccess = set;
    }

    void setBypassWriteDbMetadataAccess(bool set) {
        _bypassWriteDbMetadataAccess = set;
    }

    bool _bypassReadDbMetadataAccess{false};
    bool _bypassWriteDbMetadataAccess{false};
};

}  // namespace mongo
