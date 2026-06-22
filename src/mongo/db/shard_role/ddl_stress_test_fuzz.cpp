/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/initializer.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/scopeguard.h"

#include <atomic>
#include <vector>

#include "fuzztest/fuzztest.h"
#include "fuzztest/googletest_fixture_adapter.h"
#include "gtest/gtest.h"

namespace mongo {
namespace {

using namespace fuzztest;

const NamespaceString kTargetNss = NamespaceString::createNamespaceString_forTest("test.target");
const NamespaceString kOtherColl1Nss =
    NamespaceString::createNamespaceString_forTest("test.other1");
const NamespaceString kOtherColl2Nss =
    NamespaceString::createNamespaceString_forTest("test.other2");

auto ArbitraryNamespace() {
    return ElementOf<NamespaceString>({kTargetNss, kOtherColl1Nss, kOtherColl2Nss});
}

struct ClearMetadataDDL {};

enum class ShardingCollType { kUntracked, kUnsplittable, kSharded };

struct CreateDDL {
    NamespaceString nss;
    ShardingCollType shardingType;

    static auto arbitrary() {
        return StructOf<CreateDDL>(ArbitraryNamespace(),
                                   ElementOf<ShardingCollType>({ShardingCollType::kUntracked,
                                                                ShardingCollType::kUnsplittable,
                                                                ShardingCollType::kSharded}));
    }
};

struct DropDDL {};

using DDL = std::variant<ClearMetadataDDL, CreateDDL>;

class ShardServerFixture : public ShardServerTestFixtureWithCatalogCacheLoaderMock {
public:
    CatalogCacheMock* catalogCacheForRouter() {
        return getCatalogCacheMock();
    }

private:
    // This method is virtual since it's meant to be used by the unit test framework. However, this
    // fixture is re-used for the fuzzing test which does implement testing on its own. Therefore
    // there's no issue here to leave this blank.
    void TestBody() override {};
};

class DDLStateMachine {
private:
    void executeOperation(OperationContext* opCtx, const DDL& ddl) {
        std::visit([&](const auto& op) { return executeDDL(opCtx, op); }, ddl);
    }

    void executeDDL(OperationContext* opCtx, const CreateDDL& create) {
        if (!isCurrentPrimary) {
            return;
        }
        if (currentCatalog.contains(create.nss)) {
            return;
        }
        currentCatalog[create.nss] = create.shardingType;
        // TODO SERVER-129229: Implement the rest
    }
    void executeDDL(OperationContext* opCtx, const ClearMetadataDDL& clear) {
        auto csr = CollectionShardingRuntime::acquireExclusive(opCtx, kTargetNss);
        csr->clearCollectionMetadata(opCtx);
    }

    void checkInvariants() {
        // TODO SERVER-129227: Implement this
    }

    void reset() {
        isCurrentPrimary = true;
        currentCatalog.clear();
    };

    void executeDDLOperations(const std::vector<DDL>& ddls, std::atomic_flag& doneSignal) {
        ON_BLOCK_EXIT([&] { doneSignal.test_and_set(); });
        ThreadClient client{"BackgroundDDLExecutor",
                            actualFixture->getServiceContext()->getService()};
        for (const auto& ddl : ddls) {
            auto opCtx = client->makeOperationContext();
            executeOperation(opCtx.get(), ddl);
        }
    }

    bool isCurrentPrimary = true;
    stdx::unordered_map<NamespaceString, ShardingCollType> currentCatalog;
    boost::optional<ShardServerFixture> actualFixture;

public:
    DDLStateMachine() {
        // TODO SERVER-129585: Get rid of the initializer function call once fuzztest binaries
        // initialize things properly.
        runGlobalInitializersOrDie({});
        actualFixture.emplace();
        actualFixture->SetUp();
    }

    ~DDLStateMachine() {
        actualFixture->TearDown();
    }

    void FuzzDDLInvariants(const std::vector<DDL>& ddls) {
        reset();
        std::atomic_flag doneSignal;
        auto backgroundWriter = stdx::thread(
            &DDLStateMachine::executeDDLOperations, this, std::ref(ddls), std::ref(doneSignal));

        while (!doneSignal.test()) {
            checkInvariants();
            std::this_thread::yield();
        }

        backgroundWriter.join();
    }
};

auto ArbitraryDDL() {
    return VariantOf(Just(ClearMetadataDDL()), CreateDDL::arbitrary());
}

FUZZ_TEST_F(DDLStateMachine, FuzzDDLInvariants)
    .WithDomains(VectorOf(ArbitraryDDL()).WithMaxSize(32));

}  // namespace
}  // namespace mongo
