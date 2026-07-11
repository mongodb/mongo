// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/transaction_coordinator_structures.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace txn {
using namespace std::literals::string_view_literals;
namespace {

constexpr auto kCommitDecision = "commit"sv;
constexpr auto kAbortDecision = "abort"sv;

}  // namespace

CommitDecision readCommitDecisionEnumProperty(std::string_view decision) {
    // clang-format off
    if (decision == kCommitDecision) return CommitDecision::kCommit;
    if (decision == kAbortDecision)  return CommitDecision::kAbort;
    // clang-format on

    uasserted(ErrorCodes::BadValue,
              str::stream() << "'" << decision << "' is not a valid decision");
}

std::string_view writeCommitDecisionEnumProperty(CommitDecision decision) {
    // clang-format off
    switch (decision) {
        case CommitDecision::kCommit:     return kCommitDecision;
        case CommitDecision::kAbort:      return kAbortDecision;
    };
    // clang-format on
    MONGO_UNREACHABLE;
}

std::string_view toString(PrepareVote prepareVote) {
    switch (prepareVote) {
        case txn::PrepareVote::kCommit:
            return "kCommit"sv;
        case txn::PrepareVote::kAbort:
            return "kAbort"sv;
    };
    MONGO_UNREACHABLE;
}

}  // namespace txn
}  // namespace mongo
