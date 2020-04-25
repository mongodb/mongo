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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator_structures.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace txn {
namespace {

constexpr auto kCommitDecision = "commit"_sd;
constexpr auto kAbortDecision = "abort"_sd;

}  // namespace

CommitDecision readCommitDecisionEnumProperty(StringData decision) {
    // clang-format off
    if (decision == kCommitDecision) return CommitDecision::kCommit;
    if (decision == kAbortDecision)  return CommitDecision::kAbort;
    // clang-format on

    uasserted(ErrorCodes::BadValue,
              str::stream() << "'" << decision << "' is not a valid decision");
}

StringData writeCommitDecisionEnumProperty(CommitDecision decision) {
    // clang-format off
    switch (decision) {
        case CommitDecision::kCommit:     return kCommitDecision;
        case CommitDecision::kAbort:      return kAbortDecision;
    };
    // clang-format on
    MONGO_UNREACHABLE;
}

StringData toString(PrepareVote prepareVote) {
    switch (prepareVote) {
        case txn::PrepareVote::kCommit:
            return "kCommit"_sd;
        case txn::PrepareVote::kAbort:
            return "kAbort"_sd;
    };
    MONGO_UNREACHABLE;
}

Status readStatusProperty(const BSONElement& statusBSON) {
    return Status(ErrorCodes::Error(statusBSON["code"].Int()), statusBSON["errmsg"].String());
}

void writeStatusProperty(const Status& status, StringData fieldName, BSONObjBuilder* builder) {
    BSONObjBuilder statusBuilder(builder->subobjStart(fieldName));
    status.serializeErrorToBSON(&statusBuilder);
}

}  // namespace txn
}  // namespace mongo
