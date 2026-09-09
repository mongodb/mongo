// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/initial_sync/cloner_test_fixture.h"
#include "mongo/db/repl/initial_sync/initial_sync_shared_data.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

class InitialSyncClonerTestFixture : public ClonerTestFixture {
protected:
    void setUp() override;

    InitialSyncSharedData* getSharedData();

    // Updates the initial sync id stored in InitialSyncSharedData.
    void setInitialSyncId();

    UUID _initialSyncId = UUID::gen();
    static constexpr int kInitialRollbackId = 1;

    // Stands for a sync source that has already recorded some clean shutdowns. Deliberately neither
    // kNoCleanShutdownId, which is the separate "no history at all" case, nor small enough that a
    // base versus base + 1 mistake could still match by coincidence.
    static constexpr long long kBaseCleanShutdownId = 5;
    static const Timestamp kBeginApplyingTimestamp;

    /**
     * Builds a find response for the clean shutdown check the cloners run whenever they retry a
     * stage. The check's find uses limit 1, so it carries at most one document: boost::none for
     * "the sync source has recorded no clean shutdown since our baseline", or the single document
     * the check should pass judgement on.
     *
     * The mock server ignores filters, so a test states the outcome it wants directly rather than
     * seeding documents and relying on the query to select among them.
     */
    static BSONObj makeCleanShutdownFindResponse(boost::optional<BSONObj> doc = boost::none);

    /**
     * Builds a clean shutdown document, for handing to makeCleanShutdownFindResponse().
     */
    static BSONObj makeCleanShutdownDoc(long long id, Timestamp lastCheckpointTs);
};

}  // namespace repl
}  // namespace mongo
