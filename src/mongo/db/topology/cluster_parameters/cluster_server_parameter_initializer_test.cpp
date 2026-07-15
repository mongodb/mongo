// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_initializer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/server_parameter_with_storage.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_parameters/cluster_parameter_synchronization_helpers.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_gen.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

#include <ctime>
#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
using namespace cluster_server_parameter_test_util;

typedef ClusterParameterWithStorage<ClusterServerParameterTest> ClusterTestParameter;

class ClusterServerParameterInitializerTest : public ClusterServerParameterTestBase {
public:
    void setUp() final {
        ClusterServerParameterTestBase::setUp();
        {
            auto opCtx = cc().makeOperationContext();
            ASSERT_OK(createCollection(
                opCtx.get(), CreateCommand(NamespaceString::kClusterParametersNamespace)));
        }

        // Insert a document on-disk for ClusterServerParameterTest. This should be loaded in-memory
        // by the initializer during startup recovery and at the end of initial sync.
        Timestamp now(time(nullptr));
        auto doc = makeClusterParametersDoc(LogicalTime(now), kInitialIntValue, kInitialStrValue);

        upsert(doc, boost::none);
    }

    void tearDown() final {
        // Delete all cluster server parameter documents written and refresh in-memory state.
        remove(boost::none);
        auto opCtx = cc().makeOperationContext();
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest(NamespaceString::kClusterParametersNamespace,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);

        cluster_parameters::resynchronizeAllTenantParametersFromCollection(
            opCtx.get(), *coll.getCollectionPtr().get());
    }
    /**
     * Simulates the call to the ClusterServerParameterInitializer at the end of initial sync, when
     * data is available but is not guaranteed to be majority committed.
     */
    void doInitialSync() {
        auto opCtx = cc().makeOperationContext();
        _initializer.onConsistentDataAvailable(
            opCtx.get(), false /* isMajority */, false /* isRollback */);
    }

    /**
     * Simulates the call to the ClusterServerParameterInitializer at the end of startup recovery,
     * when we expect to see majority committed data on-disk.
     */
    void doStartupRecovery() {
        auto opCtx = cc().makeOperationContext();
        _initializer.onConsistentDataAvailable(
            opCtx.get(), true /* isMajority */, false /* isRollback */);
    }

protected:
    ClusterServerParameterInitializer _initializer;
};

TEST_F(ClusterServerParameterInitializerTest, OnInitialSync) {
    // Retrieve the in-memory test cluster server parameter and ensure it's set to the default
    // value.
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);
    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Indicate that data is available at the end of initial sync and check that the in-memory data
    // is updated.
    doInitialSync();
    sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);
    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);
}

TEST_F(ClusterServerParameterInitializerTest, OnStartupRecovery) {
    // Retrieve the test cluster server parameter and ensure it's set to the default value.
    auto* sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);
    ClusterServerParameterTest cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kDefaultIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kDefaultStrValue);

    // Indicate that data is available at the end of startup recovery and check that the in-memory
    // data is updated.
    doStartupRecovery();
    sp = ServerParameterSet::getClusterParameterSet()->get<ClusterTestParameter>(kCSPTest);
    ASSERT(sp != nullptr);
    cspTest = sp->getValue(boost::none);
    ASSERT_EQ(cspTest.getIntValue(), kInitialIntValue);
    ASSERT_EQ(cspTest.getStrValue(), kInitialStrValue);
}

}  // namespace
}  // namespace mongo
