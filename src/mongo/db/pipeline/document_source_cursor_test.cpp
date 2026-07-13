// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

/**
 * Mock of the CatalogResourceHandle used by DocumentSourceCursor. This implementation does not
 * actually acquire any resources, and instead keeps track of whether the associated PlanExecutor
 * holds any storage engine state. If the PlanExecutor holds storage engine state while the catalog
 * resources are released, this class will trigger a test failure.
 */
class MockDSCursorCatalogResourceHandle : public CatalogResourceHandle {
public:
    MockDSCursorCatalogResourceHandle(std::shared_ptr<bool> executorHoldsStorageEngineState)
        : _executorHoldsStorageEngineState(executorHoldsStorageEngineState) {}
    ~MockDSCursorCatalogResourceHandle() override {
        ASSERT_EQ(*_executorHoldsStorageEngineState, false);
    }

    void acquire(OperationContext* opCtx) override {
        // No-op.
    }

    void release() override {
        ASSERT_EQ(*_executorHoldsStorageEngineState, false);
    }

    boost::intrusive_ptr<ShardRoleTransactionResourcesStasherForPipeline> getStasher()
        const override {
        MONGO_UNREACHABLE;
    }

private:
    // Tracks whether or not the associated plan executor holds storage engine state. This class
    // does not modify the contents of this variable, it just asserts that the executor is not
    // holding storage engine state when the catalog resources are released.
    std::shared_ptr<bool> _executorHoldsStorageEngineState;
};

/**
 * Mock implementation of PlanExplainer needed to satisfy DocumentSourceCursor's requirements.
 */
class MockPlanExplainer : public PlanExplainer {
public:
    bool isSbeExplainer() const override {
        return false;  // Assuming classic engine for this mock
    }

    bool areThereRejectedPlansToExplain() const override {
        return false;  // Assuming no multi-planning by default
    }

    std::string getPlanSummary() const override {
        return "Mock plan explainer";
    }

    void getSummaryStats(PlanSummaryStats* statsOut) const override {}
    PlanStatsDetails getWinningPlanStats(ExplainOptions::Verbosity verbosity) const override {
        return PlanStatsDetails(BSONObj(), boost::none);
    }
    PlanStatsDetails getWinningPlanTrialStats() const override {
        return PlanStatsDetails(BSONObj(), boost::none);
    }
    std::vector<PlanStatsDetails> getRejectedPlansStats(
        ExplainOptions::Verbosity verbosity) const override {
        return {};
    }
};


/**
 * Base class for a MockPlanExecutor that returns BSON objects from a pre-canned vector.
 */
class MockPlanExecutorBase : public PlanExecutor {
public:
    MockPlanExecutorBase(OperationContext* opCtx, std::vector<BSONObj> data)
        : _opCtx(opCtx), _data(std::move(data)) {}

    CanonicalQuery* getCanonicalQuery() const override {
        return nullptr;
    }

    const NamespaceString& nss() const override {
        static auto kNss = NamespaceString::createNamespaceString_forTest("foo.bar");
        return kNss;
    }

    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const override {
        static std::vector<NamespaceStringOrUUID> kEmpty;
        return kEmpty;
    }

    OperationContext* getOpCtx() const override {
        return _opCtx;
    }

    ExecState getNext(BSONObj* out, RecordId* dlOut) override {
        if (_idx >= _data.size()) {
            return ExecState::IS_EOF;
        }
        *out = _data[_idx++];
        return ExecState::ADVANCED;
    }

    ExecState getNextDocument(Document& objOut) override {
        BSONObj bson;
        auto ret = getNext(&bson, nullptr);
        objOut = Document(bson).getOwned();

        return ret;
    }

    bool isEOF() const override {
        return _idx == _data.size();
    }

    long long executeCount() override {
        MONGO_UNREACHABLE;
    }

    UpdateResult getUpdateResult() const override {
        MONGO_UNREACHABLE;
    }

    long long getDeleteResult() const override {
        MONGO_UNREACHABLE;
    }

    BatchedDeleteStats getBatchedDeleteStats() override {
        MONGO_UNREACHABLE;
    }

    void markAsKilled(Status killStatus) override {
        MONGO_UNREACHABLE;
    }

    void dispose(OperationContext* opCtx) override {
        _isDisposed = true;
    }

    void forceSpill(PlanYieldPolicy* yieldPolicy) override {
        MONGO_UNREACHABLE;
    }

    void stashResult(const BSONObj& obj) override {
        MONGO_UNREACHABLE;
    }

    bool isMarkedAsKilled() const override {
        MONGO_UNREACHABLE;
    }
    Status getKillStatus() const override {
        MONGO_UNREACHABLE;
    }

    bool isDisposed() const override {
        return _isDisposed;
    }

    Timestamp getLatestOplogTimestamp() const override {
        MONGO_UNREACHABLE;
    }

    BSONObj getPostBatchResumeToken() const override {
        MONGO_UNREACHABLE;
    }

    LockPolicy lockPolicy() const override {
        return PlanExecutor::LockPolicy::kLockExternally;
    }

    const PlanExplainer& getPlanExplainer() const override {
        return _mockExplainer;
    }

    boost::optional<std::string_view> getExecutorType() const override {
        return boost::none;
    }

    QueryFramework getQueryFramework() const override {
        return QueryFramework::kUnknown;
    }

private:
    OperationContext* _opCtx;

    std::vector<BSONObj> _data;
    size_t _idx = 0;

    MockPlanExplainer _mockExplainer;
    bool _isDisposed = false;
};

/**
 * Mock implementation of a PlanExecutor which randomly throws on calls to saveState() and
 * restoreState() based on given probabilities.
 */
class ThrowyPlanExecutor : public MockPlanExecutorBase {
public:
    ThrowyPlanExecutor(OperationContext* opCtx,
                       std::vector<BSONObj> data,
                       double saveProbability,
                       double restoreProbability,
                       std::shared_ptr<bool> holdsStorageEngineState)
        : MockPlanExecutorBase(opCtx, data),
          _seed(Date_t::now().asInt64()),
          _prng(_seed),
          _probabilityThrowInSave(saveProbability),
          _probabilityThrowInRestore(restoreProbability),
          _holdsStorageEngineState(holdsStorageEngineState) {
        LOGV2(10271308, "Created throwy plan executor with seed {seed}", "seed"_attr = _seed);
    }

    ~ThrowyPlanExecutor() override {
        *_holdsStorageEngineState = false;
    }

    // Generally the order of calls to detach the executor from storage:
    // saveState()
    // detachFromOperationContext() // This actually releases storage engine state.
    //
    // reattachToOperationContext()
    // restoreState() // This actually re-acquires storage engine state.
    //
    // The lack of symmetry here is no doubt confusing, but this is how it works.
    void saveState() override {
        // Note: It is NOT a precondition that the executor must be in a restored state to call
        // save(). save() is re-entrant and idempotent, specifically for the case where exceptions
        // are thrown in save() and restore().

        if (_prng.nextCanonicalDouble() < _probabilityThrowInSave) {
            throwWriteConflictException("WCE for test: saveState()");
        }
        _saved = true;
    }

    void restoreState(const RestoreContext& context) override {
        ASSERT(_saved);

        if (_prng.nextCanonicalDouble() < _probabilityThrowInRestore) {
            throwWriteConflictException("WCE for test: restoreState()");
        }
        _saved = false;
        *_holdsStorageEngineState = true;
    }

    void detachFromOperationContext() override {
        if (_saved) {
            *_holdsStorageEngineState = false;
        }
    }

    void reattachToOperationContext(OperationContext* opCtx) override {}

private:
    int64_t _seed;
    PseudoRandom _prng;
    double _probabilityThrowInSave;
    double _probabilityThrowInRestore;

    std::shared_ptr<bool> _holdsStorageEngineState;

    bool _saved = false;
};

using DSCursorTest = AggregationContextFixture;
TEST_F(DSCursorTest, TestSaveAndRestoreThrowing) {
    std::vector<BSONObj> bsons;
    for (size_t i = 0; i < 1000; ++i) {
        bsons.push_back(BSON("foo" << std::to_string(i)));
    }

    struct ThrowProbability {
        double saveProbability;
        double restoreProbability;
    };

    // Test with a variety of executors that throw with different probability.
    std::vector<double> vals = {0.0, 0.001, 0.01, 0.1, 1};
    std::vector<ThrowProbability> throwProbabilities;
    for (size_t i = 0; i < vals.size(); ++i) {
        for (size_t j = 0; j < vals.size(); ++j) {
            throwProbabilities.push_back({vals[i], vals[j]});
        }
    }

    for (auto probabilities : throwProbabilities) {
        size_t testsThatThrew = 0;
        size_t testsThatDidNotThrow = 0;

        for (size_t i = 0; i < 100; ++i) {
            // This is used to track whether the executor holds storage engine state. As part of
            // the test, we want to assert that the executor is NOT holding storage engine
            // resources when the catalog resources are freed.
            auto holdsStorageEngineStateBool = std::make_shared<bool>(true);

            auto exec = std::make_unique<ThrowyPlanExecutor>(getOpCtx(),
                                                             bsons,
                                                             probabilities.saveProbability,
                                                             probabilities.restoreProbability,
                                                             holdsStorageEngineStateBool);

            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> execWithDeleter(
                std::move(exec).release(), PlanExecutor::Deleter{getOpCtx()});

            try {
                MultipleCollectionAccessor collections;  // Intentionally empty.
                auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
                auto cursor =
                    DocumentSourceCursor::create(std::move(execWithDeleter),
                                                 getExpCtx(),
                                                 DocumentSourceCursor::CursorType::kRegular);
                cursor->bindCatalogInfo(collections, stasher);
                // Overwrite the exisitng catalogResourceHandle with our test version.
                cursor->setCatalogResourceHandle_forTest(
                    make_intrusive<MockDSCursorCatalogResourceHandle>(holdsStorageEngineStateBool));
                auto cursorStage = exec::agg::buildStage(cursor);

                for (const auto& expectedBson : bsons) {
                    DocumentSource::GetNextResult next = cursorStage->getNext();
                    ASSERT(next.isAdvanced());
                    auto doc = next.getDocument();
                    ASSERT_BSONOBJ_EQ(doc.toBson(), expectedBson);
                }

                DocumentSource::GetNextResult next = cursorStage->getNext();
                ASSERT(next.isEOF());

                cursorStage->dispose();
                testsThatDidNotThrow++;
            } catch (const DBException&) {
                // We're allowed to throw exceptions here due to the save()/restore() functions
                // throwing. However, we cannot crash the server.

                // If the test throws, we move on to the next one.
                testsThatThrew++;
            }
        }

        LOGV2(10271303,
              "Tests that threw {testsThatThrew}, tests that did not throw {testsThatDidNotThrow}",
              "testsThatThrew"_attr = testsThatThrew,
              "testsThatDidNotThrow"_attr = testsThatDidNotThrow);
    }
}
}  // namespace
}  // namespace mongo
