/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/cluster_server_parameter_initializer.h"
#include "mongo/idl/cluster_server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_response.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
using namespace cluster_server_parameter_test_util;

class ClusterServerParameterInitializerTest : public ClusterServerParameterTestBase {
public:
    void setUp() final {
        ClusterServerParameterTestBase::setUp();

        // Insert a document on-disk for ClusterServerParameterTest. This should be loaded in-memory
        // by the initializer during startup recovery and at the end of initial sync.
        Timestamp now(time(nullptr));
        const auto doc =
            makeClusterParametersDoc(LogicalTime(now), kInitialIntValue, kInitialStrValue);

        upsert(doc);
    }

    void tearDown() final {
        // Delete all cluster server parameter documents written and refresh in-memory state.
        remove();
        auto opCtx = cc().makeOperationContext();
        _initializer.resynchronizeAllParametersFromDisk(opCtx.get());
    }
    /**
     * Simulates the call to the ClusterServerParameterInitializer at the end of initial sync, when
     * data is available but is not guaranteed to be majority committed.
     */
    void doInitialSync() {
        auto opCtx = cc().makeOperationContext();
        _initializer.onInitialDataAvailable(opCtx.get(), false /* isMajorityDataAvailable */);
    }

    /**
     * Simulates the call to the ClusterServerParameterInitializer at the end of startup recovery,
     * when we expect to see majority committed data on-disk.
     */
    void doStartupRecovery() {
        auto opCtx = cc().makeOperationContext();
        _initializer.onInitialDataAvailable(opCtx.get(), true /* isMajorityDataAvailable */);
    }

protected:
    ClusterServerParameterInitializer _initializer;
};

TEST_F(ClusterServerParameterInitializerTest, OnInitialSync) {
    // Retrieve the in-memory test cluster server parameter and ensure it's set to the default
    // value.
    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);
    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Indicate that data is available at the end of initial sync and check that the in-memory data
    // is updated.
    doInitialSync();
    sp = ServerParameterSet::getClusterParameterSet()
             ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                 ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);
    cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);
}

TEST_F(ClusterServerParameterInitializerTest, OnStartupRecovery) {
    // Retrieve the test cluster server parameter and ensure it's set to the default value.
    auto* sp = ServerParameterSet::getClusterParameterSet()
                   ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                       ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);
    ClusterServerParameterTest cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Indicate that data is available at the end of startup recovery and check that the in-memory
    // data is updated.
    doStartupRecovery();
    sp = ServerParameterSet::getClusterParameterSet()
             ->get<IDLServerParameterWithStorage<ServerParameterType::kClusterWide,
                                                 ClusterServerParameterTest>>(kCSPTest);
    ASSERT(sp != nullptr);
    cspTest = sp->getValue();
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);
}

}  // namespace
}  // namespace mongo
