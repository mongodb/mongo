/**
 *    Copyright (C) 2016 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace {
using namespace mongo;

// uassert and its friends
DEATH_TEST(UassertionTerminationTest, uassert, "Terminating with uassert") {
    uassert(40204, "Terminating with uassert", false);
}

DEATH_TEST(UassertionTerminationTest, uasserted, "Terminating with uasserted") {
    uasserted(40205, "Terminating with uasserted");
}

DEATH_TEST(UassertionTerminationTest, uassertStatusOK, "Terminating with uassertStatusOK") {
    uassertStatusOK(Status(ErrorCodes::InternalError, "Terminating with uassertStatusOK"));
}

DEATH_TEST(UassertionTerminationTest, uassertStatusOKOverload, "Terminating with uassertStatusOK") {
    uassertStatusOK(
        StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with uassertStatusOK"));
}

// fassert and its friends
DEATH_TEST(FassertionTerminationTest, fassert, "40206") {
    fassert(40206, false);
}

DEATH_TEST(FassertionTerminationTest, fassertOverload, "Terminating with fassert") {
    fassert(40207, {ErrorCodes::InternalError, "Terminating with fassert"});
}

DEATH_TEST(FassertionTerminationTest, fassertStatusOK, "Terminating with fassertStatusOK") {
    fassertStatusOK(40208, Status(ErrorCodes::InternalError, "Terminating with fassertStatusOK"));
}

DEATH_TEST(FassertionTerminationTest, fassertStatusOKOverload, "Terminating with fassertStatusOK") {
    fassertStatusOK(
        40209,
        StatusWith<std::string>(ErrorCodes::InternalError, "Terminating with fassertStatusOK"));
}

DEATH_TEST(FassertionTerminationTest, fassertFailed, "40210") {
    fassertFailed(40210);
}

DEATH_TEST(FassertionTerminationTest, fassertFailedNoTrace, "40211") {
    fassertFailedNoTrace(40211);
}

DEATH_TEST(FassertionTerminationTest,
           fassertFailedWithStatus,
           "Terminating with fassertFailedWithStatus") {
    fassertFailedWithStatus(
        40212, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatus"});
}

DEATH_TEST(FassertionTerminationTest,
           fassertFailedWithStatusNoTrace,
           "Terminating with fassertFailedWithStatusNoTrace") {
    fassertFailedWithStatusNoTrace(
        40213, {ErrorCodes::InternalError, "Terminating with fassertFailedWithStatusNoTrace"});
}

// massert and its friends
DEATH_TEST(MassertionTerminationTest, massert, "Terminating with massert") {
    massert(40214, "Terminating with massert", false);
}


DEATH_TEST(MassertionTerminationTest, massertStatusOK, "Terminating with massertStatusOK") {
    massertStatusOK(Status(ErrorCodes::InternalError, "Terminating with massertStatusOK"));
}

DEATH_TEST(MassertionTerminationTest,
           massertNoTraceStatusOK,
           "Terminating with massertNoTraceStatusOK") {
    massertNoTraceStatusOK(
        Status(ErrorCodes::InternalError, "Terminating with massertNoTraceStatusOK"));
}

DEATH_TEST(MassertionTerminationTest, msgasserted, "Terminating with msgasserted") {
    msgasserted(40215, "Terminating with msgasserted");
}

DEATH_TEST(MassertionTerminationTest, msgassertedNoTrace, "Terminating with msgassertedNoTrace") {
    msgassertedNoTrace(40216, "Terminating with msgassertedNoTrace");
}

DEATH_TEST(MassertionTerminationTest,
           msgassertedNoTraceWithStatus,
           "Terminating with msgassertedNoTraceWithStatus") {
    msgassertedNoTraceWithStatus(
        40217, Status(ErrorCodes::InternalError, "Terminating with msgassertedNoTraceWithStatus"));
}
}  // namespace
