// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/util/modules.h"

#include <boost/none.hpp>

namespace mongo {

class DatabaseHolderMock : public DatabaseHolder {
public:
    DatabaseHolderMock() = default;

    Database* getDb(OperationContext* opCtx, const DatabaseName& dbName) const override {
        return nullptr;
    }

    bool dbExists(OperationContext* opCtx, const DatabaseName& dbName) const override {
        return false;
    }

    Database* openDb(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     bool* justCreated = nullptr) override {
        return nullptr;
    }

    void dropDb(OperationContext* opCtx, Database* db) override {}

    void close(OperationContext* opCtx, const DatabaseName& dbName) override {}

    void closeAll(OperationContext* opCtx) override {}

    boost::optional<DatabaseName> getNameWithConflictingCasing(
        const DatabaseName& dbName) override {
        return boost::none;
    }

    std::vector<DatabaseName> getNames() override {
        return {};
    }
};

}  // namespace mongo
