// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_recipient_transition_validation.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using enum RecipientStateEnum;

DonorShardFetchTimestamp makeDonor(std::string_view shard, boost::optional<Timestamp> minFetch) {
    DonorShardFetchTimestamp donor{ShardId(std::string{shard})};
    donor.setMinFetchTimestamp(minFetch);
    return donor;
}

// Two donors without a minFetchTimestamp, and no clone details yet.
RecipientTransitionSnapshot makeSnapshot(RecipientStateEnum state,
                                         bool performVerification = false) {
    RecipientTransitionSnapshot s{
        .options = ReshardingRecipientOptions{{}, /*minimumOperationDurationMillis=*/0}};
    s.ctx.setState(state);
    s.performVerification = performVerification;
    s.options.setDonorShards({makeDonor("donor0", boost::none), makeDonor("donor1", boost::none)});
    return s;
}

RecipientTransitionSnapshot withCloneDetails(RecipientTransitionSnapshot s) {
    s.options.setDonorShards(
        {makeDonor("donor0", Timestamp(10, 1)), makeDonor("donor1", Timestamp(12, 1))});
    s.cloneTimestamp = Timestamp(12, 1);
    s.approxDocumentsToCopy = 100;
    s.approxBytesToCopy = 1000;
    s.startConfigTxnCloneTime = Date_t::fromMillisSinceEpoch(1);
    return s;
}

RecipientTransitionSnapshot makeCloned(RecipientStateEnum state, bool performVerification = false) {
    return withCloneDetails(makeSnapshot(state, performVerification));
}

// Sets one donor's minFetchTimestamp and keeps the donor list as it is.
RecipientTransitionSnapshot withMinFetchTimestamp(RecipientTransitionSnapshot s,
                                                  size_t donor,
                                                  boost::optional<Timestamp> minFetch) {
    auto donors = s.options.getDonorShards();
    donors.at(donor).setMinFetchTimestamp(minFetch);
    s.options.setDonorShards(std::move(donors));
    return s;
}

// Replaces the donor shards: one per entry, with its minFetchTimestamp if there is one.
RecipientTransitionSnapshot withDonors(
    RecipientTransitionSnapshot s, const std::vector<boost::optional<Timestamp>>& fetchTimestamps) {
    std::vector<DonorShardFetchTimestamp> donors;
    for (size_t i = 0; i < fetchTimestamps.size(); ++i) {
        donors.push_back(makeDonor("shard" + std::to_string(i), fetchTimestamps[i]));
    }
    s.options.setDonorShards(std::move(donors));
    return s;
}

RecipientTransitionSnapshot withAbortReason(RecipientTransitionSnapshot s) {
    s.ctx.setAbortReason(BSON("code" << 12345 << "errmsg" << "aborted for test"));
    return s;
}

RecipientTransitionSnapshot withVerification(RecipientTransitionSnapshot s,
                                             bool performVerification) {
    s.performVerification = performVerification;
    return s;
}

RecipientTransitionSnapshot withState(RecipientTransitionSnapshot s, RecipientStateEnum state) {
    s.ctx.setState(state);
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

std::string edgeName(const RecipientTransitionSnapshot& before,
                     const RecipientTransitionSnapshot& after) {
    return str::stream() << idl::serialize(before.ctx.getState()) << " -> "
                         << idl::serialize(after.ctx.getState());
}

TEST(ReshardingRecipientTransitionValidationTest, AcceptsEveryAllowedForwardEdge) {
    for (const bool verify : {false, true}) {
        const auto before = withVerification(makeSnapshot(kAwaitingFetchTimestamp), verify);
        const auto after = makeCloned(kCreatingCollection, verify);
        ASSERT_OK(validate(before, after).status) << edgeName(before, after);
    }
}

TEST(ReshardingRecipientTransitionValidationTest, RejectedEdgeMessageNamesTheEdge) {
    const auto result = validate(makeCloned(kCreatingCollection), makeCloned(kBuildingIndex));
    ASSERT_EQ(result.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_EQ(result.status.reason(),
              "transition from creating-collection to building-index is not allowed");
}

TEST(ReshardingRecipientTransitionValidationTest, EnteringCreatingCollectionRequiresCloneDetails) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);

    // The clone details were not written at all.
    assertViolated({"hasCloneTimestamp",
                    "donorsHaveMinFetchTimestamp",
                    "hasValidApproxCopyCounts",
                    "hasStartConfigTxnCloneTime"},
                   validate(before, withState(before, kCreatingCollection)));

    const auto noMinFetch = withMinFetchTimestamp(makeCloned(kCreatingCollection), 1, boost::none);
    assertViolated({"donorsHaveMinFetchTimestamp"}, validate(before, noMinFetch));

    // The clone timestamp is exactly the highest minFetchTimestamp: neither earlier nor later.
    auto tooEarly = makeCloned(kCreatingCollection);
    tooEarly.cloneTimestamp = Timestamp(11, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, tooEarly));

    auto tooLate = makeCloned(kCreatingCollection);
    tooLate.cloneTimestamp = Timestamp(13, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, tooLate));

    auto negative = makeCloned(kCreatingCollection);
    negative.approxBytesToCopy = -1;
    assertViolated({"hasValidApproxCopyCounts"}, validate(before, negative));

    // No donors at all. A donor list that differs from the persisted one is reported by the
    // immutable-fields rule alone.
    assertViolated(
        {"hasDonorShards"},
        validate(withDonors(before, {}), withDonors(makeCloned(kCreatingCollection), {})));
}

// The highest minFetchTimestamp may belong to any donor.
TEST(ReshardingRecipientTransitionValidationTest, CloneTimestampIsHighestWhicheverDonorHoldsIt) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);
    auto first = withMinFetchTimestamp(
        withMinFetchTimestamp(makeCloned(kCreatingCollection), 0, Timestamp(12, 1)),
        1,
        Timestamp(10, 1));
    ASSERT_OK(validate(before, first).status);

    auto neither = first;
    neither.cloneTimestamp = Timestamp(11, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, neither));
}

TEST(ReshardingRecipientTransitionValidationTest, CloneDetailsAreWrittenExactlyOnce) {
    const auto after = makeCloned(kCreatingCollection);
    assertViolated({"cloneDetailsNotYetSet"}, validate(makeCloned(kAwaitingFetchTimestamp), after));

    const auto cloned = makeCloned(kAwaitingFetchTimestamp);
    const std::vector<std::pair<std::string, std::function<void(RecipientTransitionSnapshot&)>>>
        alreadySet{
            {"cloneTimestamp",
             [&](auto& s) {
                 s.cloneTimestamp = cloned.cloneTimestamp;
             }},
            {"approxDocumentsToCopy",
             [&](auto& s) {
                 s.approxDocumentsToCopy = cloned.approxDocumentsToCopy;
             }},
            {"approxBytesToCopy",
             [&](auto& s) {
                 s.approxBytesToCopy = cloned.approxBytesToCopy;
             }},
            {"startConfigTxnCloneTime",
             [&](auto& s) {
                 s.startConfigTxnCloneTime = cloned.startConfigTxnCloneTime;
             }},
        };
    for (const auto& [field, setField] : alreadySet) {
        auto before = makeSnapshot(kAwaitingFetchTimestamp);
        setField(before);
        const auto result = validate(before, after);
        assertViolated({"cloneDetailsNotYetSet"}, result);
        ASSERT_STRING_CONTAINS(result.status.reason(), field);
    }

    // A null clone timestamp counts as unset.
    auto nullCloneTimestamp = makeSnapshot(kAwaitingFetchTimestamp);
    nullCloneTimestamp.cloneTimestamp = Timestamp();
    ASSERT_OK(validate(nullCloneTimestamp, after).status);

    // The donors' minFetchTimestamps may already be present when the document is created.
    ASSERT_OK(
        validate(withMinFetchTimestamp(makeSnapshot(kAwaitingFetchTimestamp), 0, Timestamp(10, 1)),
                 after)
            .status);
}

TEST(ReshardingRecipientTransitionValidationTest, NullTimestampsAreNotSet) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);

    auto nullCloneTimestamp = makeCloned(kCreatingCollection);
    nullCloneTimestamp.cloneTimestamp = Timestamp();
    assertViolated({"hasCloneTimestamp"}, validate(before, nullCloneTimestamp));

    const auto nullMinFetch =
        withMinFetchTimestamp(makeCloned(kCreatingCollection), 0, Timestamp());
    assertViolated({"donorsHaveMinFetchTimestamp"}, validate(before, nullMinFetch));

    // A null clone timestamp is not treated as covering a null minFetchTimestamp.
    auto allNull = makeCloned(kCreatingCollection);
    allNull.cloneTimestamp = Timestamp();
    allNull.options.setDonorShards(
        {makeDonor("donor0", Timestamp()), makeDonor("donor1", Timestamp())});
    assertViolated({"hasCloneTimestamp", "donorsHaveMinFetchTimestamp"}, validate(before, allNull));
}

TEST(ReshardingRecipientTransitionValidationTest, MessageNamesEdgeEveryUnmetRuleAndItsValues) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);
    const auto result = validate(before, withState(before, kCreatingCollection));
    ASSERT_EQ(result.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_STRING_CONTAINS(
        result.status.reason(),
        "transition from awaiting-fetch-timestamp to creating-collection violated: "
        "hasCloneTimestamp (cloneTimestamp is unset), donorsHaveMinFetchTimestamp (");
    ASSERT_STRING_CONTAINS(result.status.reason(), "minFetchTimestamp of donor donor0 is unset");
}

TEST(ReshardingRecipientTransitionValidationTest, ImmutableFieldsMustNotChange) {
    const auto before = withVerification(makeSnapshot(kAwaitingFetchTimestamp), true);

    // Dropping verification must not make the rules that depend on it optional.
    assertViolated({"immutableFieldsUnchanged"}, validate(before, makeCloned(kCreatingCollection)));

    // Neither may skipCloningAndApplying be set.
    auto skipSet = makeCloned(kCreatingCollection, /*performVerification=*/true);
    skipSet.options.setSkipCloningAndApplying(true);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, skipSet));

    // A mismatch is reported alone, since the other rules would be evaluated against the wrong
    // options.
    auto alsoBroken = makeCloned(kCreatingCollection);
    alsoBroken.cloneTimestamp = Timestamp(99, 1);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, alsoBroken));
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

TEST(ReshardingRecipientTransitionValidationTest, ForwardEdgesRejectAbortReason) {
    assertViolated({"noAbortReason"},
                   validate(makeSnapshot(kAwaitingFetchTimestamp),
                            withAbortReason(makeCloned(kCreatingCollection))));
    assertViolated({"noAbortReason"},
                   validate(withAbortReason(makeSnapshot(kAwaitingFetchTimestamp)),
                            makeCloned(kCreatingCollection)));
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionAcceptsInstalledState) {
    const auto proposed = makeCloned(kCreatingCollection, /*performVerification=*/true);
    ASSERT_OK(validateRecipientPostTransition(
                  {.proposed = proposed, .inMemory = proposed.ctx, .durable = proposed})
                  .status);
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionRejectsMismatches) {
    const auto proposed = makeCloned(kCreatingCollection, /*performVerification=*/true);

    auto staleDurable = makeCloned(kAwaitingFetchTimestamp, /*performVerification=*/true);
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = staleDurable}),
                   ErrorCodes::ReshardingPostconditionFailure);

    // Each clone detail must agree, and the message names the one that does not.
    std::vector<std::pair<std::string, RecipientTransitionSnapshot>> otherCloneDetails;
    otherCloneDetails.emplace_back("cloneTimestamp", proposed);
    otherCloneDetails.back().second.cloneTimestamp = Timestamp(1, 1);
    otherCloneDetails.emplace_back("approxDocumentsToCopy", proposed);
    otherCloneDetails.back().second.approxDocumentsToCopy = 1;
    otherCloneDetails.emplace_back("approxBytesToCopy", proposed);
    otherCloneDetails.back().second.approxBytesToCopy = 1;
    otherCloneDetails.emplace_back("startConfigTxnCloneTime", proposed);
    otherCloneDetails.back().second.startConfigTxnCloneTime = Date_t::fromMillisSinceEpoch(2);
    otherCloneDetails.emplace_back("minFetchTimestamp of donor donor1", proposed);
    otherCloneDetails.back().second = withMinFetchTimestamp(proposed, 1, Timestamp(11, 1));
    for (const auto& [field, durable] : otherCloneDetails) {
        const auto result = validateRecipientPostTransition(
            {.proposed = proposed, .inMemory = proposed.ctx, .durable = durable});
        assertViolated(
            {"durableMatchesProposed"}, result, ErrorCodes::ReshardingPostconditionFailure);
        ASSERT_STRING_CONTAINS(result.status.reason(), field);
    }

    // The durable document must agree on the immutable fields too.
    auto otherVerification = proposed;
    otherVerification.performVerification = false;
    assertViolated(
        {"durableMatchesProposed"},
        validateRecipientPostTransition(
            {.proposed = proposed, .inMemory = proposed.ctx, .durable = otherVerification}),
        ErrorCodes::ReshardingPostconditionFailure);

    // A durable document that lacks a timestamp present in the proposal.
    const auto lostMinFetch = withMinFetchTimestamp(proposed, 1, boost::none);
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = lostMinFetch}),
                   ErrorCodes::ReshardingPostconditionFailure);

    auto otherSkip = proposed;
    otherSkip.options.setSkipCloningAndApplying(true);
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = otherSkip}),
                   ErrorCodes::ReshardingPostconditionFailure);

    assertViolated(
        {"inMemoryMatchesProposed"},
        validateRecipientPostTransition(
            {.proposed = proposed, .inMemory = makeSnapshot(kCloning).ctx, .durable = proposed}),
        ErrorCodes::ReshardingPostconditionFailure);
}

TEST(ReshardingRecipientTransitionValidationTest, PersistedStateMustMatchInMemoryState) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);
    const auto proposed = makeCloned(kCreatingCollection);
    ASSERT_OK(
        validateRecipientPreTransition(
            {.before = before, .proposed = proposed, .outgoingPhase = kAwaitingFetchTimestamp})
            .status);

    // The in-memory state is ahead of the persisted state.
    assertViolated({"persistedStateMatchesInMemory"},
                   validateRecipientPreTransition({.before = makeSnapshot(kUnused),
                                                   .proposed = proposed,
                                                   .outgoingPhase = kAwaitingFetchTimestamp}));
}

// Walks every (from, to) pair against the edge table, so a new state or edge needs a contract on
// purpose. The states come from the generated enum list, so a new enumerator is covered at once.
TEST(ReshardingRecipientTransitionValidationTest, OnlyTheDocumentedEdgesAreAllowed) {
    using Edge = std::pair<RecipientStateEnum, RecipientStateEnum>;
    const std::set<Edge> allowed{
        {kAwaitingFetchTimestamp, kCreatingCollection},
    };

    for (const auto from : RecipientStateEnum_values) {
        for (const auto to : RecipientStateEnum_values) {
            const auto result = validate(makeSnapshot(from), makeSnapshot(to));
            const bool rejectedAsEdge = failedRule(result, "isAllowedEdge");
            ASSERT_EQ(rejectedAsEdge, !allowed.contains({from, to}))
                << idl::serialize(from) << " -> " << idl::serialize(to);
            if (rejectedAsEdge) {
                // Nothing else is checked on an edge that does not exist.
                assertViolated({"isAllowedEdge"}, result);
            }
        }
    }
}

}  // namespace
}  // namespace mongo
