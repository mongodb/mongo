// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_transition_validation.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using enum CoordinatorStateEnum;

const UUID kReshardingUUID = UUID::gen();
const UUID kSourceUUID = UUID::gen();
const NamespaceString kSourceNss = NamespaceString::createNamespaceString_forTest("db.coll");
const NamespaceString kOtherNss = NamespaceString::createNamespaceString_forTest("db.other");
const NamespaceString kTempNss =
    resharding::constructTemporaryReshardingNss(kSourceNss, kSourceUUID);
const KeyPattern kNewKey{BSON("newKey" << 1)};
const BSONObj kAbortReason = BSON("code" << ErrorCodes::InternalError << "errmsg" << "test");
const Date_t kCriticalSectionExpiresAt = Date_t::fromMillisSinceEpoch(1000000);
const Date_t kQuiescePeriodEnd = Date_t::fromMillisSinceEpoch(2000000);

CoordinatorTransitionFacts factsFor(CoordinatorStateEnum to) {
    return {.canEnterCritical = to == kBlockingWrites};
}

DonorShardContext makeDonorContext(DonorStateEnum state) {
    DonorShardContext ctx;
    ctx.setState(state);
    return ctx;
}

RecipientShardContext makeRecipientContext(RecipientStateEnum state) {
    RecipientShardContext ctx;
    ctx.setState(state);
    return ctx;
}

ReshardingCoordinatorDocument makeDoc(CoordinatorStateEnum state,
                                      std::vector<DonorShardEntry> donors,
                                      std::vector<RecipientShardEntry> recipients) {
    ReshardingCoordinatorDocument doc(state, std::move(donors), std::move(recipients));
    doc.setCommonReshardingMetadata(
        CommonReshardingMetadata(kReshardingUUID, kSourceNss, kSourceUUID, kTempNss, kNewKey));
    return doc;
}

/**
 * The happy-path document for `state`. Fields are added cumulatively as the operation progresses.
 */
ReshardingCoordinatorDocument docAt(CoordinatorStateEnum state) {
    if (state == kUnused || state == kInitializing) {
        return makeDoc(state, {}, {});
    }

    const bool allDone = state == kQuiesced || state == kDone;
    const bool reachedCloning =
        state == kCloning || state == kApplying || state == kBlockingWrites || state == kCommitting;
    const bool progressed = reachedCloning || state == kAborting;

    std::vector<DonorShardEntry> donors;
    std::vector<RecipientShardEntry> recipients;
    for (int i = 0; i < 2; ++i) {
        auto donorCtx = makeDonorContext(DonorStateEnum::kUnused);
        auto recipientCtx = makeRecipientContext(RecipientStateEnum::kUnused);
        if (allDone) {
            donorCtx.setState(DonorStateEnum::kDone);
            recipientCtx.setState(RecipientStateEnum::kDone);
        } else if (progressed) {
            donorCtx.setState(DonorStateEnum::kDonatingInitialData);
            donorCtx.setMinFetchTimestamp(Timestamp(10, i + 1));
            if (state != kCloning) {
                recipientCtx.setState(state == kCommitting ? RecipientStateEnum::kStrictConsistency
                                                           : RecipientStateEnum::kApplying);
            }
        }
        donors.emplace_back(ShardId("donor" + std::to_string(i)), donorCtx);
        recipients.emplace_back(ShardId("recipient" + std::to_string(i)), recipientCtx);
    }

    auto doc = makeDoc(state, std::move(donors), std::move(recipients));
    if (progressed) {
        doc.setCloneTimestamp(Timestamp(10, 2));
        doc.setApproxBytesToCopy(1000);
        doc.setApproxDocumentsToCopy(100);
    }
    if (state == kBlockingWrites || state == kCommitting) {
        doc.setCriticalSectionExpiresAt(kCriticalSectionExpiresAt);
    }
    if (state == kAborting) {
        doc.setAbortReason(kAbortReason);
    }
    if (state == kQuiesced) {
        doc.setUserReshardingUUID(UUID::gen());
        doc.setQuiescePeriodEnd(kQuiescePeriodEnd);
    }
    return doc;
}

ReshardingCoordinatorDocument withState(const ReshardingCoordinatorDocument& doc,
                                        CoordinatorStateEnum state) {
    auto copy = doc;
    copy.setState(state);
    return copy;
}

ReshardingCoordinatorDocument aborted(const ReshardingCoordinatorDocument& doc) {
    auto copy = doc;
    copy.setState(kAborting);
    copy.setAbortReason(kAbortReason);
    return copy;
}

ReshardingCoordinatorDocument withParticipantsDone(ReshardingCoordinatorDocument doc) {
    for (auto& donor : doc.getDonorShards()) {
        donor.getMutableState().setState(DonorStateEnum::kDone);
    }
    for (auto& recipient : doc.getRecipientShards()) {
        recipient.getMutableState().setState(RecipientStateEnum::kDone);
    }
    return doc;
}

ConstraintResult validate(const ReshardingCoordinatorDocument& before,
                          const ReshardingCoordinatorDocument& proposed,
                          CoordinatorTransitionFacts facts = {}) {
    return validateCoordinatorPreTransition(
        {.before = before, .proposed = proposed, .facts = facts});
}

using DocPair = std::pair<ReshardingCoordinatorDocument, ReshardingCoordinatorDocument>;

/**
 * A success exit from committing. The user UUID is part of the immutable metadata, so a quiesced
 * exit carries it from the start. Quiescing also sets the quiesce period end.
 */
DocPair successExitPair(CoordinatorStateEnum to) {
    auto before = docAt(kCommitting);
    if (to == kQuiesced) {
        before.setUserReshardingUUID(UUID::gen());
    }
    auto proposed = withState(withParticipantsDone(before), to);
    if (to == kQuiesced) {
        proposed.setQuiescePeriodEnd(kQuiescePeriodEnd);
    }
    return {before, proposed};
}

/**
 * An abort exit. Quiesced also needs the user UUID and quiesce period end.
 */
DocPair abortExitPair(CoordinatorStateEnum from, CoordinatorStateEnum to) {
    auto before = docAt(from);
    if (from != kInitializing) {
        before.setAbortReason(kAbortReason);
    }
    if (to == kQuiesced) {
        before.setUserReshardingUUID(UUID::gen());
    }
    auto proposed = withState(before, to);
    if (from == kInitializing) {
        proposed.setAbortReason(kAbortReason);
    }
    if (to == kQuiesced) {
        proposed.setQuiescePeriodEnd(kQuiescePeriodEnd);
    }
    return {before, proposed};
}

std::vector<DocPair> forwardEdges() {
    std::vector<DocPair> edges;
    edges.emplace_back(docAt(kUnused), docAt(kInitializing));
    edges.emplace_back(docAt(kInitializing), docAt(kPreparingToDonate));
    edges.emplace_back(docAt(kPreparingToDonate), docAt(kCloning));
    edges.emplace_back(docAt(kCloning), docAt(kApplying));
    edges.emplace_back(docAt(kApplying), docAt(kBlockingWrites));
    edges.emplace_back(docAt(kBlockingWrites), docAt(kCommitting));
    return edges;
}

std::vector<DocPair> validEdges() {
    auto edges = forwardEdges();

    for (auto state : {kPreparingToDonate, kCloning, kApplying, kBlockingWrites}) {
        edges.emplace_back(docAt(state), aborted(docAt(state)));
    }

    for (auto to : {kQuiesced, kDone}) {
        edges.push_back(successExitPair(to));
    }
    for (auto from : {kInitializing, kAborting}) {
        for (auto to : {kQuiesced, kDone}) {
            edges.push_back(abortExitPair(from, to));
        }
    }

    const auto quiesced = docAt(kQuiesced);
    edges.emplace_back(quiesced, withState(quiesced, kDone));

    return edges;
}

void assertViolated(const std::vector<std::string>& expected,
                    const ConstraintResult& result,
                    ErrorCodes::Error expectedCode = ErrorCodes::ReshardingPreconditionFailure) {
    ASSERT_EQ(result.status.code(), expectedCode);
    ASSERT_EQ(result.failed, expected);
}

TEST(ReshardingCoordinatorTransitionValidationTest, AcceptsEveryAllowedEdge) {
    for (const auto& [before, proposed] : validEdges()) {
        ASSERT_OK(validate(before, proposed, factsFor(proposed.getState())).status)
            << idl::serialize(before.getState()) << " -> " << idl::serialize(proposed.getState());
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, RejectsDisallowedEdges) {
    const std::vector<CoordinatorStateEnum> states{
        kUnused,
        kInitializing,
        kPreparingToDonate,
        kCloning,
        kApplying,
        kBlockingWrites,
        kAborting,
        kCommitting,
        kQuiesced,
        kDone,
    };

    // Self-edges.
    for (auto state : states) {
        assertViolated({"isAllowedEdge"}, validate(docAt(state), docAt(state)));
    }

    const std::vector<std::pair<CoordinatorStateEnum, CoordinatorStateEnum>> edges{
        // Skipping a state.
        {kInitializing, kCloning},
        {kUnused, kPreparingToDonate},
        // Going back.
        {kApplying, kCloning},
        // Aborting is not reachable from committing or initializing.
        {kCommitting, kAborting},
        {kInitializing, kAborting},
        // Committing is not reachable from aborting.
        {kAborting, kCommitting},
        // Done is terminal.
        {kDone, kQuiesced},
    };
    for (const auto& [from, to] : edges) {
        assertViolated({"isAllowedEdge"}, validate(docAt(from), docAt(to)));
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, RejectsChangedCommonMetadata) {
    const auto before = docAt(kCloning);
    auto proposed = docAt(kApplying);
    proposed.setSourceNss(kOtherNss);

    assertViolated({"metadataUnchanged"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RejectsChangedExistingAbortReason) {
    const auto before = aborted(docAt(kApplying));
    auto proposed = withState(before, kDone);
    proposed.setAbortReason(BSON("code" << 1 << "errmsg" << "different"));

    assertViolated({"existingAbortReasonUnchanged"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RejectsChangedParticipantIds) {
    const auto before = docAt(kCloning);
    auto proposed = docAt(kApplying);
    auto donors = proposed.getDonorShards();
    donors[0].setId(ShardId("other"));
    proposed.setDonorShards(std::move(donors));

    assertViolated({"participantIdsUnchanged"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringInitForbidsShardEntries) {
    auto proposed = docAt(kInitializing);
    proposed.setDonorShards(
        {DonorShardEntry(ShardId("donor0"), makeDonorContext(DonorStateEnum::kUnused))});

    assertViolated({"noShardEntries"}, validate(docAt(kUnused), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringInitForbidsCloneTimestamp) {
    auto proposed = docAt(kInitializing);
    proposed.setCloneTimestamp(Timestamp(10, 1));

    assertViolated({"noCloneTimestamp"}, validate(docAt(kUnused), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringInitForbidsAbortReason) {
    auto proposed = docAt(kInitializing);
    proposed.setAbortReason(kAbortReason);

    assertViolated({"noAbortReason"}, validate(docAt(kUnused), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringPrepRequiresParticipants) {
    const auto before = docAt(kInitializing);

    auto noRecipients = docAt(kPreparingToDonate);
    noRecipients.setRecipientShards({});
    assertViolated({"hasParticipants", "participantsUnused"}, validate(before, noRecipients));

    auto duplicateIds = docAt(kPreparingToDonate);
    auto recipients = duplicateIds.getRecipientShards();
    recipients[1].setId(recipients[0].getId());
    duplicateIds.setRecipientShards(std::move(recipients));
    assertViolated({"hasParticipants"}, validate(before, duplicateIds));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringPrepRequiresUnusedParticipants) {
    auto proposed = docAt(kPreparingToDonate);
    auto donors = proposed.getDonorShards();
    donors[0].getMutableState().setState(DonorStateEnum::kDonatingInitialData);
    proposed.setDonorShards(std::move(donors));

    assertViolated({"participantsUnused"}, validate(docAt(kInitializing), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest,
     EnteringPrepRequiresPresetChunksAndZonesCleared) {
    const auto before = docAt(kInitializing);

    auto withZones = docAt(kPreparingToDonate);
    withZones.setZones(std::vector<ReshardingZoneType>{
        ReshardingZoneType("zone", BSON("a" << 1), BSON("a" << 2))});
    assertViolated({"presetChunksAndZonesCleared"}, validate(before, withZones));

    auto withPresetChunks = docAt(kPreparingToDonate);
    withPresetChunks.setPresetReshardedChunks(std::vector<ReshardedChunk>{
        ReshardedChunk(ShardId("recipient0"), BSON("a" << 1), BSON("a" << 2))});
    assertViolated({"presetChunksAndZonesCleared"}, validate(before, withPresetChunks));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCloneRequiresAllDonorsReported) {
    auto proposed = docAt(kCloning);
    auto donors = proposed.getDonorShards();
    donors[0].getMutableState().setState(DonorStateEnum::kPreparingToDonate);
    proposed.setDonorShards(std::move(donors));
    assertViolated({"allDonorsReportedMinFetchTimestamp"},
                   validate(docAt(kPreparingToDonate), proposed));

    auto missingTimestamp = docAt(kCloning);
    auto missingDonors = missingTimestamp.getDonorShards();
    missingDonors[0].getMutableState().setMinFetchTimestamp(boost::none);
    missingTimestamp.setDonorShards(std::move(missingDonors));
    assertViolated({"allDonorsReportedMinFetchTimestamp"},
                   validate(docAt(kPreparingToDonate), missingTimestamp));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCloneRequiresHighestMinFetchTimestamp) {
    const auto before = docAt(kPreparingToDonate);

    auto wrongTimestamp = docAt(kCloning);
    wrongTimestamp.setCloneTimestamp(Timestamp(10, 1));
    assertViolated({"cloneTimestampIsHighestMinFetch"}, validate(before, wrongTimestamp));

    auto missingTimestamp = docAt(kCloning);
    missingTimestamp.setCloneTimestamp(boost::none);
    assertViolated({"cloneTimestampIsHighestMinFetch"}, validate(before, missingTimestamp));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCloneRequiresRecipientsUnused) {
    auto proposed = docAt(kCloning);
    auto recipients = proposed.getRecipientShards();
    recipients[0].getMutableState().setState(RecipientStateEnum::kApplying);
    proposed.setRecipientShards(std::move(recipients));

    assertViolated({"recipientsUnused"}, validate(docAt(kPreparingToDonate), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RecipientsUnusedToleratesConcurrentAbort) {
    auto proposed = docAt(kCloning);
    auto recipients = proposed.getRecipientShards();
    recipients[0].getMutableState().setState(RecipientStateEnum::kError);
    recipients[0].getMutableState().setAbortReason(kAbortReason);
    proposed.setRecipientShards(std::move(recipients));

    ASSERT_OK(validate(docAt(kPreparingToDonate), proposed).status);
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCloneRequiresApproxCopySize) {
    const auto before = docAt(kPreparingToDonate);

    for (auto setter : {&ReshardingCoordinatorDocument::setApproxBytesToCopy,
                        &ReshardingCoordinatorDocument::setApproxDocumentsToCopy}) {
        auto missing = docAt(kCloning);
        (missing.*setter)(boost::none);
        assertViolated({"hasApproxCopySize"}, validate(before, missing));

        auto negative = docAt(kCloning);
        (negative.*setter)(-1);
        assertViolated({"hasApproxCopySize"}, validate(before, negative));
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringApplyRequiresRecipientsApplying) {
    auto proposed = docAt(kApplying);
    auto recipients = proposed.getRecipientShards();
    recipients[0].getMutableState().setState(RecipientStateEnum::kCloning);
    proposed.setRecipientShards(std::move(recipients));

    assertViolated({"allRecipientsApplying"}, validate(docAt(kCloning), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RequiresCopyProgressUnchangedAfterCloning) {
    for (const auto& [before, proposed] : validEdges()) {
        // Copy progress is first written on entering cloning.
        if (proposed.getState() <= kCloning) {
            continue;
        }
        const auto facts = factsFor(proposed.getState());

        auto changedBytes = proposed;
        changedBytes.setApproxBytesToCopy(changedBytes.getApproxBytesToCopy().value_or(0) + 1);
        assertViolated({"copyProgressUnchanged"}, validate(before, changedBytes, facts));

        auto changedTimestamp = proposed;
        changedTimestamp.setCloneTimestamp(Timestamp(10, 3));
        assertViolated({"copyProgressUnchanged"}, validate(before, changedTimestamp, facts));
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, RequiresCloneTimestampAfterCloning) {
    const std::vector<DocPair> edges{
        {docAt(kCloning), docAt(kApplying)},
        {docAt(kApplying), docAt(kBlockingWrites)},
    };
    for (auto [before, proposed] : edges) {
        before.setCloneTimestamp(boost::none);
        proposed.setCloneTimestamp(boost::none);
        assertViolated({"hasCloneTimestamp"},
                       validate(before, proposed, factsFor(proposed.getState())));
    }

    for (auto state : {kCloning, kApplying, kBlockingWrites}) {
        auto before = docAt(state);
        before.setCloneTimestamp(boost::none);
        assertViolated({"hasCloneTimestamp"}, validate(before, aborted(before)));
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, ForbidsAbortReasonOnEveryForwardEdge) {
    for (const auto& [before, proposed] : forwardEdges()) {
        auto changed = proposed;
        changed.setAbortReason(kAbortReason);
        assertViolated({"noAbortReason"}, validate(before, changed, factsFor(proposed.getState())));
    }
}

TEST(ReshardingCoordinatorTransitionValidationTest, RequiresDocumentsToCopyAllOrNone) {
    auto proposed = docAt(kApplying);
    auto donors = proposed.getDonorShards();
    donors[0].setDocumentsToCopy(5);
    proposed.setDonorShards(std::move(donors));

    assertViolated({"documentsToCopyAllOrNone"}, validate(docAt(kCloning), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringBlockRequiresCriticalSectionReadiness) {
    assertViolated({"canEnterCritical"},
                   validate(docAt(kApplying), docAt(kBlockingWrites), {.canEnterCritical = false}));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringBlockRequiresCriticalSectionExpiry) {
    auto proposed = docAt(kBlockingWrites);
    proposed.setCriticalSectionExpiresAt(boost::none);

    assertViolated({"hasCriticalSectionExpiresAt"},
                   validate(docAt(kApplying), proposed, factsFor(kBlockingWrites)));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCommitRequiresStrictConsistency) {
    auto proposed = docAt(kCommitting);
    auto recipients = proposed.getRecipientShards();
    recipients[0].getMutableState().setState(RecipientStateEnum::kApplying);
    proposed.setRecipientShards(std::move(recipients));

    assertViolated({"allRecipientsStrictlyConsistent"}, validate(docAt(kBlockingWrites), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCommitForbidsParticipantAbortReasons) {
    auto proposed = docAt(kCommitting);
    auto recipients = proposed.getRecipientShards();
    recipients[0].getMutableState().setAbortReason(kAbortReason);
    proposed.setRecipientShards(std::move(recipients));

    assertViolated({"noParticipantAbortReason"}, validate(docAt(kBlockingWrites), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringCommitRequiresCloneTimestamp) {
    auto before = docAt(kBlockingWrites);
    before.setCloneTimestamp(boost::none);
    auto proposed = docAt(kCommitting);
    proposed.setCloneTimestamp(boost::none);

    assertViolated({"hasCloneTimestamp"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RequiresCriticalSectionExpiryUnchanged) {
    auto proposed = docAt(kCommitting);
    proposed.setCriticalSectionExpiresAt(Date_t::fromMillisSinceEpoch(999));

    assertViolated({"criticalSectionExpiresAtUnchanged"},
                   validate(docAt(kBlockingWrites), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, RejectsEmptyParticipantLists) {
    auto noRecipients = docAt(kCommitting);
    noRecipients.setState(kBlockingWrites);
    noRecipients.setRecipientShards({});
    assertViolated(
        {"participantIdsUnchanged", "allRecipientsStrictlyConsistent", "noParticipantAbortReason"},
        validate(noRecipients, withState(noRecipients, kCommitting)));

    auto noDonors = docAt(kCommitting);
    noDonors.setState(kBlockingWrites);
    noDonors.setDonorShards({});
    assertViolated({"participantIdsUnchanged", "noParticipantAbortReason"},
                   validate(noDonors, withState(noDonors, kCommitting)));
}

TEST(ReshardingCoordinatorTransitionValidationTest, EnteringAbortRequiresAbortReason) {
    const auto before = docAt(kApplying);
    auto proposed = withState(before, kAborting);

    assertViolated({"hasAbortReason"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, SuccessExitRequiresAllParticipantsDone) {
    const auto before = docAt(kCommitting);
    auto proposed = withState(before, kDone);

    assertViolated({"allParticipantsDone"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, SuccessExitForbidsAbortReason) {
    const auto before = docAt(kCommitting);
    auto proposed = withState(withParticipantsDone(before), kDone);
    proposed.setAbortReason(kAbortReason);

    assertViolated({"noAbortReason"}, validate(before, proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, AbortExitDoesNotRequireParticipantsDone) {
    // The coordinator does not wait for participants to finish aborting before exiting.
    const auto before = docAt(kAborting);
    ASSERT_OK(validate(before, withState(before, kDone)).status);
}

TEST(ReshardingCoordinatorTransitionValidationTest, InitializingAbortExitAttachesAbortReason) {
    const auto before = docAt(kInitializing);
    auto proposed = withState(before, kDone);
    proposed.setAbortReason(kAbortReason);

    ASSERT_OK(validate(before, proposed).status);
}

TEST(ReshardingCoordinatorTransitionValidationTest, InitializingAbortExitRequiresAbortReason) {
    const auto before = docAt(kInitializing);
    assertViolated({"hasAbortReason"}, validate(before, withState(before, kDone)));
}

TEST(ReshardingCoordinatorTransitionValidationTest, ExitStateMatchesUserReshardingUUID) {
    const auto committing = docAt(kCommitting);

    auto withUserUUID = committing;
    withUserUUID.setUserReshardingUUID(UUID::gen());
    assertViolated({"exitStateMatchesUserReshardingUUID"},
                   validate(withUserUUID, withState(withParticipantsDone(withUserUUID), kDone)));

    auto quiescedWithoutUserUUID = withState(withParticipantsDone(committing), kQuiesced);
    quiescedWithoutUserUUID.setQuiescePeriodEnd(kQuiescePeriodEnd);
    assertViolated({"exitStateMatchesUserReshardingUUID"},
                   validate(committing, quiescedWithoutUserUUID));

    auto quiescedWithoutPeriodEnd = withState(withParticipantsDone(withUserUUID), kQuiesced);
    assertViolated({"exitStateMatchesUserReshardingUUID"},
                   validate(withUserUUID, quiescedWithoutPeriodEnd));
}

TEST(ReshardingCoordinatorTransitionValidationTest, ParticipantRulesTolerateConcurrentAbort) {
    auto proposed = docAt(kApplying);
    auto donors = proposed.getDonorShards();
    donors[0].getMutableState().setState(DonorStateEnum::kError);
    donors[0].getMutableState().setMinFetchTimestamp(boost::none);
    donors[0].getMutableState().setAbortReason(kAbortReason);
    proposed.setDonorShards(std::move(donors));

    ASSERT_OK(validate(docAt(kCloning), proposed).status);
}

TEST(ReshardingCoordinatorTransitionValidationTest,
     CommittingDoesNotRequireDonorsInBlockingWrites) {
    const auto before = docAt(kBlockingWrites);
    auto proposed = docAt(kCommitting);
    auto donors = proposed.getDonorShards();
    for (auto& donor : donors) {
        donor.getMutableState().setState(DonorStateEnum::kDonatingInitialData);
    }
    proposed.setDonorShards(std::move(donors));

    ASSERT_OK(validate(before, proposed).status);
}

TEST(ReshardingCoordinatorTransitionValidationTest, ReportsAllViolationsTogether) {
    auto proposed = docAt(kCloning);
    proposed.setCloneTimestamp(Timestamp(10, 1));
    proposed.setApproxBytesToCopy(-1);
    proposed.setAbortReason(kAbortReason);

    assertViolated({"cloneTimestampIsHighestMinFetch", "hasApproxCopySize", "noAbortReason"},
                   validate(docAt(kPreparingToDonate), proposed));
}

TEST(ReshardingCoordinatorTransitionValidationTest, ReportsOffendingValues) {
    auto wrongTimestamp = docAt(kCloning);
    wrongTimestamp.setCloneTimestamp(Timestamp(10, 1));
    const auto wrong = validate(docAt(kPreparingToDonate), wrongTimestamp);
    ASSERT_EQ(wrong.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_STRING_CONTAINS(wrong.status.reason(),
                           "transition from preparing-to-donate to cloning violated:");
    ASSERT_STRING_CONTAINS(wrong.status.reason(),
                           "cloneTimestamp Timestamp(10, 1) != highest minFetchTimestamp "
                           "Timestamp(10, 2)");

    auto negativeBytes = docAt(kCloning);
    negativeBytes.setApproxBytesToCopy(-1);
    const auto negative = validate(docAt(kPreparingToDonate), negativeBytes);
    ASSERT_EQ(negative.status.code(), ErrorCodes::ReshardingPreconditionFailure);
    ASSERT_STRING_CONTAINS(negative.status.reason(), "approxBytesToCopy is negative: -1");
}

TEST(ReshardingCoordinatorTransitionValidationTest, MetadataRoundTripsStably) {
    auto doc = docAt(kCommitting);
    doc.setUserReshardingUUID(UUID::gen());
    doc.setStartTime(Date_t::fromMillisSinceEpoch(1234));
    doc.setPerformVerification(true);
    doc.setTelemetryContext(BSON("key" << "value"));

    // parse() returns a view onto this BSON, so keep it alive.
    const auto bson = doc.toBSON();
    const auto parsed = ReshardingCoordinatorDocument::parse(bson, IDLParserContext("test"));
    ASSERT_TRUE(doc.getCommonReshardingMetadata().toBSON().binaryEqual(
        parsed.getCommonReshardingMetadata().toBSON()));
}

}  // namespace
}  // namespace mongo
