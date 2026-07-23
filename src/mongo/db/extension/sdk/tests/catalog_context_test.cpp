// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/host/catalog_context.h"

#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <string>
#include <string_view>

namespace mongo::extension {
namespace {
using namespace std::literals::string_view_literals;

TEST(CatalogContextTest, CatalogContextWithValidFields) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    const auto dbNameSd = "test"sv;
    const auto collNameSd = "namespace"sv;
    const auto expectedUUID = UUID::gen();

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest(dbNameSd, collNameSd),
        SerializationContext());

    expCtx->setUUID(expectedUUID);
    expCtx->setInRouter(true);
    expCtx->setExplain(ExplainOptions::Verbosity::kExecAllPlans);

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();
    const auto& extensionNamespaceString = extensionCatalogContext.namespaceString;

    ASSERT_EQUALS(byteViewAsStringView(extensionNamespaceString.databaseName),
                  std::string_view(dbNameSd));
    ASSERT_EQUALS(byteViewAsStringView(extensionNamespaceString.collectionName),
                  std::string_view(collNameSd));

    std::string uuidAsString(byteViewAsStringView(extensionCatalogContext.uuidString));
    auto statusWithUUID = UUID::parse(uuidAsString);
    ASSERT_TRUE(statusWithUUID.isOK());
    ASSERT_EQUALS(expectedUUID, statusWithUUID.getValue());

    ASSERT_EQUALS(extensionCatalogContext.inRouter, 1);
    ASSERT_EQUALS(extensionCatalogContext.willBeMerged, 0);
    ASSERT_EQUALS(extensionCatalogContext.verbosity,
                  ::MongoExtensionExplainVerbosity::kExecAllPlans);
    ASSERT_EQUALS(extensionCatalogContext.shardId.len, 0);
}

TEST(CatalogContextTest, CatalogContextWithEmptyFields) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();
    const auto dbNameSd = ""sv;
    const auto collNameSd = ""sv;

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest(dbNameSd, collNameSd),
        SerializationContext());

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();
    const auto& extensionNamespaceString = extensionCatalogContext.namespaceString;
    ASSERT_EQUALS(extensionNamespaceString.databaseName.len, 0);
    ASSERT_EQUALS(extensionNamespaceString.collectionName.len, 0);
    ASSERT_EQUALS(extensionCatalogContext.uuidString.len, 0);
    ASSERT_EQUALS(extensionCatalogContext.inRouter, 0);
    ASSERT_EQUALS(extensionCatalogContext.willBeMerged, 0);
    ASSERT_EQUALS(extensionCatalogContext.verbosity, ::MongoExtensionExplainVerbosity::kNotExplain);
    ASSERT_EQUALS(extensionCatalogContext.shardId.len, 0);
}

TEST(CatalogContextTest, CatalogContextWithWillBeMerged) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest("test"sv, "coll"sv),
        SerializationContext());

    expCtx->setInRouter(false);
    expCtx->setNeedsMerge(true);

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();

    ASSERT_EQUALS(extensionCatalogContext.inRouter, 0);
    ASSERT_EQUALS(extensionCatalogContext.willBeMerged, 1);
}

TEST(CatalogContextTest, CatalogContextWithShardId) {
    QueryTestServiceContext testCtx;
    auto opCtx = testCtx.makeOperationContext();

    // Set up ShardingState with a known shard ID.
    const std::string expectedShardId = "myShard0";
    ShardingState::get(testCtx.getServiceContext())
        ->setRecoveryCompleted({OID::gen(),
                                ClusterRole::ShardServer,
                                ConnectionString(HostAndPort("localhost", 27017)),
                                ShardId(expectedShardId)});

    const auto dbNameSd = "test"sv;
    const auto collNameSd = "namespace"sv;

    auto expCtx = make_intrusive<ExpressionContextForTest>(
        opCtx.get(),
        NamespaceString::createNamespaceString_forTest(dbNameSd, collNameSd),
        SerializationContext());

    const auto catalogContext = mongo::extension::host::CatalogContext(*expCtx);
    const auto& extensionCatalogContext = catalogContext.getAsBoundaryType();

    ASSERT_EQUALS(byteViewAsStringView(extensionCatalogContext.shardId),
                  std::string_view(expectedShardId));
}
}  // namespace
}  // namespace mongo::extension
