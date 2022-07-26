/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace txn {
namespace {

TEST(CoordinatorCommitDecisionTest, SerializeCommitHasTimestampAndNoAbortStatus) {
    CoordinatorCommitDecision decision(CommitDecision::kCommit);
    decision.setCommitTimestamp(Timestamp(100, 200));

    auto obj = decision.toBSON();

    ASSERT_BSONOBJ_EQ(BSON("decision"
                           << "commit"
                           << "commitTimestamp" << Timestamp(100, 200)),
                      obj);
}

TEST(CoordinatorCommitDecisionTest, SerializeAbortHasNoTimestampAndAbortStatus) {
    CoordinatorCommitDecision decision(CommitDecision::kAbort);
    decision.setAbortStatus(Status(ErrorCodes::InternalError, "Test error"));

    auto obj = decision.toBSON();
    auto expectedObj = BSON("decision"
                            << "abort"
                            << "abortStatus"
                            << BSON("code" << 1 << "codeName"
                                           << "InternalError"
                                           << "errmsg"
                                           << "Test error"));

    ASSERT_BSONOBJ_EQ(expectedObj, obj);

    auto deserializedDecision =
        CoordinatorCommitDecision::parse(IDLParserContext("AbortTest"), expectedObj);
    ASSERT_BSONOBJ_EQ(obj, deserializedDecision.toBSON());
}

}  // namespace
}  // namespace txn
}  // namespace mongo
