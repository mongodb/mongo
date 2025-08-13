/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class DatabaseHolderImpl : public DatabaseHolder {
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

    mutable stdx::mutex _m;

    DatabaseHolderImpl::DBsIndex _dbs;
};

}  // namespace mongo
