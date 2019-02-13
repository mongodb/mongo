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

#include "mongo/db/catalog/database_holder.h"

#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/string_map.h"

namespace mongo {

class DatabaseHolderImpl : public DatabaseHolder {
public:
    DatabaseHolderImpl() = default;

    Database* getDb(OperationContext* opCtx, StringData ns) const override;

    Database* openDb(OperationContext* opCtx, StringData ns, bool* justCreated = nullptr) override;

    void dropDb(OperationContext* opCtx, Database* db) override;

    void close(OperationContext* opCtx, StringData ns) override;

    void closeAll(OperationContext* opCtx) override;

    std::set<std::string> getNamesWithConflictingCasing(StringData name) override;

    std::unique_ptr<Collection> makeCollection(OperationContext* const opCtx,
                                               const StringData fullNS,
                                               OptionalCollectionUUID uuid,
                                               CollectionCatalogEntry* const details,
                                               RecordStore* const recordStore,
                                               DatabaseCatalogEntry* const dbce) override;

private:
    std::set<std::string> _getNamesWithConflictingCasing_inlock(StringData name);

    typedef StringMap<Database*> DBs;
    mutable SimpleMutex _m;
    DBs _dbs;

    // Databases objects and their constituent collections are destroyed and recreated when
    // databases are closed and opened. We use this counter to assign a new epoch to a database when
    // it is reopened. This permits callers to detect after yielding and reacquiring locks whether
    // their catalog pointers are still valid. Collection UUIDs are not sufficient, since they
    // remain stable when databases are closed and reopened.
    //
    // A thread must hold the global exclusive lock to write to this variable, and must hold the
    // global lock in at least MODE_IS to read it.
    uint64_t _epoch = 0;
};

}  // namespace mongo
