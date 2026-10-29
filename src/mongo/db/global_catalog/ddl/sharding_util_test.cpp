// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/sharding_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/index_spec.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/repl/always_allow_non_local_writes.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

enum class CreateIndexApi { kWritablePrimary, kStepUp };

std::vector<BSONObj> getIndexes(OperationContext* opCtx, const NamespaceString& nss) {
    DBDirectClient client(opCtx);
    const bool includeBuildUUIDs = false;
    const int options = 0;
    auto specs = client.getIndexSpecs(nss, includeBuildUUIDs, options);
    return {specs.begin(), specs.end()};
}

bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
               opCtx,
               CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
               MODE_IS)
        .exists();
}

class ShardingUtilCreateIndexesTestBase : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        installVoteCommitIndexBuildMock();
        ensureIndexBuildsCollection();
    }

    void ensureIndexBuildsCollection() {
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kIndexBuildEntryNamespace);
    }

    void installVoteCommitIndexBuildMock() {
        replicationCoordinator()->setRunCmdOnPrimaryAndAwaitResponseFunction(
            [](OperationContext* opCtx,
               const DatabaseName&,
               const BSONObj& cmdObj,
               repl::ReplicationCoordinator::OnRemoteCmdScheduledFn,
               repl::ReplicationCoordinator::OnRemoteCmdCompleteFn) {
                const auto buildUUID = uassertStatusOK(UUID::parse(cmdObj["voteCommitIndexBuild"]));
                const auto votingNode =
                    HostAndPort(std::string{cmdObj["hostAndPort"].checkAndGetStringData()});
                uassertStatusOK(IndexBuildsCoordinator::get(opCtx)->voteCommitIndexBuild(
                    opCtx, buildUUID, votingNode));
                return BSON("ok" << 1);
            });
    }

    void createIndexDirectly(const NamespaceString& nss, BSONObj keys, bool unique) {
        IndexSpec index;
        index.addKeys(keys);
        index.unique(unique);

        DBDirectClient client(operationContext());
        client.createIndexes(nss, {index.toBSON()});
    }

    Status createIndexesAtStepUp(const NamespaceString& nss,
                                 const std::vector<IndexSpec_ForCatalog>& specs) {
        auto* opCtx = operationContext();
        repl::AllowNonLocalWritesBlock allowNonLocalWrites(opCtx);
        return sharding_util::createIndexesOnCollectionAtStepUp(opCtx, nss, specs);
    }
};

class ShardingUtilCreateIndexesTest : public ShardingUtilCreateIndexesTestBase,
                                      public testing::WithParamInterface<CreateIndexApi> {
protected:
    void setUp() override {
        ShardingUtilCreateIndexesTestBase::setUp();

        if (GetParam() == CreateIndexApi::kStepUp) {
            replicationCoordinator()->setCanAcceptNonLocalWrites(false);
        }
    }

    Status createIndexes(const NamespaceString& nss,
                         const std::vector<IndexSpec_ForCatalog>& specs) {
        if (GetParam() == CreateIndexApi::kWritablePrimary) {
            return sharding_util::createIndexesOnCollectionForWritablePrimary(
                operationContext(), nss, specs);
        }
        return createIndexesAtStepUp(nss, specs);
    }

    Status createIndexes(const NamespaceString& nss, BSONObj keys, bool unique) {
        return createIndexes(nss, {IndexSpec_ForCatalog{keys, unique}});
    }

    std::string apiName() const {
        return GetParam() == CreateIndexApi::kWritablePrimary ? "WritablePrimary" : "StepUp";
    }
};

INSTANTIATE_TEST_SUITE_P(ShardingUtilCreateIndexes,
                         ShardingUtilCreateIndexesTest,
                         ::testing::Values(CreateIndexApi::kWritablePrimary,
                                           CreateIndexApi::kStepUp),
                         [](const testing::TestParamInfo<CreateIndexApi>& info) {
                             return info.param == CreateIndexApi::kWritablePrimary
                                 ? "WritablePrimary"
                                 : "StepUp";
                         });

TEST_P(ShardingUtilCreateIndexesTest, CompatibleIndexAlreadyExists) {
    const auto nss = NamespaceString::createNamespaceString_forTest("config.foo");
    createIndexDirectly(nss, BSON("a" << 1 << "b" << 1), true);

    ASSERT_OK(createIndexes(nss, BSON("a" << 1 << "b" << 1), true));

    auto indexes = getIndexes(operationContext(), nss);
    ASSERT_EQ(2U, indexes.size()) << apiName();
}

TEST_P(ShardingUtilCreateIndexesTest, IncompatibleIndexAlreadyExists) {
    const auto nss = NamespaceString::createNamespaceString_forTest("config.foo");
    createIndexDirectly(nss, BSON("a" << 1 << "b" << 1), false);

    ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict,
                  createIndexes(nss, BSON("a" << 1 << "b" << 1), true))
        << apiName();
}

TEST_P(ShardingUtilCreateIndexesTest, CreateIndex) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.foo");

    ASSERT_FALSE(collectionExists(operationContext(), nss));

    Status status = createIndexes(nss, BSON("a" << 1 << "b" << 1), true);
    ASSERT_OK(status) << apiName();

    auto indexes = getIndexes(operationContext(), nss);
    ASSERT_EQ(2U, indexes.size()) << apiName();

    status = createIndexes(nss, BSON("a" << 1 << "b" << 1), true);
    ASSERT_OK(status) << apiName();
    indexes = getIndexes(operationContext(), nss);
    ASSERT_EQ(2U, indexes.size()) << apiName();

    status = createIndexes(nss, BSON("a" << 1 << "b" << 1), false);
    ASSERT_EQUALS(ErrorCodes::IndexKeySpecsConflict, status) << apiName();
    indexes = getIndexes(operationContext(), nss);
    ASSERT_EQ(2U, indexes.size()) << apiName();
}

TEST_F(ShardingUtilCreateIndexesTestBase, CreateIndexOnNonEmptyCollectionForWritablePrimary) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.foo");

    ASSERT_FALSE(collectionExists(operationContext(), nss));

    DBDirectClient dbDirectClient(operationContext());
    dbDirectClient.insert(nss, BSON("_id" << 1 << "a" << 1));

    auto status = sharding_util::createIndexesOnCollectionForWritablePrimary(
        operationContext(), nss, {IndexSpec_ForCatalog{BSON("a" << 1), false}});
    ASSERT_OK(status);
    auto indexes = getIndexes(operationContext(), nss);

    ASSERT_EQ(2U, indexes.size());
}

class ShardingUtilCreateIndexesDeathTest : public ShardingUtilCreateIndexesTestBase {
protected:
    void setUp() override {
        ShardingUtilCreateIndexesTestBase::setUp();
        replicationCoordinator()->setCanAcceptNonLocalWrites(false);
    }

    Status createIndexes(const NamespaceString& nss, BSONObj keys, bool unique) {
        return createIndexesAtStepUp(nss, {IndexSpec_ForCatalog{keys, unique}});
    }
};

DEATH_TEST_F(ShardingUtilCreateIndexesDeathTest,
             CreateIndexOnNonEmptyCollectionAtStepUp,
             "Tripwire assertion") {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("config.foo");
    DBDirectClient dbDirectClient(operationContext());
    dbDirectClient.insert(nss, BSON("_id" << 1 << "a" << 1));

    ASSERT_EQUALS(12352501, createIndexes(nss, BSON("a" << 1), false).code());
}

}  // namespace
}  // namespace mongo
