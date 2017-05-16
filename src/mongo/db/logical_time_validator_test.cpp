/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

TEST(LogicalTimeValidator, GetTimeWithIncreasingTimes) {
    LogicalTimeValidator validator;

    LogicalTime t1(Timestamp(10, 0));
    auto newTime = validator.signLogicalTime(t1);

    ASSERT_EQ(t1.asTimestamp(), newTime.getTime().asTimestamp());
    ASSERT_TRUE(newTime.getProof());

    LogicalTime t2(Timestamp(20, 0));
    auto newTime2 = validator.signLogicalTime(t2);

    ASSERT_EQ(t2.asTimestamp(), newTime2.getTime().asTimestamp());
    ASSERT_TRUE(newTime2.getProof());
}

TEST(LogicalTimeValidator, ValidateReturnsOkForValidSignature) {
    LogicalTimeValidator validator;

    LogicalTime t1(Timestamp(20, 0));
    auto newTime = validator.signLogicalTime(t1);

    ASSERT_OK(validator.validate(newTime));
}

TEST(LogicalTimeValidator, ValidateErrorsOnInvalidTime) {
    LogicalTimeValidator validator;

    LogicalTime t1(Timestamp(20, 0));
    auto newTime = validator.signLogicalTime(t1);

    TimeProofService::TimeProof invalidProof = {{1, 2, 3}};
    SignedLogicalTime invalidTime(LogicalTime(Timestamp(30, 0)), invalidProof, 0);

    auto status = validator.validate(invalidTime);
    ASSERT_EQ(ErrorCodes::TimeProofMismatch, status);
}

}  // unnamed namespace
}  // namespace mongo
