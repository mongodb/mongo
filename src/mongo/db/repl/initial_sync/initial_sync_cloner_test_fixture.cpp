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

#include "mongo/db/repl/initial_sync/initial_sync_cloner_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
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
    stdx::lock_guard<InitialSyncSharedData> lk(*getSharedData());
    getSharedData()->setInitialSyncSourceId(lk, _initialSyncId);
}

}  // namespace repl
}  // namespace mongo
