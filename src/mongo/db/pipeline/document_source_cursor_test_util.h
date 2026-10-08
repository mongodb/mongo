// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/catalog_resource_handle.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace test {

/**
 * Produces the next BSON document for a mock PlanExecutor, or boost::none once the scan is
 * exhausted.
 */
using MockDocumentProvider = std::function<boost::optional<BSONObj>()>;

/**
 * Mock of the CatalogResourceHandle used by DocumentSourceCursor. This implementation does not
 * actually acquire any resources, and instead keeps track of whether the associated PlanExecutor
 * holds any storage engine state. If the PlanExecutor holds storage engine state while the catalog
 * resources are released, this class will trigger a test failure.
 */
class MockDSCursorCatalogResourceHandle : public CatalogResourceHandle {
public:
    explicit MockDSCursorCatalogResourceHandle(
        std::shared_ptr<bool> executorHoldsStorageEngineState)
        : _executorHoldsStorageEngineState(std::move(executorHoldsStorageEngineState)) {}
    ~MockDSCursorCatalogResourceHandle() override {
        invariant(!*_executorHoldsStorageEngineState);
    }

    void acquire(OperationContext* opCtx) override {
        // No-op.
    }

    void release() override {
        invariant(!*_executorHoldsStorageEngineState);
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
    PlanStatsDetails getWinningPlanTrialStats() const override {
        return PlanStatsDetails(BSONObj(), boost::none);
    }
    std::vector<ExplainPlanEntry> getPlanEntries(const ExplainPolicy&,
                                                 PlanStatsFormat,
                                                 PlanSelectionStrategy) const override {
        return std::vector<ExplainPlanEntry>{ExplainPlanEntry{}};
    }
};

/**
 * Base class for a MockPlanExecutor that returns BSON objects produced by a 'MockDocumentProvider'.
 * This class is abstract: subclasses must implement the save/restore and opCtx attach/detach hooks.
 */
class MockPlanExecutorBase : public PlanExecutor {
public:
    MockPlanExecutorBase(OperationContext* opCtx, MockDocumentProvider provider)
        : _opCtx(opCtx), _provider(std::move(provider)) {}

    // Convenience constructor for tests that provide a fixed vector of documents.
    MockPlanExecutorBase(OperationContext* opCtx, std::vector<BSONObj> data)
        : MockPlanExecutorBase(opCtx, makeVectorProvider(std::move(data))) {}

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
        auto next = nextDocument();
        if (!next) {
            return ExecState::IS_EOF;
        }
        if (out) {
            *out = *next;
        }
        return ExecState::ADVANCED;
    }

    ExecState getNextDocument(Document& objOut) override {
        auto next = nextDocument();
        if (!next) {
            return ExecState::IS_EOF;
        }
        objOut = Document(*next).getOwned();
        return ExecState::ADVANCED;
    }

    bool isEOF() const override {
        return _eof;
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

protected:
    // Returns the next document from the provider, or boost::none at EOF.
    boost::optional<BSONObj> nextDocument() {
        if (_eof) {
            return boost::none;
        }
        auto next = _provider();
        if (!next) {
            _eof = true;
        }
        return next;
    }

    static MockDocumentProvider makeVectorProvider(std::vector<BSONObj> data) {
        return [data = std::move(data), idx = size_t{0}]() mutable -> boost::optional<BSONObj> {
            if (idx >= data.size()) {
                return boost::none;
            }
            return data[idx++];
        };
    }

private:
    OperationContext* _opCtx;
    MockPlanExplainer _mockExplainer;
    bool _isDisposed = false;

protected:
    MockDocumentProvider _provider;
    bool _eof = false;
};

/**
 * MockPlanExecutor instance which reports a scan position in the oplog, as an oplog collection scan
 * does. The reported position advances past the documents the scan returns, which is what lets a
 * resumable oplog scan report progress even when it returns nothing.
 */
class MockPlanExecutorForOplogScan : public MockPlanExecutorBase {
public:
    MockPlanExecutorForOplogScan(OperationContext* opCtx,
                                 MockDocumentProvider provider,
                                 Timestamp latestOplogTimestamp)
        : MockPlanExecutorBase(opCtx, std::move(provider)),
          _latestOplogTimestamp(latestOplogTimestamp) {}

    MockPlanExecutorForOplogScan(OperationContext* opCtx,
                                 std::vector<BSONObj> data,
                                 Timestamp latestOplogTimestamp)
        : MockPlanExecutorBase(opCtx, std::move(data)),
          _latestOplogTimestamp(latestOplogTimestamp) {}

    void saveState() override {}
    void restoreState(const RestoreContext& context) override {}
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext* opCtx) override {}

    Timestamp getLatestOplogTimestamp() const override {
        return _latestOplogTimestamp;
    }

private:
    const Timestamp _latestOplogTimestamp;
};

}  // namespace test
}  // namespace mongo
