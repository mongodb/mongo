// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/initial_sync/initial_sync_cloner_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/clean_shutdown_gen.h"
#include "mongo/db/repl/initial_sync/repl_sync_shared_data.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/util/duration.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace repl {

const Timestamp InitialSyncClonerTestFixture::kBeginApplyingTimestamp = Timestamp(100, 1);

BSONObj InitialSyncClonerTestFixture::makeCleanShutdownDoc(long long id,
                                                           Timestamp lastCheckpointTs) {
    CleanShutdownDocument doc;
    doc.setId(id);
    doc.setCleanShutdownLastCheckpointTimestamp(lastCheckpointTs);

    BSONObjBuilder bob;
    doc.serialize(&bob);
    return bob.obj();
}

BSONObj InitialSyncClonerTestFixture::makeCleanShutdownFindResponse(boost::optional<BSONObj> doc) {
    BSONObjBuilder bob;
    bob.append("ok", 1);
    BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
    cursorBob.append("id", 0LL);
    cursorBob.append("ns", NamespaceString::kCleanShutdownLogNamespace.toStringForErrorMsg());
    BSONArrayBuilder batchBob(cursorBob.subarrayStart("firstBatch"));
    if (doc) {
        batchBob.append(*doc);
    }
    batchBob.done();
    cursorBob.done();
    return bob.obj();
}

void InitialSyncClonerTestFixture::setUp() {
    ClonerTestFixture::setUp();

    _sharedData = std::make_unique<InitialSyncSharedData>(kInitialRollbackId,
                                                          true /* cleanShutdownCheckEnabled */,
                                                          kBaseCleanShutdownId,
                                                          kBeginApplyingTimestamp,
                                                          Days(1),
                                                          &_clock);

    // Set the initial sync ID on the mock server.
    _mockServer->insert(NamespaceString::kDefaultInitialSyncIdNamespace,
                        BSON("_id" << _initialSyncId));

    // The cloners check the sync source for clean shutdowns every time they retry a stage. Default
    // to reporting none since the baseline, so only the tests that exercise the check have to say
    // anything about it.
    _mockServer->setCommandReply("find", makeCleanShutdownFindResponse());
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
