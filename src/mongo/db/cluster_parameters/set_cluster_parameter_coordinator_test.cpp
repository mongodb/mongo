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
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/cluster_parameters/set_cluster_parameter_coordinator_document_gen.h"
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
