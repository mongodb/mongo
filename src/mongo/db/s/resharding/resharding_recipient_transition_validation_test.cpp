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

// What a recipient holds when it transitions to 'state'. Each state keeps what the states before it
// wrote. kError depends on the state it failed in, so it must be built explicitly.
RecipientTransitionSnapshot makeRecipientAt(RecipientStateEnum state,
                                            bool performVerification = false) {
    auto s = makeSnapshot(state, performVerification);
    switch (state) {
        case kDone:
        case kStrictConsistency:
        case kApplying:
            if (performVerification) {
                s.ctx.setNumDocumentsCloned(100);
                s.ctx.setTotalDocumentSize(1000);
                s.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 0};
            }
            [[fallthrough]];
        case kBuildingIndex:
        case kCloning:
        case kCreatingCollection:
            return withCloneDetails(std::move(s));
        case kError:
            uasserted(ErrorCodes::InternalError, "build kError explicitly");
        default:
            return s;
    }
}

// A recipient that skips cloning and applying records zero totals and never starts a monitor.
RecipientTransitionSnapshot makeSkippingApplying(RecipientStateEnum state) {
    auto s = makeRecipientAt(state, /*performVerification=*/true);
    s.options.setSkipCloningAndApplying(true);
    s.changeStreamsMonitor = boost::none;
    s.ctx.setNumDocumentsCloned(0);
    s.ctx.setTotalDocumentSize(0);
    s.approxDocumentsToCopy = 0;
    s.approxBytesToCopy = 0;
    return s;
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

RecipientTransitionSnapshot withCoordinatorAbort(RecipientTransitionSnapshot s) {
    s.ctx.setAbortReason(BSON("code" << static_cast<int>(ErrorCodes::ReshardCollectionAborted)
                                     << "errmsg"
                                     << "Received abort from the resharding coordinator"));
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

using SnapshotEdge = std::pair<RecipientTransitionSnapshot, RecipientTransitionSnapshot>;

// The forward edges that carry no abort reason, which is every forward edge but the one into kDone.
std::vector<SnapshotEdge> forwardEdgesBeforeDone(bool verify) {
    return {
        {makeSnapshot(kAwaitingFetchTimestamp, verify),
         makeRecipientAt(kCreatingCollection, verify)},
        {makeRecipientAt(kCreatingCollection, verify), makeRecipientAt(kCloning, verify)},
        {makeRecipientAt(kCloning, verify), makeRecipientAt(kBuildingIndex, verify)},
        {makeRecipientAt(kBuildingIndex, verify), makeRecipientAt(kApplying, verify)},
        {makeRecipientAt(kApplying, verify), makeRecipientAt(kStrictConsistency, verify)},
    };
}

TEST(ReshardingRecipientTransitionValidationTest, AcceptsEveryAllowedForwardEdge) {
    for (const bool verify : {false, true}) {
        auto edges = forwardEdgesBeforeDone(verify);
        edges.emplace_back(makeRecipientAt(kStrictConsistency, verify),
                           makeRecipientAt(kDone, verify));
        for (const auto& [before, after] : edges) {
            ASSERT_OK(validate(before, after).status) << edgeName(before, after);
        }
    }
}

TEST(ReshardingRecipientTransitionValidationTest, RejectedEdgeMessageNamesTheEdge) {
    const auto result =
        validate(makeRecipientAt(kCreatingCollection), makeRecipientAt(kBuildingIndex));
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

    const auto noMinFetch =
        withMinFetchTimestamp(makeRecipientAt(kCreatingCollection), 1, boost::none);
    assertViolated({"donorsHaveMinFetchTimestamp"}, validate(before, noMinFetch));

    // The clone timestamp is exactly the highest minFetchTimestamp: neither earlier nor later.
    auto tooEarly = makeRecipientAt(kCreatingCollection);
    tooEarly.cloneTimestamp = Timestamp(11, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, tooEarly));

    auto tooLate = makeRecipientAt(kCreatingCollection);
    tooLate.cloneTimestamp = Timestamp(13, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, tooLate));

    auto negative = makeRecipientAt(kCreatingCollection);
    negative.approxBytesToCopy = -1;
    assertViolated({"hasValidApproxCopyCounts"}, validate(before, negative));

    // No donors at all. A donor list that differs from the persisted one is reported by the
    // immutable-fields rule alone.
    assertViolated(
        {"hasDonorShards"},
        validate(withDonors(before, {}), withDonors(makeRecipientAt(kCreatingCollection), {})));
}

// The highest minFetchTimestamp may belong to any donor.
TEST(ReshardingRecipientTransitionValidationTest, CloneTimestampIsHighestWhicheverDonorHoldsIt) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);
    auto first = withMinFetchTimestamp(
        withMinFetchTimestamp(makeRecipientAt(kCreatingCollection), 0, Timestamp(12, 1)),
        1,
        Timestamp(10, 1));
    ASSERT_OK(validate(before, first).status);

    auto neither = first;
    neither.cloneTimestamp = Timestamp(11, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp"}, validate(before, neither));
}

TEST(ReshardingRecipientTransitionValidationTest, CloneDetailsAreWrittenExactlyOnce) {
    const auto after = makeRecipientAt(kCreatingCollection);
    // A recipient that already holds clone details before the edge that writes them.
    const auto cloned = withCloneDetails(makeSnapshot(kAwaitingFetchTimestamp));
    assertViolated({"cloneDetailsNotYetSet"}, validate(cloned, after));

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

TEST(ReshardingRecipientTransitionValidationTest, LaterEdgesRequireCloneDetailsToBeKept) {
    auto lost = makeRecipientAt(kCreatingCollection);
    lost.cloneTimestamp = boost::none;
    // Lost before the edge.
    assertViolated({"cloneDetailsUnchanged"},
                   validate(lost, withState(makeRecipientAt(kCreatingCollection), kCloning)));
    // Lost by the edge.
    assertViolated({"hasCloneTimestamp", "cloneDetailsUnchanged"},
                   validate(makeRecipientAt(kCreatingCollection), withState(lost, kCloning)));
    // Lost both before and by the edge: reported once.
    assertViolated({"hasCloneTimestamp"}, validate(lost, withState(lost, kCloning)));
}

// Every edge after the first carries the clone details forward, so each must require each detail on
// its own. Both snapshots lose the same detail, which leaves cloneDetailsUnchanged satisfied and
// only the presence rule to fire.
TEST(ReshardingRecipientTransitionValidationTest, LaterEdgesRequireEachCloneDetail) {
    struct CloneDetail {
        std::string name;
        std::string rule;
        std::function<void(RecipientTransitionSnapshot&)> lose;
    };
    const std::vector<CloneDetail> details = {
        {"cloneTimestamp",
         "hasCloneTimestamp",
         [](auto& s) {
             s.cloneTimestamp = boost::none;
         }},
        {"donor minFetchTimestamp",
         "donorsHaveMinFetchTimestamp",
         [](auto& s) {
             s = withMinFetchTimestamp(std::move(s), 0, boost::none);
         }},
        {"approxDocumentsToCopy",
         "hasValidApproxCopyCounts",
         [](auto& s) {
             s.approxDocumentsToCopy = boost::none;
         }},
        {"approxBytesToCopy",
         "hasValidApproxCopyCounts",
         [](auto& s) {
             s.approxBytesToCopy = boost::none;
         }},
        {"startConfigTxnCloneTime",
         "hasStartConfigTxnCloneTime",
         [](auto& s) {
             s.startConfigTxnCloneTime = boost::none;
         }},
    };

    for (const bool verify : {false, true}) {
        const auto edges = forwardEdgesBeforeDone(verify);
        for (size_t i = 1; i < edges.size(); ++i) {
            for (const auto& detail : details) {
                auto before = edges[i].first;
                auto after = edges[i].second;
                detail.lose(before);
                detail.lose(after);
                const auto result = validate(before, after);
                ASSERT_EQ(result.status.code(), ErrorCodes::ReshardingPreconditionFailure)
                    << edgeName(before, after) << " without " << detail.name;
                ASSERT_EQ(result.failed, std::vector<std::string>{detail.rule})
                    << edgeName(before, after) << " without " << detail.name << ": "
                    << result.status;
            }
        }
    }
}

TEST(ReshardingRecipientTransitionValidationTest, NullTimestampsAreNotSet) {
    const auto before = makeSnapshot(kAwaitingFetchTimestamp);

    auto nullCloneTimestamp = makeRecipientAt(kCreatingCollection);
    nullCloneTimestamp.cloneTimestamp = Timestamp();
    assertViolated({"hasCloneTimestamp"}, validate(before, nullCloneTimestamp));

    const auto nullMinFetch =
        withMinFetchTimestamp(makeRecipientAt(kCreatingCollection), 0, Timestamp());
    assertViolated({"donorsHaveMinFetchTimestamp"}, validate(before, nullMinFetch));

    // A null clone timestamp is not treated as covering a null minFetchTimestamp.
    auto allNull = makeRecipientAt(kCreatingCollection);
    allNull.cloneTimestamp = Timestamp();
    allNull.options.setDonorShards(
        {makeDonor("donor0", Timestamp()), makeDonor("donor1", Timestamp())});
    assertViolated({"hasCloneTimestamp", "donorsHaveMinFetchTimestamp"}, validate(before, allNull));
}

// A change to a clone detail, a donor's timestamp included, is reported by cloneDetailsUnchanged.
TEST(ReshardingRecipientTransitionValidationTest, CloneDetailsAreNeverChangedAfterTheyAreWritten) {
    const auto creating = makeRecipientAt(kCreatingCollection);
    const auto cloning = withState(creating, kCloning);

    auto otherTimestamp = cloning;
    otherTimestamp.cloneTimestamp = Timestamp(13, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp", "cloneDetailsUnchanged"},
                   validate(creating, otherTimestamp));

    auto otherDocuments = cloning;
    otherDocuments.approxDocumentsToCopy = 7;
    assertViolated({"cloneDetailsUnchanged"}, validate(creating, otherDocuments));

    auto otherBytes = cloning;
    otherBytes.approxBytesToCopy = 7;
    assertViolated({"cloneDetailsUnchanged"}, validate(creating, otherBytes));

    auto otherStart = cloning;
    otherStart.startConfigTxnCloneTime = Date_t::fromMillisSinceEpoch(2);
    assertViolated({"cloneDetailsUnchanged"}, validate(creating, otherStart));

    // A donor's timestamp changed: either donor, highest or not.
    assertViolated({"cloneDetailsUnchanged"},
                   validate(creating, withMinFetchTimestamp(cloning, 0, Timestamp(9, 1))));
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp", "cloneDetailsUnchanged"},
                   validate(creating, withMinFetchTimestamp(cloning, 1, Timestamp(9, 1))));

    // A donor's timestamp lost after being set.
    assertViolated({"donorsHaveMinFetchTimestamp", "cloneDetailsUnchanged"},
                   validate(creating, withMinFetchTimestamp(cloning, 0, boost::none)));

    // The same holds on the edges that follow.
    auto changedBuildingIndex = withState(creating, kBuildingIndex);
    changedBuildingIndex.approxBytesToCopy = 7;
    assertViolated({"cloneDetailsUnchanged"}, validate(cloning, changedBuildingIndex));

    auto changedApplying = makeRecipientAt(kApplying);
    changedApplying.startConfigTxnCloneTime = Date_t::fromMillisSinceEpoch(2);
    assertViolated({"cloneDetailsUnchanged"},
                   validate(makeRecipientAt(kBuildingIndex), changedApplying));
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

    const auto creating = makeRecipientAt(kCreatingCollection);
    auto changed = withState(creating, kCloning);
    changed.approxDocumentsToCopy = 7;
    ASSERT_STRING_CONTAINS(validate(creating, changed).status.reason(),
                           "cloneDetailsUnchanged (approxDocumentsToCopy changed from 100 to 7)");

    ASSERT_STRING_CONTAINS(
        validate(creating, withMinFetchTimestamp(withState(creating, kCloning), 0, Timestamp(9, 1)))
            .status.reason(),
        "cloneDetailsUnchanged (minFetchTimestamp of donor donor0 changed from Timestamp(10, 1) to "
        "Timestamp(9, 1))");
}

TEST(ReshardingRecipientTransitionValidationTest, ImmutableFieldsMustNotChange) {
    const auto before = withVerification(makeSnapshot(kAwaitingFetchTimestamp), true);

    // Dropping verification must not make the rules that depend on it optional.
    assertViolated({"immutableFieldsUnchanged"},
                   validate(before, makeRecipientAt(kCreatingCollection)));

    // Neither may skipCloningAndApplying be set.
    auto skipSet = makeRecipientAt(kCreatingCollection, /*performVerification=*/true);
    skipSet.options.setSkipCloningAndApplying(true);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, skipSet));

    // A mismatch is reported alone, since the other rules would be evaluated against the wrong
    // options.
    auto alsoBroken = makeRecipientAt(kCreatingCollection);
    alsoBroken.cloneTimestamp = Timestamp(99, 1);
    assertViolated({"immutableFieldsUnchanged"}, validate(before, alsoBroken));
    assertViolated({"immutableFieldsUnchanged"}, validate(before, makeSnapshot(kDone)));
}

TEST(ReshardingRecipientTransitionValidationTest, ImmutableFieldsCannotWaiveTheMonitor) {
    const auto before = makeRecipientAt(kBuildingIndex, /*performVerification=*/true);

    // Dropping verification must not make the monitor optional.
    assertViolated({"immutableFieldsUnchanged"}, validate(before, makeRecipientAt(kApplying)));

    // Nor may skipCloningAndApplying be set to waive it.
    auto skipSet = makeRecipientAt(kApplying, /*performVerification=*/true);
    skipSet.options.setSkipCloningAndApplying(true);
    skipSet.changeStreamsMonitor = boost::none;
    assertViolated({"immutableFieldsUnchanged"}, validate(before, skipSet));
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

TEST(ReshardingRecipientTransitionValidationTest, EnteringApplyingRequiresVerificationState) {
    // Before and proposed always share the immutable fields.
    const auto verifying = makeRecipientAt(kBuildingIndex, /*performVerification=*/true);
    const auto plain = makeRecipientAt(kBuildingIndex);

    auto noTotals = makeRecipientAt(kApplying, /*performVerification=*/true);
    noTotals.ctx.setNumDocumentsCloned(boost::none);
    assertViolated({"hasCloningTotalsWhenVerifying"}, validate(verifying, noTotals));

    auto noSize = makeRecipientAt(kApplying, /*performVerification=*/true);
    noSize.ctx.setTotalDocumentSize(boost::none);
    assertViolated({"hasCloningTotalsWhenVerifying"}, validate(verifying, noSize));

    auto negativeDocuments = makeRecipientAt(kApplying, /*performVerification=*/true);
    negativeDocuments.ctx.setNumDocumentsCloned(-1);
    assertViolated({"hasCloningTotalsWhenVerifying"}, validate(verifying, negativeDocuments));

    auto negativeSize = makeRecipientAt(kApplying, /*performVerification=*/true);
    negativeSize.ctx.setTotalDocumentSize(-1);
    assertViolated({"hasCloningTotalsWhenVerifying"}, validate(verifying, negativeSize));

    auto noMonitor = makeRecipientAt(kApplying, /*performVerification=*/true);
    noMonitor.changeStreamsMonitor = boost::none;
    assertViolated({"changeStreamsMonitorMatchesOptions"}, validate(verifying, noMonitor));

    auto unwantedMonitor = makeRecipientAt(kApplying);
    unwantedMonitor.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 0};
    assertViolated({"changeStreamsMonitorMatchesOptions"}, validate(plain, unwantedMonitor));

    auto dirtyMonitor = makeRecipientAt(kApplying, /*performVerification=*/true);
    dirtyMonitor.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 5};
    assertViolated({"changeStreamsMonitorStartsEmpty"}, validate(verifying, dirtyMonitor));

    // A null start would begin the monitor at the start of the oplog.
    auto nullStart = makeRecipientAt(kApplying, /*performVerification=*/true);
    nullStart.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(), 0};
    assertViolated({"changeStreamsMonitorHasStart"}, validate(verifying, nullStart));

    auto completedMonitor = makeRecipientAt(kApplying, /*performVerification=*/true);
    completedMonitor.changeStreamsMonitor->setCompleted(true);
    assertViolated({"changeStreamsMonitorStartsEmpty"}, validate(verifying, completedMonitor));

    // A resume token would skip events that the initial delta does not account for.
    auto resumedMonitor = makeRecipientAt(kApplying, /*performVerification=*/true);
    resumedMonitor.changeStreamsMonitor->setResumeToken(BSON("_data" << "8264"));
    assertViolated({"changeStreamsMonitorStartsEmpty"}, validate(verifying, resumedMonitor));

    auto monitorAlreadyStarted = makeRecipientAt(kBuildingIndex);
    monitorAlreadyStarted.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(9, 1), 0};
    assertViolated({"noChangeStreamsMonitorYet"},
                   validate(monitorAlreadyStarted, makeRecipientAt(kApplying)));
}

TEST(ReshardingRecipientTransitionValidationTest, StrictConsistencyKeepsMonitorStartAndTotals) {
    const auto applying = makeRecipientAt(kApplying, /*performVerification=*/true);

    auto otherStart = withState(applying, kStrictConsistency);
    otherStart.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(14, 1), 0};
    assertViolated({"changeStreamsMonitorStartUnchanged"}, validate(applying, otherStart));

    auto otherDocuments = withState(applying, kStrictConsistency);
    otherDocuments.ctx.setNumDocumentsCloned(99);
    assertViolated({"cloningTotalsUnchanged"}, validate(applying, otherDocuments));

    auto otherSize = withState(applying, kStrictConsistency);
    otherSize.ctx.setTotalDocumentSize(999);
    assertViolated({"cloningTotalsUnchanged"}, validate(applying, otherSize));

    auto otherDetails = withState(applying, kStrictConsistency);
    otherDetails.cloneTimestamp = Timestamp(13, 1);
    assertViolated({"cloneTimestampIsHighestMinFetchTimestamp", "cloneDetailsUnchanged"},
                   validate(applying, otherDetails));
}

TEST(ReshardingRecipientTransitionValidationTest, StrictConsistencyAllowsMonitorProgress) {
    const auto before = makeRecipientAt(kApplying, /*performVerification=*/true);
    auto after = withState(before, kStrictConsistency);
    after.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 42};
    ASSERT_OK(validate(before, after).status);

    // A resume token is progress too, and is rejected only on a new monitor. Completion is not
    // exercised here: the recipient's final event is written after it enters kStrictConsistency.
    after.changeStreamsMonitor->setResumeToken(BSON("_data" << "8264"));
    ASSERT_OK(validate(before, after).status);
}

TEST(ReshardingRecipientTransitionValidationTest, StrictConsistencyKeepsTheApplyingState) {
    const auto verifying = makeRecipientAt(kApplying, /*performVerification=*/true);

    auto lostMonitor = withState(verifying, kStrictConsistency);
    lostMonitor.changeStreamsMonitor = boost::none;
    assertViolated({"changeStreamsMonitorMatchesOptions", "changeStreamsMonitorStartUnchanged"},
                   validate(verifying, lostMonitor));

    auto lostTotals = withState(verifying, kStrictConsistency);
    lostTotals.ctx.setNumDocumentsCloned(boost::none);
    assertViolated({"hasCloningTotalsWhenVerifying", "cloningTotalsUnchanged"},
                   validate(verifying, lostTotals));

    auto lostSize = withState(verifying, kStrictConsistency);
    lostSize.ctx.setTotalDocumentSize(boost::none);
    assertViolated({"hasCloningTotalsWhenVerifying", "cloningTotalsUnchanged"},
                   validate(verifying, lostSize));
}

TEST(ReshardingRecipientTransitionValidationTest, DoneAfterAbortRecordsTheCoordinatorsAbort) {
    for (const bool verify : {false, true}) {
        const auto strict = makeRecipientAt(kStrictConsistency, verify);
        ASSERT_OK(validate(strict, withState(withCoordinatorAbort(strict), kDone)).status);

        assertViolated({"newAbortReasonIsCoordinatorAbort"},
                       validate(strict, withState(withAbortReason(strict), kDone)));
    }
}

// The coordinator commits without waiting for a monitor once its verification deadline passes.
TEST(ReshardingRecipientTransitionValidationTest, DoneDoesNotRequireACompletedMonitor) {
    const auto strict = makeRecipientAt(kStrictConsistency, /*performVerification=*/true);
    auto done = withState(strict, kDone);
    ASSERT_FALSE(done.changeStreamsMonitor->getCompleted());
    ASSERT_OK(validate(strict, done).status);

    done.changeStreamsMonitor->setCompleted(true);
    ASSERT_OK(validate(strict, done).status);
}

TEST(ReshardingRecipientTransitionValidationTest, DoneKeepsMonitorStart) {
    const auto strict = makeRecipientAt(kStrictConsistency, /*performVerification=*/true);

    auto otherStart = withState(strict, kDone);
    otherStart.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(14, 1), 0};
    assertViolated({"changeStreamsMonitorStartUnchanged"}, validate(strict, otherStart));

    // The monitor's progress is not compared.
    auto progressed = withState(strict, kDone);
    progressed.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 42};
    ASSERT_OK(validate(strict, progressed).status);
}

// kDone does not compare the totals: the service refreshes totalDocumentSize, totalNumDocuments and
// numOfIndexes from the temporary collection before it writes kDone, verifying or not.
TEST(ReshardingRecipientTransitionValidationTest, DoneAllowsRefreshedTotals) {
    for (const bool verify : {false, true}) {
        const auto strict = makeRecipientAt(kStrictConsistency, verify);
        auto done = withState(strict, kDone);
        done.ctx.setTotalDocumentSize(5000);
        done.ctx.setTotalNumDocuments(50);
        ASSERT_OK(validate(strict, done).status);
    }
}

TEST(ReshardingRecipientTransitionValidationTest, DoneKeepsCloneDetails) {
    const auto strict = makeRecipientAt(kStrictConsistency);
    auto changed = withState(strict, kDone);
    changed.approxBytesToCopy = 7;
    assertViolated({"cloneDetailsUnchanged"}, validate(strict, changed));
}

TEST(ReshardingRecipientTransitionValidationTest, SkipCloningAndApplyingRecipientHasNoMonitor) {
    const auto before = makeSkippingApplying(kBuildingIndex);
    const auto applying = makeSkippingApplying(kApplying);
    ASSERT_OK(validate(before, applying).status);
    ASSERT_OK(validate(applying, withState(applying, kStrictConsistency)).status);

    auto withMonitor = applying;
    withMonitor.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 0};
    assertViolated({"changeStreamsMonitorMatchesOptions"}, validate(before, withMonitor));

    auto noTotals = applying;
    noTotals.ctx.setNumDocumentsCloned(boost::none);
    assertViolated({"hasCloningTotalsWhenVerifying"}, validate(before, noTotals));
}

TEST(ReshardingRecipientTransitionValidationTest, SkipCloningAndApplyingWithoutVerification) {
    auto applying = makeRecipientAt(kApplying);
    applying.options.setSkipCloningAndApplying(true);
    applying.approxDocumentsToCopy = 0;
    applying.approxBytesToCopy = 0;
    ASSERT_OK(validate(withState(applying, kBuildingIndex), applying).status);
    ASSERT_OK(validate(applying, withState(applying, kStrictConsistency)).status);
}

TEST(ReshardingRecipientTransitionValidationTest, ForwardEdgesRejectAbortReason) {
    for (const bool verify : {false, true}) {
        for (const auto& [before, after] : forwardEdgesBeforeDone(verify)) {
            assertViolated({"noAbortReason"}, validate(before, withAbortReason(after)));
            assertViolated({"noAbortReason"}, validate(withAbortReason(before), after));
        }
    }
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionAcceptsInstalledState) {
    const auto proposed = makeRecipientAt(kCreatingCollection, /*performVerification=*/true);
    ASSERT_OK(validateRecipientPostTransition(
                  {.proposed = proposed, .inMemory = proposed.ctx, .durable = proposed})
                  .status);
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionIgnoresMonitorProgress) {
    const auto proposed = makeRecipientAt(kApplying, /*performVerification=*/true);
    ASSERT_OK(validateRecipientPostTransition(
                  {.proposed = proposed, .inMemory = proposed.ctx, .durable = proposed})
                  .status);

    // Progress persisted concurrently by the monitor is not a mismatch.
    auto durable = proposed;
    durable.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(13, 1), 7};
    durable.changeStreamsMonitor->setResumeToken(BSON("_data" << "8264"));
    ASSERT_OK(validateRecipientPostTransition(
                  {.proposed = proposed, .inMemory = proposed.ctx, .durable = durable})
                  .status);
}

TEST(ReshardingRecipientTransitionValidationTest,
     PostTransitionRejectsAMonitorThatStartedElsewhere) {
    const auto proposed = makeRecipientAt(kApplying, /*performVerification=*/true);

    auto otherStart = proposed;
    otherStart.changeStreamsMonitor = ChangeStreamsMonitorContext{Timestamp(14, 1), 0};
    const auto result = validateRecipientPostTransition(
        {.proposed = proposed, .inMemory = proposed.ctx, .durable = otherStart});
    assertViolated({"durableMatchesProposed"}, result, ErrorCodes::ReshardingPostconditionFailure);
    ASSERT_STRING_CONTAINS(result.status.reason(), "changeStreamsMonitor start");

    auto lostMonitor = proposed;
    lostMonitor.changeStreamsMonitor = boost::none;
    assertViolated({"durableMatchesProposed"},
                   validateRecipientPostTransition(
                       {.proposed = proposed, .inMemory = proposed.ctx, .durable = lostMonitor}),
                   ErrorCodes::ReshardingPostconditionFailure);
}

TEST(ReshardingRecipientTransitionValidationTest, PostTransitionRejectsMismatches) {
    const auto proposed = makeRecipientAt(kCreatingCollection, /*performVerification=*/true);

    auto staleDurable = makeRecipientAt(kAwaitingFetchTimestamp, /*performVerification=*/true);
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
    const auto proposed = makeRecipientAt(kCreatingCollection);
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
        {kCreatingCollection, kCloning},
        {kCloning, kBuildingIndex},
        {kBuildingIndex, kApplying},
        {kApplying, kStrictConsistency},
        {kStrictConsistency, kDone},
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
