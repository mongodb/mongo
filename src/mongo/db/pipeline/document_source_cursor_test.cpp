/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/pipeline/document_source_cursor.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <string>
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
class MockDSCursorCatalogResourceHandle : public DocumentSourceCursor::CatalogResourceHandle {
public:
    MockDSCursorCatalogResourceHandle(std::shared_ptr<bool> executorHoldsStorageEngineState)
        : _executorHoldsStorageEngineState(executorHoldsStorageEngineState) {}
    ~MockDSCursorCatalogResourceHandle() override {
        ASSERT_EQ(*_executorHoldsStorageEngineState, false);
    }

    void acquire(OperationContext* opCtx, const PlanExecutor& exec) override {
        // No-op.
    }

    void release() override {
        ASSERT_EQ(*_executorHoldsStorageEngineState, false);
    }
    void checkCanServeReads(OperationContext* opCtx, const PlanExecutor& exec) override {
        // No-op.
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
    const ExplainVersion& getVersion() const override {
        static const ExplainVersion version = "mock";  // or any default value you prefer
        return version;
    }

    bool isMultiPlan() const override {
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

    boost::optional<StringData> getExecutorType() const override {
        return boost::none;
    }

    QueryFramework getQueryFramework() const override {
        return QueryFramework::kUnknown;
    }

    bool usesCollectionAcquisitions() const override {
        return true;
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

            auto catalogResourceHandle =
                make_intrusive<MockDSCursorCatalogResourceHandle>(holdsStorageEngineStateBool);

            try {
                MultipleCollectionAccessor collections;  // Intentionally empty.
                auto cursor =
                    DocumentSourceCursor::create(collections,
                                                 std::move(execWithDeleter),
                                                 catalogResourceHandle,
                                                 getExpCtx(),
                                                 DocumentSourceCursor::CursorType::kRegular);
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
