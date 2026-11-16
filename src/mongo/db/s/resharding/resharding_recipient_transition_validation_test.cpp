// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_recipient_transition_validation.h"

#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using enum RecipientStateEnum;

RecipientTransitionSnapshot makeSnapshot(RecipientStateEnum state,
                                         bool performVerification = false) {
    RecipientTransitionSnapshot s{
        .options = ReshardingRecipientOptions{{}, /*minimumOperationDurationMillis=*/0}};
    s.ctx.setState(state);
    s.performVerification = performVerification;
    return s;
}

// Sets the donor shards of the snapshot: one per entry, with its minFetchTimestamp if there is one.
RecipientTransitionSnapshot withDonors(
    RecipientTransitionSnapshot s, const std::vector<boost::optional<Timestamp>>& fetchTimestamps) {
    std::vector<DonorShardFetchTimestamp> donors;
    for (size_t i = 0; i < fetchTimestamps.size(); ++i) {
        DonorShardFetchTimestamp donor{ShardId{"shard" + std::to_string(i)}};
        donor.setMinFetchTimestamp(fetchTimestamps[i]);
        donors.push_back(std::move(donor));
    }
    s.options.setDonorShards(std::move(donors));
    return s;
}

bool failedRule(const ConstraintResult& result, const std::string& rule) {
    return std::find(result.failed.begin(), result.failed.end(), rule) != result.failed.end();
}

ConstraintResult validate(const RecipientTransitionSnapshot& before,
                          const RecipientTransitionSnapshot& after) {
    return validateRecipientPreTransition({.before = before, .proposed = after});
}

void assertViolated(const std::vector<std::string>& expected,
                    const ConstraintResult& result,
                    ErrorCodes::Error expectedCode = ErrorCodes::ReshardingPreconditionFailure) {
    ASSERT_EQ(result.status.code(), expectedCode) << result.status;
    ASSERT_EQ(result.failed, expected) << result.status;
}

// Walks every (from, to) pair against the edge table, so a new state or edge needs a contract on
// purpose. The states come from the generated enum list, so a new enumerator is covered at once.
TEST(ReshardingRecipientTransitionValidationTest, OnlyTheDocumentedEdgesAreAllowed) {
    using Edge = std::pair<RecipientStateEnum, RecipientStateEnum>;
    const std::set<Edge> allowed{};

    for (const auto from : RecipientStateEnum_values) {
        for (const auto to : RecipientStateEnum_values) {
            const auto result = validate(makeSnapshot(from), makeSnapshot(to));
            const bool rejectedAsEdge =
                std::find(result.failed.begin(), result.failed.end(), "isAllowedEdge") !=
                result.failed.end();
            ASSERT_EQ(rejectedAsEdge, !allowed.contains({from, to}))
                << idl::serialize(from) << " -> " << idl::serialize(to);
            if (rejectedAsEdge) {
                // Nothing else is checked on an edge that does not exist.
                assertViolated({"isAllowedEdge"}, result);
            }
        }
    }
}

TEST(ReshardingRecipientTransitionValidationTest, RejectedEdgeMessageNamesTheEdge) {
    const auto result =
        validate(makeSnapshot(kAwaitingFetchTimestamp), makeSnapshot(kCreatingCollection));
    ASSERT_EQ(result.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_EQ(result.status.reason(),
              "transition from awaiting-fetch-timestamp to creating-collection is not allowed");
}

TEST(ReshardingRecipientTransitionValidationTest, ImmutableFieldsMustNotChange) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp, /*performVerification=*/true);

    // Dropping verification must not make the rules that depend on it optional.
    assertViolated({"immutableFieldsUnchanged"},
                   validate(before, makeSnapshot(kCreatingCollection)));

    // Neither may skipCloningAndApplying be set.
    auto skipSet = makeSnapshot(kCreatingCollection, /*performVerification=*/true);
    skipSet.options.setSkipCloningAndApplying(true);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, skipSet));

    // A mismatch is reported alone, as for the in-memory state: the other rules would be evaluated
    // against the wrong options, whether or not the edge exists.
    assertViolated({"immutableFieldsUnchanged"}, validate(before, makeSnapshot(kDone)));
}

// The donors' minFetchTimestamps are written by the transition out of kAwaitingFetchTimestamp and
// must stay afterwards. The edge validators enforce both, so the immutability rule has no opinion
// on them, whichever way they change.
TEST(ReshardingRecipientTransitionValidationTest, ImmutableFieldsIgnoreDonorFetchTimestamps) {
    const Timestamp t1{1, 1}, t2{2, 1};
    const auto unset =
        withDonors(makeSnapshot(kAwaitingFetchTimestamp), {boost::none, boost::none});
    const auto set = withDonors(makeSnapshot(kAwaitingFetchTimestamp), {t1, t2});

    // Unset to set, and in part.
    ASSERT_FALSE(
        failedRule(validate(unset, withDonors(makeSnapshot(kCreatingCollection), {t1, t2})),
                   "immutableFieldsUnchanged"));
    ASSERT_FALSE(failedRule(
        validate(unset, withDonors(makeSnapshot(kCreatingCollection), {t1, boost::none})),
        "immutableFieldsUnchanged"));

    // Set to another value, and to unset.
    ASSERT_FALSE(failedRule(validate(set, withDonors(makeSnapshot(kCreatingCollection), {t1, t1})),
                            "immutableFieldsUnchanged"));
    ASSERT_FALSE(
        failedRule(validate(set, withDonors(makeSnapshot(kCreatingCollection), {t1, boost::none})),
                   "immutableFieldsUnchanged"));
}

TEST(ReshardingRecipientTransitionValidationTest, DonorListAndOtherOptionsMustNotChange) {
    const Timestamp t1{1, 1};
    const auto before = withDonors(makeSnapshot(kAwaitingFetchTimestamp), {boost::none});

    // Donors are neither added nor removed, nor replaced.
    assertViolated({"immutableFieldsUnchanged"},
                   validate(before, withDonors(makeSnapshot(kCreatingCollection), {t1, t1})));
    assertViolated({"immutableFieldsUnchanged"},
                   validate(before, withDonors(makeSnapshot(kCreatingCollection), {})));
    auto renamed = withDonors(makeSnapshot(kCreatingCollection), {t1});
    auto donors = renamed.options.getDonorShards();
    donors[0].setShardId(ShardId{"other"});
    renamed.options.setDonorShards(donors);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, renamed));

    // Nor do they change places.
    const auto twoDonors = withDonors(makeSnapshot(kAwaitingFetchTimestamp), {boost::none, t1});
    auto swapped = withDonors(makeSnapshot(kCreatingCollection), {boost::none, t1});
    auto swappedDonors = swapped.options.getDonorShards();
    std::swap(swappedDonors[0], swappedDonors[1]);
    swapped.options.setDonorShards(swappedDonors);
    assertViolated({"immutableFieldsUnchanged"}, validate(twoDonors, swapped));

    // Neither is any other option, not only the ones that decide which rules apply.
    auto otherOption = withDonors(makeSnapshot(kCreatingCollection), {boost::none});
    otherOption.options.setOplogBatchTaskCount(4);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, otherOption));
}

TEST(ReshardingRecipientTransitionValidationTest, PersistedStateMustMatchInMemoryState) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);
    const auto proposed = makeSnapshot(kCreatingCollection);

    // Past the in-memory check, the edge is looked up.
    assertViolated(
        {"isAllowedEdge"},
        validateRecipientPreTransition(
            {.before = before, .proposed = proposed, .outgoingPhase = kAwaitingFetchTimestamp}));

    // Memory is ahead of disk: a lost write, or a deposed primary. Reported alone.
    assertViolated({"persistedStateMatchesInMemory"},
                   validateRecipientPreTransition({.before = makeSnapshot(kUnused),
                                                   .proposed = proposed,
                                                   .outgoingPhase = kAwaitingFetchTimestamp}));
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionAcceptsInstalledState) {
    const auto proposed = makeSnapshot(kCreatingCollection, /*performVerification=*/true);
    ASSERT_OK(validateRecipientPostTransition(
                  {.proposed = proposed, .inMemory = proposed.ctx, .durable = proposed})
                  .status);
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionRejectsMismatches) {
    const auto proposed = makeSnapshot(kCreatingCollection, /*performVerification=*/true);

    const auto staleDurable = makeSnapshot(kAwaitingFetchTimestamp, /*performVerification=*/true);
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = staleDurable}),
                   ErrorCodes::ReshardingPostconditionFailure);

    // The durable document must agree on the immutable options too.
    auto otherVerification = proposed;
    otherVerification.performVerification = false;
    assertViolated(
        {"durableMatchesProposed"},
        validateRecipientPostTransition(
            {.proposed = proposed, .inMemory = proposed.ctx, .durable = otherVerification}),
        ErrorCodes::ReshardingPostconditionFailure);

    // After the write the fetch timestamps are compared exactly: a durable document without them
    // is a lost write.
    const auto withTimestamps = withDonors(proposed, {Timestamp{1, 1}});
    assertViolated(
        {"durableMatchesProposed"},
        validateRecipientPostTransition({.proposed = withTimestamps,
                                         .inMemory = withTimestamps.ctx,
                                         .durable = withDonors(proposed, {boost::none})}),
        ErrorCodes::ReshardingPostconditionFailure);

    auto otherSkip = proposed;
    otherSkip.options.setSkipCloningAndApplying(true);
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = otherSkip}),
                   ErrorCodes::ReshardingPostconditionFailure);

    assertViolated(
        {"inMemoryMatchesProposed"},
        validateRecipientPostTransition({.proposed = proposed,
                                         .inMemory = makeSnapshot(kAwaitingFetchTimestamp).ctx,
                                         .durable = proposed}),
        ErrorCodes::ReshardingPostconditionFailure);
}

}  // namespace
}  // namespace mongo
