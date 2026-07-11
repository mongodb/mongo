// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_sync_cloner_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/util/duration.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

void InitialSyncClonerTestFixture::setUp() {
    ClonerTestFixture::setUp();

    _sharedData = std::make_unique<InitialSyncSharedData>(kInitialRollbackId, Days(1), &_clock);

    // Set the initial sync ID on the mock server.
    _mockServer->insert(NamespaceString::kDefaultInitialSyncIdNamespace,
                        BSON("_id" << _initialSyncId));
}

InitialSyncSharedData* InitialSyncClonerTestFixture::getSharedData() {
    return checked_cast<InitialSyncSharedData*>(_sharedData.get());
}

void InitialSyncClonerTestFixture::setInitialSyncId() {
    std::lock_guard<InitialSyncSharedData> lk(*getSharedData());
    getSharedData()->setInitialSyncSourceId(lk, _initialSyncId);
}

}  // namespace repl
}  // namespace mongo
