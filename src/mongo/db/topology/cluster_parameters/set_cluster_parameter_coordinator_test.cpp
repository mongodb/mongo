// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/topology/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
class SetClusterParameterCoordinatorTest : public unittest::Test {
protected:
    bool _isPersistedStateConflictingWithPreviousTime(
        const boost::optional<LogicalTime>& previousTime,
        const boost::optional<BSONObj>& currentClusterParameterValue) {
        return SetClusterParameterCoordinator::_isPersistedStateConflictingWithPreviousTime(
            previousTime, currentClusterParameterValue);
    }
    bool _parameterValuesEqual(const BSONObj& parameter, const BSONObj& persistedParameter) {
        return SetClusterParameterCoordinator::_parameterValuesEqual(parameter, persistedParameter);
    }
};

namespace {
const LogicalTime kSomeTimestamp{Timestamp{1, 0}};

// Verify that if "previousTime" is not set, then concurrent update conflict is not detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeNotPassed) {
    ASSERT(!_isPersistedStateConflictingWithPreviousTime(boost::none, boost::none));
}

// Verify that if "previousTime" is set to uninitialized and the parameter value is not present,
// then concurrent update conflict is not detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeUninitializedParameterNotPresent) {
    ASSERT(!_isPersistedStateConflictingWithPreviousTime(LogicalTime::kUninitialized, boost::none));
}

// Verify that if "previousTime" is set to uninitialized and the parameter value is present, then
// concurrent update conflict is detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeUninitializedParameterPresent) {
    ASSERT(_isPersistedStateConflictingWithPreviousTime(LogicalTime::kUninitialized, BSONObj{}));
}

// Verify that if "previousTime" is set and the parameter value is not present, then concurrent
// update conflict is detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeSetParameterNotPresent) {
    ASSERT(_isPersistedStateConflictingWithPreviousTime(kSomeTimestamp, boost::none));
}

// Verify that if "previousTime" is set and matches the "clusterParameterTime" in the parameter
// value, then concurrent update conflict is not detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeSetClusterParameterTimeMatches) {
    ASSERT(!_isPersistedStateConflictingWithPreviousTime(
        kSomeTimestamp,
        BSON(SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName
             << kSomeTimestamp.asTimestamp() << "attr1"
             << "val")));
}

// Verify that if "previousTime" is set, "previousTime" does not match the "clusterParameterTime" in
// the parameter value, then concurrent update conflict is detected.
TEST_F(SetClusterParameterCoordinatorTest,
       IsPersistedStateConflictingWithPreviousTimePreviousTimeSetClusterParameterTimeDoesNotMatch) {
    ASSERT(_isPersistedStateConflictingWithPreviousTime(
        kSomeTimestamp,
        BSON(SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName
             << Timestamp(2, 0) << "_id"
             << "parameterName")));
}

TEST_F(SetClusterParameterCoordinatorTest, ParameterValuesEqualParametersEqual) {
    ASSERT(_parameterValuesEqual(
        BSON("attr1" << "val"),
        BSON(SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName
             << Timestamp(2, 0) << "_id"
             << "parameterName"
             << "attr1"
             << "val")));
}

TEST_F(SetClusterParameterCoordinatorTest, ParameterValuesEqualParametersNotEqual) {
    ASSERT(!_parameterValuesEqual(
        BSON("attr1" << "val"),
        BSON(SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName
             << Timestamp(2, 0) << "_id"
             << "parameterName"
             << "attr1"
             << "val2")));
}

TEST_F(SetClusterParameterCoordinatorTest, ParameterValuesEqualParameterCountNotEqual) {
    ASSERT(!_parameterValuesEqual(
        BSON("attr1" << "val"),
        BSON(SetClusterParameterCoordinatorDocument::kClusterParameterTimeFieldName
             << Timestamp(2, 0) << "_id"
             << "parameterName"
             << "attr1"
             << "val"
             << "attr2"
             << "val")));
}
}  // namespace
}  // namespace mongo
