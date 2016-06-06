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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/request_types/control_balancer_request_type.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using unittest::assertGet;

TEST(ControlBalancerRequest, ParsingFromMongosCommandForStart) {
    auto request = assertGet(ControlBalancerRequest::parseFromMongosCommand(BSON("controlBalancer"
                                                                                 << "start")));

    ASSERT_EQ(ControlBalancerRequest::kStart, request.getAction());
}

TEST(ControlBalancerRequest, ParsingFromMongosCommandForStop) {
    auto request = assertGet(ControlBalancerRequest::parseFromMongosCommand(BSON("controlBalancer"
                                                                                 << "stop")));

    ASSERT_EQ(ControlBalancerRequest::kStop, request.getAction());
}

TEST(ControlBalancerRequest, ParsingFromMongosCommandFailsForInvalidValue) {
    ASSERT_EQ(ErrorCodes::IllegalOperation,
              ControlBalancerRequest::parseFromMongosCommand(BSON("controlBalancer"
                                                                  << "invalid"))
                  .getStatus());
}

TEST(ControlBalancerRequest, ParsingFromMongosCommandFailsForInvalidType) {
    ASSERT_EQ(
        ErrorCodes::TypeMismatch,
        ControlBalancerRequest::parseFromMongosCommand(BSON("controlBalancer" << 1)).getStatus());
}

TEST(ControlBalancerRequest, ParseFromConfigCommandForStart) {
    auto request =
        assertGet(ControlBalancerRequest::parseFromConfigCommand(BSON("_configsvrControlBalancer"
                                                                      << "start")));

    ASSERT_EQ(ControlBalancerRequest::kStart, request.getAction());
}

TEST(ControlBalancerRequest, ParseFromConfigCommandForStop) {
    auto request =
        assertGet(ControlBalancerRequest::parseFromConfigCommand(BSON("_configsvrControlBalancer"
                                                                      << "stop")));

    ASSERT_EQ(ControlBalancerRequest::kStop, request.getAction());
}

TEST(ControlBalancerRequest, ParsingFromConfigCommandFailsForInvalidValue) {
    ASSERT_EQ(ErrorCodes::IllegalOperation,
              ControlBalancerRequest::parseFromConfigCommand(BSON("_configsvrControlBalancer"
                                                                  << "invalid"))
                  .getStatus());
}

TEST(ControlBalancerRequest, ParsingFromConfigCommandFailsForInvalidType) {
    ASSERT_EQ(ErrorCodes::TypeMismatch,
              ControlBalancerRequest::parseFromConfigCommand(BSON("_configsvrControlBalancer" << 1))
                  .getStatus());
}

TEST(ControlBalancerRequest, ConversionFromMongosCommandToConfigCommand) {
    auto mongosRequest =
        assertGet(ControlBalancerRequest::parseFromMongosCommand(BSON("controlBalancer"
                                                                      << "start")));

    auto configRequest = assertGet(
        ControlBalancerRequest::parseFromConfigCommand(mongosRequest.toCommandForConfig()));

    ASSERT_EQ(ControlBalancerRequest::kStart, configRequest.getAction());
}

}  // namespace
}  // namespace mongo
