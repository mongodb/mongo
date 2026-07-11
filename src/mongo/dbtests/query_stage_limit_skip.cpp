// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file tests db/exec/limit.cpp and db/exec/skip.cpp.
 */


#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/classic/limit.h"
#include "mongo/db/exec/classic/mock_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/skip.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using std::max;
using std::min;
using std::unique_ptr;

static const int N = 50;

/**
 * Populates a 'MockStage' and returns it.
 */
std::unique_ptr<MockStage> getMS(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 WorkingSet* ws) {
    auto ms = std::make_unique<MockStage>(expCtx.get(), ws);

    // Put N ADVANCED results into the mock stage, and some other stalling results (YIELD/TIME).
    for (int i = 0; i < N; ++i) {
        ms->enqueueStateCode(PlanStage::NEED_TIME);

        WorkingSetID id = ws->allocate();
        WorkingSetMember* wsm = ws->get(id);
        wsm->doc = {SnapshotId(), Document{BSON("x" << i)}};
        wsm->transitionToOwnedObj();
        ms->enqueueAdvanced(id);

        ms->enqueueStateCode(PlanStage::NEED_TIME);
    }

    return ms;
}

int countResults(PlanStage* stage) {
    int count = 0;
    while (!stage->isEOF()) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        PlanStage::StageState status = stage->work(&id);
        if (PlanStage::ADVANCED != status) {
            continue;
        }
        ++count;
    }
    return count;
}

//
// Insert 50 objects.  Filter/skip 0, 1, 2, ..., 100 objects and expect the right # of results.
//
class QueryStageLimitSkipBasicTest {
public:
    void run() {
        const auto expCtx = ExpressionContextBuilder{}
                                .opCtx(_opCtx)
                                .ns(NamespaceString::createNamespaceString_forTest("test.dummyNS"))
                                .build();
        for (int i = 0; i < 2 * N; ++i) {
            WorkingSet ws;

            unique_ptr<PlanStage> skip =
                std::make_unique<SkipStage>(expCtx.get(), i, &ws, getMS(expCtx.get(), &ws));
            ASSERT_EQUALS(max(0, N - i), countResults(skip.get()));

            unique_ptr<PlanStage> limit =
                std::make_unique<LimitStage>(expCtx.get(), i, &ws, getMS(expCtx.get(), &ws));
            ASSERT_EQUALS(min(N, i), countResults(limit.get()));
        }
    }

protected:
    const ServiceContext::UniqueOperationContext _uniqOpCtx = cc().makeOperationContext();
    OperationContext* const _opCtx = _uniqOpCtx.get();
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_limit_skip") {}

    void setupTests() override {
        add<QueryStageLimitSkipBasicTest>();
    }
};

unittest::OldStyleSuiteInitializer<All> queryStageLimitSkipAll;

}  // namespace
}  // namespace mongo
