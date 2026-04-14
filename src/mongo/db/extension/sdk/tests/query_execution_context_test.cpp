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

#include "mongo/db/extension/host/query_execution_context.h"

#include "mongo/db/curop.h"
#include "mongo/db/extension/host_connector/adapter/host_services_adapter.h"
#include "mongo/db/extension/host_connector/adapter/query_execution_context_adapter.h"
#include "mongo/db/extension/sdk/query_execution_context_handle.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::extension {
namespace {

class QueryExecutionContextTestFixture : public unittest::Test {
protected:
    void setUp() override {
        _opCtx = _testCtx.makeOperationContext();
        _nss = NamespaceString::createNamespaceString_forTest("db", "coll");
        _expCtx =
            make_intrusive<ExpressionContextForTest>(_opCtx.get(), _nss, SerializationContext());
    }

    QueryTestServiceContext _testCtx;
    ServiceContext::UniqueOperationContext _opCtx;
    NamespaceString _nss;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

TEST_F(QueryExecutionContextTestFixture, CheckForInterruptOk) {
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    ASSERT_EQ(handle->checkForInterrupt(), ExtensionGenericStatus());
}

TEST_F(QueryExecutionContextTestFixture, CheckForInterruptDefaultKillCode) {
    _opCtx->markKilled();

    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);
    auto status = handle->checkForInterrupt();

    ASSERT_EQ(handle->checkForInterrupt(),
              ExtensionGenericStatus(ErrorCodes::Interrupted, "operation was interrupted"));
}

TEST_F(QueryExecutionContextTestFixture, CheckForInterruptCustomKillCode) {
    ErrorCodes::Error customKillCode = ErrorCodes::Error(11098301);
    _opCtx->markKilled(customKillCode);

    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    ASSERT_EQ(handle->checkForInterrupt(),
              ExtensionGenericStatus(customKillCode, "operation was interrupted"));
}

TEST_F(QueryExecutionContextTestFixture, CheckForDeadline) {
    _opCtx->setDeadlineAfterNowBy(Seconds{5}, ErrorCodes::ExceededTimeLimit);
    const auto deadlineTimestampMs = _opCtx->getDeadline().asInt64();
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    ASSERT_EQ(handle->getDeadlineTimestampMs(), deadlineTimestampMs);
}

TEST_F(QueryExecutionContextTestFixture, GetDeadlineTimestampMsWhenNoDeadline) {
    // Do not set a deadline; OperationContext defaults to Date_t::max() (no deadline).
    const auto deadlineTimestampMs = _opCtx->getDeadline().asInt64();
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    ASSERT_EQ(handle->getDeadlineTimestampMs(), deadlineTimestampMs);
    ASSERT_EQ(handle->getDeadlineTimestampMs(), std::numeric_limits<int64_t>::max());
}

TEST_F(QueryExecutionContextTestFixture, GetHostMetricsEmpty) {
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    BSONObj result = handle->getHostMetrics({});
    ASSERT_TRUE(result.isEmpty());
}

TEST_F(QueryExecutionContextTestFixture, GetHostMetricsKnownFieldNotPopulated) {
    // Request a known OpDebug field that is not populated (searchIdLookupMetrics is null).
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    BSONObj result = handle->getHostMetrics({"docsSeenByIdLookup"});
    // The field is known but not populated — it should not appear in the result.
    ASSERT_FALSE(result.hasField("docsSeenByIdLookup"));
}

TEST_F(QueryExecutionContextTestFixture, GetHostMetricsPopulatedFields) {
    // Populate searchIdLookupMetrics on the current operation's OpDebug.
    auto& opDebug = CurOp::get(_opCtx.get())->debug();
    opDebug.searchIdLookupMetrics = std::make_shared<OpDebug::SearchIdLookupMetrics>();
    opDebug.searchIdLookupMetrics->incrementDocsSeenByIdLookup();
    opDebug.searchIdLookupMetrics->incrementDocsSeenByIdLookup();
    opDebug.searchIdLookupMetrics->incrementDocsReturnedByIdLookup();

    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    BSONObj result = handle->getHostMetrics({"docsSeenByIdLookup", "docsReturnedByIdLookup"});
    ASSERT_EQ(result["docsSeenByIdLookup"].Long(), 2LL);
    ASSERT_EQ(result["docsReturnedByIdLookup"].Long(), 1LL);
}

TEST_F(QueryExecutionContextTestFixture, GetHostMetricsUnknownFieldThrows) {
    std::unique_ptr<host::QueryExecutionContext> wrappedCtx =
        std::make_unique<host::QueryExecutionContext>(_expCtx.get());
    host_connector::QueryExecutionContextAdapter adapter(std::move(wrappedCtx));
    sdk::QueryExecutionContextHandle handle(&adapter);

    // Requesting an unknown metric name must throw a DBException.
    ASSERT_THROWS(handle->getHostMetrics({"thisMetricDoesNotExist"}), DBException);
}

class QueryExecutionContextVTableDeathTest : public unittest::Test {
public:
    void setUp() override {
        // Initialize HostServices so that aggregation stages will be able to access member
        // functions, e.g. to run assertions.
        extension::sdk::HostServicesAPI::setHostServices(
            &extension::host_connector::HostServicesAdapter::get());
    }
};

DEATH_TEST_F(QueryExecutionContextVTableDeathTest, InvalidCheckForInterrupt, "11098300") {
    auto vtable = mongo::extension::host_connector::QueryExecutionContextAdapter::getVTable();
    vtable.check_for_interrupt = nullptr;
    sdk::QueryExecutionContextAPI::assertVTableConstraints(vtable);
};

DEATH_TEST_F(QueryExecutionContextVTableDeathTest, InvalidGetMetrics, "11213507") {
    auto vtable = mongo::extension::host_connector::QueryExecutionContextAdapter::getVTable();
    vtable.get_metrics = nullptr;
    sdk::QueryExecutionContextAPI::assertVTableConstraints(vtable);
};

DEATH_TEST_F(QueryExecutionContextVTableDeathTest, InvalidGetDeadlineTimestampMs, "11646100") {
    auto vtable = mongo::extension::host_connector::QueryExecutionContextAdapter::getVTable();
    vtable.get_deadline_timestamp_ms = nullptr;
    sdk::QueryExecutionContextAPI::assertVTableConstraints(vtable);
};

DEATH_TEST_F(QueryExecutionContextVTableDeathTest, InvalidGetHostMetrics, "12199900") {
    auto vtable = mongo::extension::host_connector::QueryExecutionContextAdapter::getVTable();
    vtable.get_host_metrics = nullptr;
    sdk::QueryExecutionContextAPI::assertVTableConstraints(vtable);
};

}  // namespace
}  // namespace mongo::extension
