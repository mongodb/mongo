// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_invocation.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <functional>
#include <string>
#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const WriteConcernOptions kMajorityWriteConcern{WriteConcernOptions::kMajority,
                                                WriteConcernOptions::SyncMode::UNSET,
                                                WriteConcernOptions::kNoTimeout};

// Mocks
class MockParameterService : public ServerParameterService {
public:
    MockParameterService(std::function<ServerParameter*(std::string_view)> get) : _getMock(get) {};

    ServerParameter* get(std::string_view parameterName) override {
        return _getMock(parameterName);
    }

private:
    std::function<ServerParameter*(std::string_view)> _getMock;
};

class MockServerParameter : public ServerParameter {
public:
    MockServerParameter(std::string_view name,
                        std::function<Status(const BSONElement& newValueElement)> validateImpl)
        : ServerParameter(name, ServerParameterType::kRuntimeOnly) {
        this->validateImpl = validateImpl;
    }
    void append(OperationContext* opCtx,
                BSONObjBuilder* b,
                std::string_view name,
                const boost::optional<TenantId>&) override {}

    void appendSupportingRoundtrip(OperationContext* opCtx,
                                   BSONObjBuilder* b,
                                   std::string_view name,
                                   const boost::optional<TenantId>&) override {}

    Status set(const BSONElement& newValueElement,
               const boost::optional<TenantId>& tenantId) override {
        return Status(ErrorCodes::BadValue, "Should not call set() in this test");
    }

    Status setFromString(std::string_view str, const boost::optional<TenantId>& tenantId) override {
        return Status(ErrorCodes::BadValue, "Should not call setFromString() in this test");
    }

    Status validate(const BSONElement& newValueElement,
                    const boost::optional<TenantId>& tenantId) const override {
        return validateImpl(newValueElement);
    }

private:
    std::function<Status(const BSONElement& newValueElement)> validateImpl;
};

class DBClientMock : public DBClientService {
public:
    DBClientMock(std::function<BatchedCommandResponse(
                     BSONObj, BSONObj, const boost::optional<auth::ValidatedTenancyScope>&)>
                     updateParameterOnDiskMock) {
        this->updateParameterOnDiskMockImpl = updateParameterOnDiskMock;
    }

    BatchedCommandResponse updateParameterOnDisk(
        BSONObj query,
        BSONObj update,
        const WriteConcernOptions&,
        const boost::optional<auth::ValidatedTenancyScope>& vts) override {
        return updateParameterOnDiskMockImpl(query, update, vts);
    }

    Timestamp getUpdateClusterTime(OperationContext*) override {
        LogicalTime lt;
        return lt.asTimestamp();
    }

private:
    std::function<BatchedCommandResponse(
        BSONObj, BSONObj, const boost::optional<auth::ValidatedTenancyScope>&)>
        updateParameterOnDiskMockImpl;
};

MockServerParameter alwaysValidatingServerParameter(std::string_view name) {

    MockServerParameter sp(name, [&](const BSONElement& newValueElement) { return Status::OK(); });

    return sp;
}

MockServerParameter alwaysInvalidatingServerParameter(std::string_view name) {

    MockServerParameter sp(name, [&](const BSONElement& newValueElement) {
        return Status(ErrorCodes::BadValue, "Parameter Validation Failed");
    });

    return sp;
}

DBClientMock alwaysSucceedingDbClient() {
    DBClientMock dbServiceMock(
        [&](BSONObj, BSONObj, const boost::optional<auth::ValidatedTenancyScope>&) {
            BatchedCommandResponse result;
            result.setStatus(Status::OK());
            return result;
        });

    return dbServiceMock;
}

DBClientMock tenantIdReportingDbClient() {
    DBClientMock dbServiceMock(
        [&](BSONObj, BSONObj, const boost::optional<auth::ValidatedTenancyScope>& vts) {
            uasserted(ErrorCodes::UnknownError,
                      (vts && vts->hasTenantId()) ? vts->tenantId().toString() : "");
            return BatchedCommandResponse();
        });

    return dbServiceMock;
}

DBClientMock alwaysFailingDbClient() {
    DBClientMock dbServiceMock(
        [&](BSONObj, BSONObj, const boost::optional<auth::ValidatedTenancyScope>&) {
            uasserted(ErrorCodes::UnknownError, "DB Client Update Failed");
            return BatchedCommandResponse();
        });

    return dbServiceMock;
}

// Tests
TEST(SetClusterParameterCommand, SucceedsWithObjectParameter) {

    MockServerParameter sp = alwaysValidatingServerParameter("Succeeds"sv);
    DBClientMock dbServiceMock = alwaysSucceedingDbClient();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) { return &sp; });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;

    BSONObj subObj = BSON("ok" << "hello_there");
    testCmdBson.append("testCmd"sv, subObj);

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);
    spyCtx.setWriteConcern(WriteConcernOptions::deserializerForIDL(BSON("w" << "majority")));
    SetClusterParameter testCmd(obj);

    fixture.invoke(&spyCtx, testCmd, boost::none, boost::none, kMajorityWriteConcern);
}

TEST(SetClusterParameterCommand, ThrowsWithNonObjectParameter) {

    MockServerParameter sp = alwaysValidatingServerParameter("Succeeds"sv);
    DBClientMock dbServiceMock = alwaysSucceedingDbClient();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) { return &sp; });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;
    testCmdBson << "testCommand" << 5;

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);
    SetClusterParameter testCmd(obj);

    ASSERT_THROWS_CODE(
        fixture.invoke(&spyCtx, testCmd, boost::none, boost::none, kMajorityWriteConcern),
        DBException,
        ErrorCodes::BadValue);
}

TEST(SetClusterParameterCommand, ThrowsWhenServerParameterValidationFails) {

    MockServerParameter sp =
        alwaysInvalidatingServerParameter("CommandFailsWhenServerParameterValidationFails"sv);
    DBClientMock dbServiceMock = alwaysSucceedingDbClient();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) { return &sp; });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;
    testCmdBson << "testCommand" << BSON("ok" << "someval");

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);
    SetClusterParameter testCmd(obj);

    ASSERT_THROWS_CODE_AND_WHAT(
        fixture.invoke(&spyCtx, testCmd, boost::none, boost::none, kMajorityWriteConcern),
        DBException,
        ErrorCodes::BadValue,
        "Parameter Validation Failed"sv);
}

TEST(SetClusterParameterCommand, ThrowsWhenDBUpdateFails) {

    MockServerParameter sp = alwaysValidatingServerParameter("CommandFailsWhenDBUpdateFails"sv);
    DBClientMock dbServiceMock = alwaysFailingDbClient();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) { return &sp; });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;
    testCmdBson << "testCommand" << BSON("ok" << "someval");

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);

    SetClusterParameter testCmd(obj);

    ASSERT_THROWS_WHAT(
        fixture.invoke(&spyCtx, testCmd, boost::none, boost::none, kMajorityWriteConcern),
        DBException,
        "DB Client Update Failed"sv);
}

TEST(SetClusterParameterCommand, ThrowsWhenParameterNotPresent) {

    DBClientMock dbServiceMock = alwaysSucceedingDbClient();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) {
        return ServerParameterSet::getClusterParameterSet()->get("doesNotExistParam"sv);
    });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;
    testCmdBson << "testCommand" << BSON("ok" << "someval");

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);

    SetClusterParameter testCmd(obj);

    ASSERT_THROWS_CODE(
        fixture.invoke(&spyCtx, testCmd, boost::none, boost::none, kMajorityWriteConcern),
        DBException,
        ErrorCodes::NoSuchKey);
}

TEST(SetClusterParameterCommand, TenantIdPassesThrough) {

    DBClientMock dbServiceMock = tenantIdReportingDbClient();
    MockServerParameter sp = alwaysValidatingServerParameter("TenantIdPassesThroughParameter"sv);

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("SomeTest");

    auto mpsPtr = std::make_unique<MockParameterService>([&](std::string_view s) { return &sp; });

    Client* clientPtr = client.get();

    BSONObjBuilder testCmdBson;
    testCmdBson << "testCommand" << BSON("ok" << "someval");

    BSONObj obj = testCmdBson.obj();

    SetClusterParameterInvocation fixture(std::move(mpsPtr), dbServiceMock);

    OperationContext spyCtx(clientPtr, 1234);

    TenantId tenantId(OID("123456789012345678901234"));

    SetClusterParameter testCmdNoTenant(obj);

    ASSERT_THROWS_CODE_AND_WHAT(
        fixture.invoke(&spyCtx, testCmdNoTenant, boost::none, boost::none, kMajorityWriteConcern),
        DBException,
        ErrorCodes::UnknownError,
        "");

    SetClusterParameter testCmdWithTenant(obj);
    testCmdWithTenant.setDbName(NamespaceString::makeClusterParametersNSS(tenantId).dbName());

    // Prepare the tenant operation context with tenant scope.
    auth::ValidatedTenancyScopeGuard::runAsTenant(&spyCtx, tenantId, [&]() {
        ASSERT_THROWS_CODE_AND_WHAT(
            fixture.invoke(
                &spyCtx, testCmdWithTenant, boost::none, boost::none, kMajorityWriteConcern),
            DBException,
            ErrorCodes::UnknownError,
            tenantId.toString());
    });
}

}  // namespace
}  // namespace mongo
