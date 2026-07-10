/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/classic/projection.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <limits>
#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

static const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("db.dummy");

// Number of bytes each evaluation of the observing expression charges to the tracker when one is
// threaded in. Chosen arbitrarily; the tests only assert that the peak is non-zero.
static constexpr int64_t kChargedBytes = 128;

/**
 * A test-only expression that records whether it received a memory tracker via its
 * EvaluationContext, and charges a fixed number of bytes to that tracker when present. Used to
 * observe that ProjectionStage threads its tracker down into expression evaluation.
 */
class MemoryTrackerObservingExpression final : public Expression {
public:
    static inline int gEvaluations = 0;
    static inline int gEvaluationsWithTracker = 0;
    static inline int64_t gLastTrackerMaxBytes = -1;
    static inline std::string gLastStageName;
    static void resetObservations() {
        gEvaluations = 0;
        gEvaluationsWithTracker = 0;
        gLastTrackerMaxBytes = -1;
        gLastStageName.clear();
    }

    explicit MemoryTrackerObservingExpression(ExpressionContext* expCtx) : Expression(expCtx) {}

    static boost::intrusive_ptr<Expression> parse(ExpressionContext* expCtx,
                                                  BSONElement expr,
                                                  const VariablesParseState&) {
        return make_intrusive<MemoryTrackerObservingExpression>(expCtx);
    }

    Value evaluate(const Document&, Variables*, const EvaluationContext& ctx) const final {
        ++gEvaluations;
        gLastStageName = std::string{ctx.stageName};
        if (ctx.tracker != nullptr) {
            ++gEvaluationsWithTracker;
            gLastTrackerMaxBytes = ctx.tracker->maxAllowedMemoryUsageBytes();
            ctx.tracker->add(kChargedBytes);
        }
        return Value(1);
    }

    Value serialize(const query_shape::SerializationOptions&) const final {
        return Value(Document{});
    }
    boost::intrusive_ptr<Expression> clone(ExpressionContext& expCtx) const final {
        return make_intrusive<MemoryTrackerObservingExpression>(&expCtx);
    }
    void acceptVisitor(ExpressionMutableVisitor*) final {}
    void acceptVisitor(ExpressionConstVisitor*) const final {}
};

REGISTER_TEST_EXPRESSION(_testProjectionTrackerObserver,
                         MemoryTrackerObservingExpression::parse,
                         AllowedWithApiStrict::kAlways,
                         AllowedWithClientType::kAny,
                         nullptr /* featureFlag */);

class ProjectionStageTest : public ServiceContextMongoDTest {
public:
    ProjectionStageTest()
        : _opCtx{makeOperationContext()},
          _expCtx{ExpressionContextBuilder{}.opCtx(_opCtx.get()).ns(kNss).build()} {}

    /**
     * Builds a ProjectionStageDefault for 'projSpec' over a single input document, runs it to EOF,
     * and returns its ProjectionStats.
     */
    ProjectionStats runProjection(BSONObj projSpec) {
        MemoryTrackerObservingExpression::resetObservations();

        auto queued = std::make_unique<QueuedDataStage>(_expCtx.get(), &_ws);
        WorkingSetID wsid = _ws.allocate();
        WorkingSetMember* member = _ws.get(wsid);
        member->doc = {SnapshotId(), Document{BSON("_id" << 0 << "a" << "hello")}};
        member->transitionToOwnedObj();
        queued->pushBack(wsid);

        ProjectionPolicies policies;
        auto projection = projection_ast::parseAndAnalyze(_expCtx, projSpec, policies);
        ProjectionStageDefault stage(_expCtx, projSpec, &projection, &_ws, std::move(queued));

        WorkingSetID outId = WorkingSet::INVALID_ID;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            state = stage.work(&outId);
        }

        auto stats = stage.getStats();
        return static_cast<const ProjectionStats&>(*stats->specific);
    }

    /**
     * Builds a ProjectionStageSimple (an inclusion/exclusion-only projection that never evaluates
     * expressions) for 'projSpec' over a single input document and runs it to EOF.
     */
    void runSimpleProjection(BSONObj projSpec) {
        auto queued = std::make_unique<QueuedDataStage>(_expCtx.get(), &_ws);
        WorkingSetID wsid = _ws.allocate();
        WorkingSetMember* member = _ws.get(wsid);
        member->doc = {SnapshotId(), Document{BSON("_id" << 0 << "a" << "hello")}};
        member->transitionToOwnedObj();
        queued->pushBack(wsid);

        ProjectionPolicies policies;
        auto projection = projection_ast::parseAndAnalyze(_expCtx, projSpec, policies);
        ProjectionStageSimple stage(_expCtx.get(), projSpec, &projection, &_ws, std::move(queued));

        WorkingSetID outId = WorkingSet::INVALID_ID;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            state = stage.work(&outId);
        }
    }

    // A computed projection whose value is the observing test expression.
    static BSONObj observingProjection() {
        return BSON("b" << BSON("$_testProjectionTrackerObserver" << BSONObj()));
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    // True if an operation-wide memory tracker was attached to the operation (i.e. some stage
    // created a memory tracker via OperationMemoryUsageTracker). Consumes the tracker.
    bool operationMemoryTrackerAttached() {
        return static_cast<bool>(OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(opCtx()));
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    WorkingSet _ws;
};

TEST_F(ProjectionStageTest, ThreadsMemoryTrackerWhenEvaluatingExpressions) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runProjection(observingProjection());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
}

TEST_F(ProjectionStageTest, DoesNotThreadMemoryTrackerWhenExpressionMemoryTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runProjection(observingProjection());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluations, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 0);
}

TEST_F(ProjectionStageTest, StageNameIsSetInEvaluationContext) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runProjection(observingProjection());

    ASSERT_EQ(MemoryTrackerObservingExpression::gLastStageName, "PROJECTION_DEFAULT");
}

TEST_F(ProjectionStageTest, MemoryTrackerHasNoPerStageLimit) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runProjection(observingProjection());

    ASSERT_EQ(MemoryTrackerObservingExpression::gEvaluationsWithTracker, 1);
    ASSERT_EQ(MemoryTrackerObservingExpression::gLastTrackerMaxBytes,
              std::numeric_limits<int64_t>::max());
}

TEST_F(ProjectionStageTest, ReportsPeakTrackedMemBytesInStatsWhenEnabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    auto stats = runProjection(observingProjection());

    ASSERT_GT(stats.peakTrackedMemBytes, 0u);
}

TEST_F(ProjectionStageTest, DoesNotReportPeakTrackedMemBytesInStatsWhenDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    auto stats = runProjection(observingProjection());

    ASSERT_EQ(stats.peakTrackedMemBytes, 0u);
}

TEST_F(ProjectionStageTest, AttachesOperationMemoryTrackerForComputedProjectionWhenEnabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runProjection(observingProjection());

    ASSERT_TRUE(operationMemoryTrackerAttached());
}

TEST_F(ProjectionStageTest, DoesNotAttachOperationMemoryTrackerWhenExpressionTrackingDisabled) {
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", false);

    runProjection(observingProjection());

    ASSERT_FALSE(operationMemoryTrackerAttached());
}

TEST_F(ProjectionStageTest, DoesNotAttachOperationMemoryTrackerForSimpleProjection) {
    // A simple (inclusion-only) projection never evaluates expressions, so it must not create a
    // tracker or attach the operation-wide memory tracker, even with both flags enabled.
    unittest::ServerParameterGuard queryMemTracking("featureFlagQueryMemoryTracking", true);
    unittest::ServerParameterGuard exprMemTracking("featureFlagExpressionMemoryTracking", true);

    runSimpleProjection(BSON("a" << 1));

    ASSERT_FALSE(operationMemoryTrackerAttached());
}

}  // namespace
}  // namespace mongo
