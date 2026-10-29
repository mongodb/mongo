// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/time_proof_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/logical_time.h"
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
