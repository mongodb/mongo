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
#include "mongo/db/logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using TimeProof = TimeProofService::TimeProof;

const TimeProofService::Key key = {};

// Verifies cluster time with proof signed with the correct key.
TEST(TimeProofService, VerifyLogicalTimeWithValidProof) {
    TimeProofService timeProofService;

    LogicalTime time(Timestamp(1));
    TimeProof proof = timeProofService.getProof(time, key);

    ASSERT_OK(timeProofService.checkProof(time, proof, key));
}

// Fails for cluster time with proof signed with an invalid key.
TEST(TimeProofService, LogicalTimeWithMismatchingProofShouldFail) {
    TimeProofService timeProofService;

    LogicalTime time(Timestamp(1));
    TimeProof invalidProof = {{1, 2, 3}};

    ASSERT_EQUALS(ErrorCodes::TimeProofMismatch,
                  timeProofService.checkProof(time, invalidProof, key));
}

TEST(TimeProofService, VerifyLogicalTimeProofCache) {
    TimeProofService timeProofService;

    LogicalTime time1(Timestamp(0x1111'2222'3333'0001));
    TimeProof proof1 = timeProofService.getProof(time1, key);
    ASSERT_OK(timeProofService.checkProof(time1, proof1, key));

    LogicalTime time2(Timestamp(0x1111'2222'3333'1FFF));
    TimeProof proof2 = timeProofService.getProof(time2, key);
    ASSERT_OK(timeProofService.checkProof(time2, proof2, key));
    ASSERT_OK(timeProofService.checkProof(time2, proof1, key));
    ASSERT_OK(timeProofService.checkProof(time1, proof2, key));
    ASSERT_EQUALS(proof1, proof2);

    LogicalTime time3(Timestamp(0x1111'2222'3334'0000));
    TimeProof proof3 = timeProofService.getProof(time3, key);
    ASSERT_OK(timeProofService.checkProof(time3, proof3, key));
    ASSERT_EQUALS(ErrorCodes::TimeProofMismatch, timeProofService.checkProof(time3, proof2, key));
}

}  // unnamed namespace
}  // namespace mongo
