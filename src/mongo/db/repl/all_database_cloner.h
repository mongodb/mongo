/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/db/repl/database_cloner.h"

namespace mongo {
namespace repl {

class AllDatabaseCloner final : public BaseCloner {
public:
    struct Stats {
        size_t databasesCloned{0};
        std::vector<DatabaseCloner::Stats> databaseStats;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    AllDatabaseCloner(InitialSyncSharedData* sharedData,
                      const HostAndPort& source,
                      DBClientConnection* client,
                      StorageInterface* storageInterface,
                      ThreadPool* dbPool);

    virtual ~AllDatabaseCloner() = default;

    Stats getStats() const;

    std::string toString() const;

protected:
    ClonerStages getStages() final;

private:
    friend class AllDatabaseClonerTest;
    class ConnectStage : public ClonerStage<AllDatabaseCloner> {
    public:
        ConnectStage(std::string name, AllDatabaseCloner* cloner, ClonerRunFn stageFunc)
            : ClonerStage<AllDatabaseCloner>(name, cloner, stageFunc){};
        bool checkSyncSourceValidityOnRetry() final {
            return false;
        }
    };

    /**
     * Validation function to ensure we connect only to primary or secondary nodes.
     *
     * Because the cloner connection is separate from the usual inter-node connection pool and
     * did not have the 'hangUpOnStepDown:false' flag set in the initial isMaster request, we
     * will always disconnect if the sync source transitions to a state other than PRIMARY
     * or SECONDARY.  It will not disconnect on a PRIMARY to SECONDARY or SECONDARY to PRIMARY
     * transition because we no longer do that (the flag name is anachronistic).  After
     * disconnecting, this validation function will prevent us from reconnecting until the node
     * re-enters PRIMARY or SECONDARY state.
     *
     * The reason this is necessary is that in 4.2, commands which read metadata (listDatabases,
     * listCollections, listIndexes) succeed while the sync source is in RECOVERING or ROLLBACK.
     * In those states, this metadata may be out of date compared to the end of the oplog. So
     * we could for instance do a listCollections on a database while in RECOVERING, and miss an
     * entire collection that was recently added.  Then before we read any data (which would cause
     * a failure) the node could finish recovery, and we could end up missing an entire collection.
     * If the only data added to that collection was within the recovery period, the initial sync
     * would succeed and we would have an inconsistent node.  If other data was added we would
     * invariant during oplog application with a NamespaceNotFound error.
     */
    Status ensurePrimaryOrSecondary(const executor::RemoteCommandResponse& isMasterReply);

    /**
     * Stage function that makes a connection to the sync source.
     */
    AfterStageBehavior connectStage();

    /**
     * Stage function that gets the wire version and initial sync ID.
     */
    AfterStageBehavior getInitialSyncIdStage();

    /**
     * Stage function that retrieves database information from the sync source.
     */
    AfterStageBehavior listDatabasesStage();

    /**
     *
     * The postStage creates and runs the individual DatabaseCloners on each database found on
     * the sync source.
     */
    void postStage() final;

    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        return "admin db: { " + stage->getName() + ": 1 }";
    }

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to classes own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    // (MX) Write access with mutex from main flow of control, read access with mutex from other
    //      threads, read access allowed from main flow without mutex.
    ConnectStage _connectStage;                              // (R)
    ConnectStage _getInitialSyncIdStage;                     // (R)
    ClonerStage<AllDatabaseCloner> _listDatabasesStage;      // (R)
    std::vector<std::string> _databases;                     // (X)
    std::unique_ptr<DatabaseCloner> _currentDatabaseCloner;  // (MX)
    Stats _stats;                                            // (MX)
};

}  // namespace repl
}  // namespace mongo
