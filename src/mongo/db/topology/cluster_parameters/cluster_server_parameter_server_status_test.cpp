// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_server_status.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_gen.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using TestParameter = ClusterParameterWithStorage<ClusterServerParameterTest>;
using ParameterStorage = TenantIdMap<ClusterServerParameterTest>;

struct StorageAndParameter {
    StorageAndParameter(std::string_view name,
                        LogicalTime time,
                        int intValue,
                        std::string_view stringValue)
        : parameter(std::make_unique<TestParameter>(name, storage)) {
        ClusterServerParameter base;
        base.set_id(name);
        base.setClusterParameterTime(time);
        ClusterServerParameterTest values;
        values.setClusterServerParameter(base);
        values.setIntValue(intValue);
        values.setStrValue(stringValue);
        storage[boost::none] = values;
    }

    ParameterStorage storage;
    std::unique_ptr<TestParameter> parameter;
};

class ClusterParameterServerStatusTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _opCtxHolder = makeOperationContext();
        _opCtx = _opCtxHolder.get();
        std::set<std::string> reportedParameters;
        for (auto i = 0; i < 2; i++) {
            auto name = fmt::format("TestParameter{}", i);
            _testParameters.push_back(std::make_unique<StorageAndParameter>(
                name,
                LogicalTime{Timestamp{1000, static_cast<unsigned int>(i)}},
                i,
                str::stream() << "Test String" << i));
            reportedParameters.insert(name);
        }
        _serverStatus = std::make_unique<ClusterServerParameterServerStatus>(
            &_parameterSet, std::move(reportedParameters));
    }

    BSONObj getReport() const {
        BSONObjBuilder result;
        _serverStatus->report(_opCtx, &result);
        return result.obj();
    }

    BSONObj serializeParameter(TestParameter* parameter) {
        return parameter->getValue(boost::none).toBSON();
    }

    ServerParameterSet _parameterSet;
    std::unique_ptr<ClusterServerParameterServerStatus> _serverStatus;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;
    std::vector<std::unique_ptr<StorageAndParameter>> _testParameters;
};

TEST_F(ClusterParameterServerStatusTest, ReportEmpty) {
    ASSERT_BSONOBJ_EQ(getReport(), BSONObj::kEmptyObject);
}

TEST_F(ClusterParameterServerStatusTest, ReportsOneParameter) {
    auto& parameterOwnership = _testParameters[0]->parameter;
    auto parameter = parameterOwnership.get();
    _parameterSet.add(std::move(parameterOwnership));

    ASSERT_BSONOBJ_EQ(getReport(),
                      BSON(ClusterServerParameterServerStatus::kClusterParameterFieldName
                           << BSON(parameter->name() << serializeParameter(parameter))));
}

TEST_F(ClusterParameterServerStatusTest, ReportsMultipleParameters) {
    BSONObjBuilder expected;
    BSONObjBuilder section =
        expected.subobjStart(ClusterServerParameterServerStatus::kClusterParameterFieldName);
    for (auto& storageAndParameter : _testParameters) {
        auto& parameter = storageAndParameter->parameter;
        section.append(parameter->name(), serializeParameter(parameter.get()));
        _parameterSet.add(std::move(parameter));
    }
    section.done();

    ASSERT_BSONOBJ_EQ(getReport(), expected.obj());
}

TEST_F(ClusterParameterServerStatusTest, DoNotReportParametersNotInReportSet) {
    _serverStatus = std::make_unique<ClusterServerParameterServerStatus>(&_parameterSet,
                                                                         std::set<std::string>{});
    _parameterSet.add(std::move(_testParameters[0]->parameter));

    ASSERT_BSONOBJ_EQ(getReport(), BSONObj::kEmptyObject);
}

TEST_F(ClusterParameterServerStatusTest, DoNotReportDisabledParameters) {
    auto& parameter = _testParameters[0]->parameter;
    parameter->disable(false);
    _parameterSet.add(std::move(parameter));
    ASSERT_BSONOBJ_EQ(getReport(), BSONObj::kEmptyObject);
}

TEST_F(ClusterParameterServerStatusTest, DoNotReportUnsetParameters) {
    auto& parameter = _testParameters[0]->parameter;
    ASSERT_OK(parameter->reset(boost::none));
    _parameterSet.add(std::move(parameter));
    ASSERT_BSONOBJ_EQ(getReport(), BSONObj::kEmptyObject);
}

}  // namespace
}  // namespace mongo
