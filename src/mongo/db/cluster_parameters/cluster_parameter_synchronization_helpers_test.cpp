/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/cluster_parameter_synchronization_helpers.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_test_util.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {
using namespace cluster_server_parameter_test_util;

class ClusterServerParameterSynchronizationHelpersTest : public ClusterServerParameterTestBase {
    // Ensure that we test all of the log statements as well.
    mongo::unittest::MinimumLoggedSeverityGuard _severityGuard{mongo::logv2::LogComponent::kControl,
                                                               mongo::logv2::LogSeverity::Debug(5)};
};

TEST_F(ClusterServerParameterSynchronizationHelpersTest, ValidateGoodClusterParameter) {
    BSONObj clusterParamDoc = BSON("_id" << kCSPTest);

    ASSERT_DOES_NOT_THROW(cluster_parameters::validateParameter(clusterParamDoc, boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, ValidateBadClusterParameterNameType) {
    BSONObj clusterParamDoc = BSON("_id" << 12345);

    ASSERT_THROWS_CODE(cluster_parameters::validateParameter(clusterParamDoc, boost::none),
                       DBException,
                       ErrorCodes::OperationFailed);
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, ValidateUnknownClusterParameterName) {
    BSONObj clusterParamDoc = BSON("_id" << "testUnknown");

    ASSERT_THROWS_CODE(cluster_parameters::validateParameter(clusterParamDoc, boost::none),
                       DBException,
                       ErrorCodes::OperationFailed);
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, CorrectClusterParameterDate) {
    auto opCtx = cc().makeOperationContext();

    BSONObj clusterParamDoc = BSON("_id" << kCSPTest << "clusterParameterTime" << Date_t::now());

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), clusterParamDoc, "", boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, CorrectClusterParameterTimestamp) {
    auto opCtx = cc().makeOperationContext();

    BSONObj clusterParamDoc = BSON("_id" << kCSPTest << "clusterParameterTime" << Timestamp());

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), clusterParamDoc, "", boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, EmptyClusterParameter) {
    auto opCtx = cc().makeOperationContext();

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), BSONObj(), "", boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, BadClusterParameterNameType) {
    auto opCtx = cc().makeOperationContext();

    BSONObj clusterParamDoc = BSON("_id" << 12345);

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), clusterParamDoc, "", boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, EmptyClusterParameterName) {
    auto opCtx = cc().makeOperationContext();

    BSONObj clusterParamDoc = BSON("_id" << "");

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), clusterParamDoc, "", boost::none));
}

TEST_F(ClusterServerParameterSynchronizationHelpersTest, MissingClusterParameterTime) {
    auto opCtx = cc().makeOperationContext();

    BSONObj clusterParamDoc = BSON("_id" << kCSPTest);

    ASSERT_DOES_NOT_THROW(
        cluster_parameters::updateParameter(opCtx.get(), clusterParamDoc, "", boost::none));
}

}  // namespace
}  // namespace mongo
