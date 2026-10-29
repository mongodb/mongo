// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/commit_quorum_options.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

const std::string_view CommitQuorumOptions::kCommitQuorumField = "commitQuorum"sv;
const char CommitQuorumOptions::kMajority[] = "majority";
const char CommitQuorumOptions::kVotingMembers[] = "votingMembers";

const BSONObj CommitQuorumOptions::Majority(BSON(kCommitQuorumField
                                                 << CommitQuorumOptions::kMajority));
const BSONObj CommitQuorumOptions::VotingMembers(BSON(kCommitQuorumField
                                                      << CommitQuorumOptions::kVotingMembers));

CommitQuorumOptions::CommitQuorumOptions(int numNodesOpts) {
    reset();
    numNodes = numNodesOpts;
    invariant(numNodes > 0 &&
              numNodes <= static_cast<decltype(numNodes)>(repl::ReplSetConfig::kMaxMembers));
}

CommitQuorumOptions::CommitQuorumOptions(const std::string& modeOpts) {
    reset();
    mode = modeOpts;
    invariant(!mode.empty());
}

Status CommitQuorumOptions::parse(const BSONElement& commitQuorumElement) {
    reset();

    if (commitQuorumElement.isNumber()) {
        auto cNumNodes = commitQuorumElement.safeNumberLong();
        if (cNumNodes < 0 ||
            cNumNodes > static_cast<decltype(cNumNodes)>(repl::ReplSetConfig::kMaxMembers)) {
            return Status(
                ErrorCodes::FailedToParse,
                str::stream()
                    << "commitQuorum has to be a non-negative number and not greater than "
                    << repl::ReplSetConfig::kMaxMembers);
        }
        // Implicitly upgrade commitQuorum of 0 to 1 as they have effectively the same semantics.
        numNodes = std::max(kPrimarySelfVote, static_cast<decltype(numNodes)>(cNumNodes));
    } else if (commitQuorumElement.type() == BSONType::string) {
        mode = commitQuorumElement.str();
        if (mode.empty()) {
            return Status(ErrorCodes::FailedToParse,
                          str::stream() << "commitQuorum can't be an empty string");
        }
    } else {
        return Status(ErrorCodes::FailedToParse, "commitQuorum has to be a number or a string");
    }

    return Status::OK();
}

CommitQuorumOptions CommitQuorumOptions::deserializerForIDL(
    const BSONElement& commitQuorumElement) {
    CommitQuorumOptions commitQuorumOptions;
    uassertStatusOK(commitQuorumOptions.parse(commitQuorumElement));
    return commitQuorumOptions;
}

BSONObj CommitQuorumOptions::toBSON() const {
    BSONObjBuilder builder;
    appendToBuilder(kCommitQuorumField, &builder);
    return builder.obj();
}

void CommitQuorumOptions::appendToBuilder(std::string_view fieldName,
                                          BSONObjBuilder* builder) const {
    if (mode.empty()) {
        builder->append(fieldName, numNodes);
    } else {
        builder->append(fieldName, mode);
    }
}

}  // namespace mongo
