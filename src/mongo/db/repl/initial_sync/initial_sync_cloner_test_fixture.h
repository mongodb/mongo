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
};

}  // namespace repl
}  // namespace mongo
