// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/transaction_coordinator_structures.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace txn {
namespace {

TEST(CoordinatorCommitDecisionTest, SerializeCommitHasTimestampAndNoAbortStatus) {
    CoordinatorCommitDecision decision(CommitDecision::kCommit);
    decision.setCommitTimestamp(Timestamp(100, 200));

    auto obj = decision.toBSON();

    ASSERT_BSONOBJ_EQ(BSON("decision" << "commit"
                                      << "commitTimestamp" << Timestamp(100, 200)),
                      obj);
}

TEST(CoordinatorCommitDecisionTest, SerializeAbortHasNoTimestampAndAbortStatus) {
    CoordinatorCommitDecision decision(CommitDecision::kAbort);
    decision.setAbortStatus(Status(ErrorCodes::InternalError, "Test error"));

    auto obj = decision.toBSON();
    auto expectedObj = BSON("decision" << "abort"
                                       << "abortStatus"
                                       << BSON("code" << 1 << "codeName"
                                                      << "InternalError"
                                                      << "errmsg"
                                                      << "Test error"));

    ASSERT_BSONOBJ_EQ(expectedObj, obj);

    auto deserializedDecision =
        CoordinatorCommitDecision::parse(expectedObj, IDLParserContext("AbortTest"));
    ASSERT_BSONOBJ_EQ(obj, deserializedDecision.toBSON());
}

}  // namespace
}  // namespace txn
}  // namespace mongo
