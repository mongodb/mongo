// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <mutex>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_USE_REPLACEMENT(DatabaseHolder)]] DatabaseHolderImpl : public DatabaseHolder {
public:
    DatabaseHolderImpl() = default;

    Database* getDb(OperationContext* opCtx, const DatabaseName& dbName) const override;

    bool dbExists(OperationContext* opCtx, const DatabaseName& dbName) const override;

    Database* openDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     bool* justCreated = nullptr) override;

    void dropDb(OperationContext* opCtx, Database* db) override;

    void close(OperationContext* opCtx, const DatabaseName& dbName) override;

    void closeAll(OperationContext* opCtx) override;

    boost::optional<DatabaseName> getNameWithConflictingCasing(const DatabaseName& dbName) override;

    std::vector<DatabaseName> getNames() override;

    // This class is the owner of the Database objects opened by DatabaseHolderImpl. It contains
    // a DatabaseName -> Database map to locate Database's by name as well as a multimap of used for
    // efficient search of case insensitive name duplicates. The class keeps both structures
    // synchronized, and thus, it does not allow write access to the maps individually.
    class DBsIndex {
    public:
        using DBs = stdx::unordered_map<DatabaseName, std::unique_ptr<Database>>;

        const DBs& viewAll() const;

        Database* getOrCreate(const DatabaseName& dbName);

        void erase(const DatabaseName& dbName);

        boost::optional<DatabaseName> getAnyConflictingName(const DatabaseName& dbName) const;

        std::pair<Database*, bool> upsert(const DatabaseName& dbName, std::unique_ptr<Database> db);

    private:
        using NormalizedDatabaseName = std::string;
        using NormalizedDBs =
            std::unordered_multimap<NormalizedDatabaseName, DatabaseName>;  // NOLINT

        DBs _dbs;                      // Use for exact matching
        NormalizedDBs _normalizedDBs;  // Use to locate DBs with same normalized key

        static NormalizedDatabaseName normalize(const DatabaseName& dbName);
    };

private:
    boost::optional<DatabaseName> _getNameWithConflictingCasing_inlock(const DatabaseName& dbName);

    mutable std::mutex _m;

    DatabaseHolderImpl::DBsIndex _dbs;
};

}  // namespace mongo
