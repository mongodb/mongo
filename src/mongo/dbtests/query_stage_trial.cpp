// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/trial_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("db.dummy");

class TrialStageTest : public unittest::Test {
public:
    TrialStageTest()
        : _opCtx(cc().makeOperationContext()),
          _expCtx(ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kTestNss).build()) {}

protected:
    // Pushes BSONObjs from the given vector into the given MockStage. Each empty BSONObj in
    // the vector causes a NEED_TIME to be queued up at that point instead of a result.
    void queueData(const std::vector<BSONObj>& results, MockStage* mockStage) {
        for (const auto& result : results) {
            if (result.isEmpty()) {
                mockStage->enqueueStateCode(PlanStage::NEED_TIME);
                continue;
            }
            const auto id = _ws.allocate();
            auto* member = _ws.get(id);
            member->doc.setValue(Document{result});
            _ws.transitionToOwnedObj(id);
            mockStage->enqueueAdvanced(id);
        }
    }

    // Returns the next result from the TrialStage, or boost::none if there are no more results.
    boost::optional<BSONObj> nextResult(TrialStage* trialStage) {
        PlanStage::StageState state;
        WorkingSetID id;
        do {
            state = trialStage->work(&id);
            if (state == PlanStage::ADVANCED) {
                auto* member = _ws.get(id);
                return member->doc.value().toBson();
            }
        } while (state == PlanStage::NEED_TIME);

        // There are not more results to return.
        ASSERT_TRUE(trialStage->isEOF());
        return boost::none;
    }

    std::unique_ptr<PlanYieldPolicy> yieldPolicy() {
        return std::make_unique<NoopYieldPolicy>(opCtx(), &opCtx()->fastClockSource());
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    WorkingSet* ws() {
        return &_ws;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    WorkingSet _ws;

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(TrialStageTest, AdoptsTrialPlanIfTrialSucceeds) {
    auto trialPlan = std::make_unique<MockStage>(_expCtx.get(), ws());
    auto backupPlan = std::make_unique<MockStage>(_expCtx.get(), ws());

    // Seed the trial plan with 20 results and no NEED_TIMEs.
    std::vector<BSONObj> trialResults;
    for (auto i = 0; i < 20; ++i) {
        trialResults.push_back(BSON("_id" << i));
    }
    queueData(trialResults, trialPlan.get());

    // Set the minimum advanced-to-works ratio to 0.75. Because every work() will result in an
    // ADVANCE, the trial plan will succeed.
    auto trialStage = std::make_unique<TrialStage>(
        _expCtx.get(), ws(), std::move(trialPlan), std::move(backupPlan), 10, 0.75);

    ASSERT_OK(trialStage->pickBestPlan(yieldPolicy().get()));

    // The trial phase completed and we picked the trial plan, not the backup plan.
    ASSERT_TRUE(trialStage->isTrialPhaseComplete());
    ASSERT_FALSE(trialStage->pickedBackupPlan());

    // Confirm that we see the full trialPlan results when we iterate the trialStage.
    for (const auto& result : trialResults) {
        ASSERT_BSONOBJ_EQ(result, *nextResult(trialStage.get()));
    }
    ASSERT_FALSE(nextResult(trialStage.get()));
    ASSERT_TRUE(trialStage->isEOF());
}

TEST_F(TrialStageTest, AdoptsTrialPlanIfTrialPlanHitsEOF) {
    auto trialPlan = std::make_unique<MockStage>(_expCtx.get(), ws());
    auto backupPlan = std::make_unique<MockStage>(_expCtx.get(), ws());

    // Seed the trial plan with 5 results and no NEED_TIMEs.
    std::vector<BSONObj> trialResults;
    for (auto i = 0; i < 5; ++i) {
        trialResults.push_back(BSON("_id" << i));
    }
    queueData(trialResults, trialPlan.get());

    // We schedule the trial to run for 10 works. Because we hit EOF after 5 results, we will end
    // the trial phase early and adopt the successful trial plan.
    auto trialStage = std::make_unique<TrialStage>(
        _expCtx.get(), ws(), std::move(trialPlan), std::move(backupPlan), 10, 0.75);

    ASSERT_OK(trialStage->pickBestPlan(yieldPolicy().get()));

    // The trial phase completed and we picked the trial plan, not the backup plan.
    ASSERT_TRUE(trialStage->isTrialPhaseComplete());
    ASSERT_FALSE(trialStage->pickedBackupPlan());

    // Get the specific stats for the stage and confirm that the trial completed early.
    auto* stats = static_cast<const TrialStats*>(trialStage->getSpecificStats());
    ASSERT_EQ(stats->trialPeriodMaxWorks, 10U);
    ASSERT_EQ(stats->trialWorks, 5U);

    // Confirm that we see the full trialPlan results when we iterate the trialStage.
    for (const auto& result : trialResults) {
        ASSERT_BSONOBJ_EQ(result, *nextResult(trialStage.get()));
    }
    ASSERT_FALSE(nextResult(trialStage.get()));
    ASSERT_TRUE(trialStage->isEOF());
}

TEST_F(TrialStageTest, AdoptsBackupPlanIfTrialDoesNotSucceed) {
    auto trialPlan = std::make_unique<MockStage>(_expCtx.get(), ws());
    auto backupPlan = std::make_unique<MockStage>(_expCtx.get(), ws());

    // Seed the trial plan with 20 results. Every second result will produce a NEED_TIME.
    std::vector<BSONObj> trialResults;
    for (auto i = 0; i < 20; ++i) {
        trialResults.push_back((i % 2) ? BSON("_id" << i) : BSONObj());
    }
    queueData(std::move(trialResults), trialPlan.get());

    // Seed the backup plan with 20 different results, so that we can validate that we see the
    // correct dataset once the trial phase is complete.
    std::vector<BSONObj> backupResults;
    for (auto i = 0; i < 20; ++i) {
        backupResults.push_back(BSON("_id" << (-i)));
    }
    queueData(backupResults, backupPlan.get());

    // Set the minimum advanced-to-works ratio to 0.75. Because every second work() will result in a
    // NEED_TIME and the actual ratio is thus 0.5, the trial plan will fail.
    auto trialStage = std::make_unique<TrialStage>(
        _expCtx.get(), ws(), std::move(trialPlan), std::move(backupPlan), 10, 0.75);

    ASSERT_OK(trialStage->pickBestPlan(yieldPolicy().get()));

    // The trial phase completed and we picked the backup plan.
    ASSERT_TRUE(trialStage->isTrialPhaseComplete());
    ASSERT_TRUE(trialStage->pickedBackupPlan());

    // Confirm that we see the full backupPlan results when we iterate the trialStage.
    for (const auto& result : backupResults) {
        ASSERT_BSONOBJ_EQ(result, *nextResult(trialStage.get()));
    }
    ASSERT_FALSE(nextResult(trialStage.get()));
    ASSERT_TRUE(trialStage->isEOF());
}

}  // namespace
}  // namespace mongo
