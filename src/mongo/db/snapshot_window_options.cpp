/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/snapshot_window_options.h"

#include "mongo/db/server_parameters.h"
#include "mongo/platform/compiler.h"

namespace mongo {

SnapshotWindowParams snapshotWindowParams;

/**
 * Provides validation for snapshot window server parameter settings.
 */

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedMaxTargetSnapshotHistoryWindowInSeconds =
    (new ExportedServerParameter<int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "maxTargetSnapshotHistoryWindowInSeconds",
        &snapshotWindowParams.maxTargetSnapshotHistoryWindowInSeconds))
        -> withValidator([](const int32_t& potentialNewValue) {
            if (potentialNewValue < 0) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "maxTargetSnapshotHistoryWindowInSeconds must be "
                                               "greater than or equal to 0. '"
                                            << potentialNewValue
                                            << "' is an invalid setting.");
            }
            return Status::OK();
        });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedCachePressureThreshold =
    (new ExportedServerParameter<int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "cachePressureThreshold",
        &snapshotWindowParams.cachePressureThreshold))
        -> withValidator([](const int32_t& potentialNewValue) {
            if (potentialNewValue < 0 || potentialNewValue > 100) {
                return Status(ErrorCodes::BadValue,
                              str::stream() << "cachePressureThreshold must be greater than or "
                                               "equal to 0 and less than or equal to 100. '"
                                            << potentialNewValue
                                            << "' is an invalid setting.");
            }
            return Status::OK();
        });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedSnapshotWindowMultiplicativeDecrease =
    (new ExportedServerParameter<double, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "snapshotWindowMultiplicativeDecrease",
        &snapshotWindowParams.snapshotWindowMultiplicativeDecrease))
        -> withValidator([](const double& potentialNewValue) {
            if (potentialNewValue <= 0 || potentialNewValue >= 1) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "snapshotWindowMultiplicativeDecrease must be greater "
                                     "than 0 and less than 1. '"
                                  << potentialNewValue
                                  << "' is an invalid setting.");
            }

            return Status::OK();
        });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedSnapshotWindowAdditiveIncreaseSeconds =
    (new ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "snapshotWindowAdditiveIncreaseSeconds",
        &snapshotWindowParams
             .snapshotWindowAdditiveIncreaseSeconds)) -> withValidator([](const int32_t&
                                                                              potentialNewValue) {
        if (potentialNewValue < 1) {
            return Status(
                ErrorCodes::BadValue,
                str::stream()
                    << "snapshotWindowAdditiveIncreaseSeconds must be greater than or equal to 1. '"
                    << potentialNewValue
                    << "' is an invalid setting.");
        }

        return Status::OK();
    });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedMinMillisBetweenSnapshotWindowInc =
    (new ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "minMillisBetweenSnapshotWindowInc",
        &snapshotWindowParams.minMillisBetweenSnapshotWindowInc))
        -> withValidator([](const int32_t& potentialNewValue) {
            if (potentialNewValue < 1) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "minMillisBetweenSnapshotWindowInc must be greater than or equal to 1. '"
                        << potentialNewValue
                        << "' is an invalid setting.");
            }

            return Status::OK();
        });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedMinMillisBetweenSnapshotWindowDec =
    (new ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "minMillisBetweenSnapshotWindowDec",
        &snapshotWindowParams.minMillisBetweenSnapshotWindowDec))
        -> withValidator([](const int32_t& potentialNewValue) {
            if (potentialNewValue < 1) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "minMillisBetweenSnapshotWindowDec must be greater than or equal to 1. '"
                        << potentialNewValue
                        << "' is an invalid setting.");
            }

            return Status::OK();
        });

MONGO_COMPILER_VARIABLE_UNUSED auto _exportedCheckCachePressurePeriodSeconds =
    (new ExportedServerParameter<std::int32_t, ServerParameterType::kStartupAndRuntime>(
        ServerParameterSet::getGlobal(),
        "checkCachePressurePeriodSeconds",
        &snapshotWindowParams.checkCachePressurePeriodSeconds))
        -> withValidator([](const int32_t& potentialNewValue) {
            if (potentialNewValue < 1) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream()
                        << "checkCachePressurePeriodSeconds must be greater than or equal to 1. '"
                        << potentialNewValue
                        << "' is an invalid setting.");
            }

            return Status::OK();
        });

}  // namespace mongo
