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

#include <vector>

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/tenant_base_cloner.h"
#include "mongo/db/repl/tenant_collection_cloner.h"
#include "mongo/db/repl/tenant_migration_shared_data.h"

namespace mongo {
namespace repl {

class TenantDatabaseCloner final : public TenantBaseCloner {
public:
    struct Stats {
        std::string dbname;
        Date_t start;
        Date_t end;
        size_t collections{0};
        size_t clonedCollections{0};
        std::vector<TenantCollectionCloner::Stats> collectionStats;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    TenantDatabaseCloner(const std::string& dbName,
                         TenantMigrationSharedData* sharedData,
                         const HostAndPort& source,
                         DBClientConnection* client,
                         StorageInterface* storageInterface,
                         ThreadPool* dbPool,
                         StringData tenantId);

    virtual ~TenantDatabaseCloner() = default;

    Stats getStats() const;

    std::string toString() const;

    Timestamp getOperationTime_forTest();

protected:
    ClonerStages getStages() final;

    bool isMyFailPoint(const BSONObj& data) const final;

private:
    friend class TenantDatabaseClonerTest;

    class TenantDatabaseClonerStage : public ClonerStage<TenantDatabaseCloner> {
    public:
        TenantDatabaseClonerStage(std::string name,
                                  TenantDatabaseCloner* cloner,
                                  ClonerRunFn stageFunc)
            : ClonerStage<TenantDatabaseCloner>(name, cloner, stageFunc) {}

        bool isTransientError(const Status& status) override {
            // Always abort on error.
            return false;
        }
    };

    /**
     * Stage function that retrieves collection information from the donor.
     */
    AfterStageBehavior listCollectionsStage();

    /**
     * Stage function that retrieves collection information locally for collections that are already
     * cloned.
     */
    AfterStageBehavior listExistingCollectionsStage();

    /**
     * The preStage sets the start time in _stats.
     */
    void preStage() final;

    /**
     * The postStage creates and runs the individual TenantCollectionCloners on each database found
     * on the sync source, and sets the end time in _stats when done.
     */
    void postStage() final;

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    // (MX) Write access with mutex from main flow of control, read access with mutex from other
    //      threads, read access allowed from main flow without mutex.
    const std::string _dbName;                                                // (R)
    std::vector<std::pair<NamespaceString, CollectionOptions>> _collections;  // (X)
    std::unique_ptr<TenantCollectionCloner> _currentCollectionCloner;         // (MX)

    TenantDatabaseClonerStage _listCollectionsStage;          // (R)
    TenantDatabaseClonerStage _listExistingCollectionsStage;  // (R)

    // The database name prefix of the tenant associated with this migration.
    std::string _tenantId;  // (R)

    // The operationTime returned with the listCollections result.
    Timestamp _operationTime;  // (X)

    Stats _stats;  // (MX)
};


}  // namespace repl
}  // namespace mongo
