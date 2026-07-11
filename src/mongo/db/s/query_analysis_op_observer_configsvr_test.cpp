// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/query_analysis_op_observer_configsvr.h"

#include "mongo/db/global_catalog/type_mongos.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

class ThrowingQueryAnalysisCoordinator : public QueryAnalysisCoordinator {
public:
    ThrowingQueryAnalysisCoordinator() = default;

    void onSamplerInsert(const MongosType& doc) override {
        uasserted(ErrorCodes::InternalError, "Mocked error");
    }

    void onSamplerUpdate(const MongosType& doc) override {
        uasserted(ErrorCodes::InternalError, "Mocked error");
    }

    void onSamplerDelete(const MongosType& doc) override {
        uasserted(ErrorCodes::InternalError, "Mocked error");
    }
};

class TestQueryAnalysisCoordinatorFactory : public QueryAnalysisCoordinatorFactory {
public:
    QueryAnalysisCoordinator* getQueryAnalysisCoordinator(OperationContext* opCtx) override {
        return _coordinator.get();
    }

private:
    std::unique_ptr<QueryAnalysisCoordinator> _coordinator =
        std::make_unique<ThrowingQueryAnalysisCoordinator>();
};

class TestQueryAnalysisOpObserverConfigSvr : public QueryAnalysisOpObserverConfigSvr {
public:
    TestQueryAnalysisOpObserverConfigSvr(
        std::unique_ptr<QueryAnalysisCoordinatorFactory> coordinatorFactory)
        : QueryAnalysisOpObserverConfigSvr(std::move(coordinatorFactory)) {}

    void testInsert(OperationContext* opCtx, const MongosType& doc) {
        _onInserts(opCtx, doc);
    }

    void testUpdate(OperationContext* opCtx, const MongosType& doc) {
        _onUpdate(opCtx, doc);
    }

    void testDelete(OperationContext* opCtx, const MongosType& doc) {
        _onDelete(opCtx, doc);
    }
};

class QueryAnalysisOpObserverConfigSvrTest : public ConfigServerTestFixture {
public:
    QueryAnalysisOpObserverConfigSvrTest() : ConfigServerTestFixture() {}
};

TEST_F(QueryAnalysisOpObserverConfigSvrTest, InsertSamplerHandlesException) {
    auto coordinatorFactory = std::make_unique<TestQueryAnalysisCoordinatorFactory>();
    TestQueryAnalysisOpObserverConfigSvr queryAnalysisOpObserverConfigSvr(
        std::move(coordinatorFactory));

    auto doc = MongosType();

    ASSERT_DOES_NOT_THROW(queryAnalysisOpObserverConfigSvr.testInsert(operationContext(), doc));
}

TEST_F(QueryAnalysisOpObserverConfigSvrTest, UpdateSamplerHandlesExceptionSamplerHandlesException) {
    auto coordinatorFactory = std::make_unique<TestQueryAnalysisCoordinatorFactory>();
    TestQueryAnalysisOpObserverConfigSvr queryAnalysisOpObserverConfigSvr(
        std::move(coordinatorFactory));

    auto doc = MongosType();

    ASSERT_DOES_NOT_THROW(queryAnalysisOpObserverConfigSvr.testUpdate(operationContext(), doc));
}

TEST_F(QueryAnalysisOpObserverConfigSvrTest, DeleteSamplerHandlesException) {
    auto coordinatorFactory = std::make_unique<TestQueryAnalysisCoordinatorFactory>();
    TestQueryAnalysisOpObserverConfigSvr queryAnalysisOpObserverConfigSvr(
        std::move(coordinatorFactory));

    auto doc = MongosType();

    ASSERT_DOES_NOT_THROW(queryAnalysisOpObserverConfigSvr.testDelete(operationContext(), doc));
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
