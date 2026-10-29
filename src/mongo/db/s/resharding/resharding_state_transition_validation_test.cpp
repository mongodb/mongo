// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_state_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using enum DonorStateEnum;

DonorShardContext makeContext(DonorStateEnum state,
                              bool withProgress = false,
                              bool withAbortReason = false) {
    DonorShardContext ctx;
    ctx.setState(state);
    if (withProgress) {
        ctx.setMinFetchTimestamp(Timestamp(10, 5));
        ctx.setBytesToClone(100);
        ctx.setDocumentsToClone(10);
        ctx.setIndexCount(2);
    }
    if (withAbortReason) {
        ctx.setAbortReason(BSON("code" << 12345 << "errmsg" << "aborted for test"));
    }
    return ctx;
}

// A context whose clone progress fields are all populated.
DonorShardContext progressing(DonorStateEnum state) {
    return makeContext(state, /*withProgress=*/true);
}

// A context that has progressed and carries an abort reason.
DonorShardContext aborted(DonorStateEnum state) {
    return makeContext(state, /*withProgress=*/true, /*withAbortReason=*/true);
}

DonorShardContext withState(DonorShardContext ctx, DonorStateEnum state) {
    ctx.setState(state);
    return ctx;
}

ConstraintResult validate(const DonorShardContext& before, const DonorShardContext& after) {
    return validateDonorPreTransition({.before = before, .proposed = after});
}

ConstraintResult validateInstalled(const DonorShardContext& proposed,
                                   const DonorShardContext& inMemory,
                                   const DonorShardContext& durable) {
    return validateDonorPostTransition(
        {.proposed = proposed, .inMemory = inMemory, .durable = durable});
}

void assertViolated(const std::vector<std::string>& expected,
                    const ConstraintResult& result,
                    ErrorCodes::Error expectedCode = ErrorCodes::ReshardingPreconditionFailure) {
    ASSERT_EQ(result.status.code(), expectedCode);
    ASSERT_EQ(result.failed, expected);
}

TEST(ReshardingDonorTransitionValidationTest, AcceptsEveryAllowedEdge) {
    const auto preparingToDonate = makeContext(kPreparingToDonate);
    const auto donatingInitialData = progressing(kDonatingInitialData);
    const auto donatingOplogEntries = progressing(kDonatingOplogEntries);
    const auto preparingToBlockWrites = progressing(kPreparingToBlockWrites);
    const auto blockingWrites = progressing(kBlockingWrites);
    const auto error = aborted(kError);

    const std::vector<std::pair<DonorShardContext, DonorShardContext>> edges{
        // Forward edges.
        {preparingToDonate, donatingInitialData},
        {donatingInitialData, donatingOplogEntries},
        {donatingOplogEntries, preparingToBlockWrites},
        {preparingToBlockWrites, blockingWrites},
        // kError edges, including one before any progress was recorded.
        {makeContext(kPreparingToDonate), makeContext(kError, false, true)},
        {donatingInitialData, error},
        {donatingOplogEntries, error},
        {preparingToBlockWrites, error},
        // kDone edges, with and without an abort reason.
        {preparingToDonate, makeContext(kDone)},
        {preparingToDonate, makeContext(kDone, false, true)},
        {donatingInitialData, withState(donatingInitialData, kDone)},
        {donatingOplogEntries, withState(donatingOplogEntries, kDone)},
        {preparingToBlockWrites, withState(preparingToBlockWrites, kDone)},
        {blockingWrites, withState(blockingWrites, kDone)},
        {error, withState(error, kDone)},
    };
    for (const auto& [before, after] : edges) {
        ASSERT_OK(validate(before, after).status)
            << idl::serialize(before.getState()) << " -> " << idl::serialize(after.getState());
    }
}

TEST(ReshardingDonorTransitionValidationTest, RejectsDisallowedEdges) {
    const std::vector<std::pair<DonorShardContext, DonorShardContext>> edges{
        // Skipping or reversing states.
        {makeContext(kPreparingToDonate), progressing(kDonatingOplogEntries)},
        {progressing(kDonatingInitialData), progressing(kPreparingToBlockWrites)},
        {progressing(kDonatingOplogEntries), progressing(kDonatingInitialData)},
        // kError is only reachable before kBlockingWrites.
        {progressing(kBlockingWrites), aborted(kError)},
        // kError and kDone are terminal.
        {aborted(kError), aborted(kDonatingOplogEntries)},
        {progressing(kDone), aborted(kError)},
        // kUnused is not a real state.
        {makeContext(kUnused), progressing(kDonatingInitialData)},
        {makeContext(kPreparingToDonate), makeContext(kUnused)},
        // Self-edges.
        {makeContext(kUnused), makeContext(kUnused)},
        {makeContext(kPreparingToDonate), makeContext(kPreparingToDonate)},
        {progressing(kDonatingInitialData), progressing(kDonatingInitialData)},
        {progressing(kDonatingOplogEntries), progressing(kDonatingOplogEntries)},
        {progressing(kPreparingToBlockWrites), progressing(kPreparingToBlockWrites)},
        {aborted(kError), aborted(kError)},
        {progressing(kBlockingWrites), progressing(kBlockingWrites)},
        {progressing(kDone), progressing(kDone)},
    };
    for (const auto& [before, after] : edges) {
        assertViolated({"isAllowedEdge"}, validate(before, after));
    }
}

TEST(ReshardingDonorTransitionValidationTest, DonatingInitialDataRequiresMinFetchTimestamp) {
    const auto before = makeContext(kPreparingToDonate);

    auto missing = progressing(kDonatingInitialData);
    missing.setMinFetchTimestamp(boost::none);
    assertViolated({"hasMinFetchTimestamp"}, validate(before, missing));

    auto null = progressing(kDonatingInitialData);
    null.setMinFetchTimestamp(Timestamp());
    assertViolated({"hasMinFetchTimestamp"}, validate(before, null));
}

TEST(ReshardingDonorTransitionValidationTest, DonatingInitialDataRequiresValidCloneStats) {
    const auto before = makeContext(kPreparingToDonate);
    const auto valid = progressing(kDonatingInitialData);

    for (auto setter : {&DonorShardContext::setBytesToClone,
                        &DonorShardContext::setDocumentsToClone,
                        &DonorShardContext::setIndexCount}) {
        auto missing = valid;
        (missing.*setter)(boost::none);
        assertViolated({"hasValidCloneStats"}, validate(before, missing));

        auto negative = valid;
        (negative.*setter)(-1);
        assertViolated({"hasValidCloneStats"}, validate(before, negative));
    }
}

TEST(ReshardingDonorTransitionValidationTest, NonTerminalStatesForbidAbortReason) {
    const std::vector<std::pair<DonorShardContext, DonorShardContext>> edges{
        {makeContext(kPreparingToDonate), aborted(kDonatingInitialData)},
        {progressing(kDonatingInitialData), aborted(kDonatingOplogEntries)},
        {progressing(kDonatingOplogEntries), aborted(kPreparingToBlockWrites)},
        {progressing(kPreparingToBlockWrites), aborted(kBlockingWrites)},
    };
    for (const auto& [before, after] : edges) {
        assertViolated({"noAbortReason"}, validate(before, after));
    }
}

TEST(ReshardingDonorTransitionValidationTest, ErrorRequiresAbortReason) {
    const auto before = progressing(kDonatingInitialData);
    assertViolated({"hasAbortReason"}, validate(before, withState(before, kError)));
}

TEST(ReshardingDonorTransitionValidationTest, RequiresCloneDetailsUnchanged) {
    const auto before = progressing(kDonatingInitialData);

    auto changed = progressing(kDonatingOplogEntries);
    changed.setBytesToClone(999);
    assertViolated({"cloneDetailsUnchanged"}, validate(before, changed));

    auto lost = withState(before, kDone);
    lost.setMinFetchTimestamp(boost::none);
    assertViolated({"cloneDetailsUnchanged"}, validate(before, lost));

    auto changedOnError = aborted(kError);
    changedOnError.setDocumentsToClone(999);
    assertViolated({"cloneDetailsUnchanged"}, validate(before, changedOnError));
}

TEST(ReshardingDonorTransitionValidationTest, DoneKeepsExistingAbortReason) {
    const auto before = aborted(kError);

    auto dropped = withState(before, kDone);
    dropped.setAbortReason(boost::none);
    assertViolated({"existingAbortReasonUnchanged"}, validate(before, dropped));

    auto replaced = withState(before, kDone);
    replaced.setAbortReason(BSON("code" << 1 << "errmsg" << "different"));
    assertViolated({"existingAbortReasonUnchanged"}, validate(before, replaced));
}

TEST(ReshardingDonorTransitionValidationTest, ReportsAllViolationsTogether) {
    auto after =
        makeContext(kDonatingInitialData, /*withProgress=*/false, /*withAbortReason=*/true);
    after.setBytesToClone(-1);
    assertViolated({"hasMinFetchTimestamp", "hasValidCloneStats", "noAbortReason"},
                   validate(makeContext(kPreparingToDonate), after));
}

TEST(ReshardingDonorTransitionValidationTest, AcceptsInstalledAndDurableContexts) {
    const auto proposed = progressing(kDonatingOplogEntries);
    ASSERT_OK(validateInstalled(proposed, proposed, proposed).status);
}

TEST(ReshardingDonorTransitionValidationTest, RejectsMismatchingDurableContext) {
    const auto proposed = progressing(kDonatingOplogEntries);
    auto durable = proposed;
    durable.setBytesToClone(999);
    assertViolated({"durableMatchesProposed"},
                   validateInstalled(proposed, proposed, durable),
                   ErrorCodes::ReshardingPostconditionFailure);
}

TEST(ReshardingDonorTransitionValidationTest, RejectsMismatchingInMemoryContext) {
    const auto proposed = progressing(kDonatingOplogEntries);
    auto inMemory = proposed;
    inMemory.setDocumentsToClone(999);
    assertViolated({"inMemoryMatchesProposed"},
                   validateInstalled(proposed, inMemory, proposed),
                   ErrorCodes::ReshardingPostconditionFailure);
}

TEST(ReshardingDonorTransitionValidationTest, RejectsContextsThatStillShowThePreviousState) {
    const auto before = progressing(kDonatingInitialData);
    const auto proposed = withState(before, kDonatingOplogEntries);

    // Both contexts agree with each other, but neither agrees with the proposed state.
    assertViolated({"durableMatchesProposed", "inMemoryMatchesProposed"},
                   validateInstalled(proposed, before, before),
                   ErrorCodes::ReshardingPostconditionFailure);
}

TEST(ReshardingDonorTransitionValidationTest, ReportsOffendingValues) {
    auto missingTimestamp = progressing(kDonatingInitialData);
    missingTimestamp.setMinFetchTimestamp(boost::none);
    const auto missing = validate(makeContext(kPreparingToDonate), missingTimestamp);
    ASSERT_EQ(missing.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_STRING_CONTAINS(missing.status.reason(), "minFetchTimestamp is unset");
    ASSERT_STRING_CONTAINS(
        missing.status.reason(),
        "transition from preparing-to-donate to donating-initial-data violated:");

    auto negativeBytes = progressing(kDonatingInitialData);
    negativeBytes.setBytesToClone(-1);
    const auto negative = validate(makeContext(kPreparingToDonate), negativeBytes);
    ASSERT_EQ(negative.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_STRING_CONTAINS(negative.status.reason(), "bytesToClone is negative: -1");
}

TEST(ReshardingDonorTransitionValidationTest, ReportsEveryFailureInOrder) {
    struct Fake {
        bool first;
        bool second;
        bool third;
    };

    const auto constraint = [](std::string_view name, bool Fake::* flag) {
        return TransitionConstraint<Fake>{
            name, [name, flag](const Fake& fake) -> Status {
                return fake.*flag
                    ? Status::OK()
                    : Status{ErrorCodes::InternalError, str::stream() << name << " does not hold"};
            }};
    };
    const auto first = constraint("first", &Fake::first);
    const auto second = constraint("second", &Fake::second);
    const auto third = constraint("third", &Fake::third);

    const auto failed = TransitionCheck<Fake>("fake edge", Fake{false, true, false})
                            .require(first)
                            .require(second)
                            .require(third)
                            .result();
    ASSERT_EQ(failed.status.code(), ErrorCodes::InternalError);
    ASSERT_EQ(failed.failed, (std::vector<std::string>{"first", "third"}));
    ASSERT_STRING_CONTAINS(failed.status.reason(), "fake edge violated: first");
    ASSERT_STRING_CONTAINS(failed.status.reason(), "third (third does not hold)");

    const auto ok = TransitionCheck<Fake>("fake edge", Fake{true, true, true})
                        .require(first)
                        .require(second)
                        .require(third)
                        .result();
    ASSERT_OK(ok.status);
    ASSERT_TRUE(ok.failed.empty());
}

}  // namespace
}  // namespace mongo
