/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_op_observer_configsvr.h"

#include "mongo/db/global_catalog/type_mongos.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/session/logical_session_id.h"
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
        throw ExceptionFor<ErrorCodes::InternalError>(
            Status(ErrorCodes::InternalError, "Mocked error"));
    }

    void onSamplerUpdate(const MongosType& doc) override {
        throw ExceptionFor<ErrorCodes::InternalError>(
            Status(ErrorCodes::InternalError, "Mocked error"));
    }

    void onSamplerDelete(const MongosType& doc) override {
        throw ExceptionFor<ErrorCodes::InternalError>(
            Status(ErrorCodes::InternalError, "Mocked error"));
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
