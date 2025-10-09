/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>

#include "mongo/db/query/plan_executor_express.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/express/express_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/delete_request_gen.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/cursor_response_gen.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer_express.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/s/scoped_collection_metadata.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"


namespace mongo {
namespace {
class DoNotRecoverPolicy final : public express::ExceptionRecoveryPolicy {
public:
    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::WriteConflict>& exception) const override {
        throwWriteConflictException(
            "Write conflict during plan execution and "
            "yielding is disabled.");
    }

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TemporarilyUnavailable>& exception) const override {
        throwTemporarilyUnavailableException(
            "got TemporarilyUnavailable exception on a plan that "
            "cannot auto-yield");
    }

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TransactionTooLargeForCache>& exception) const override {
        exception.addContext("Internal retry explicitly disabled for query"_sd);
        throw exception;
    }

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::StaleConfig>& exception) const override {
        exception.addContext("Internal retry explicitly disabled for query"_sd);
        throw exception;
    }
};

static const DoNotRecoverPolicy doNotRecoverPolicy;

class BaseRecoveryPolicy : public express::ExceptionRecoveryPolicy {
public:
    using express::ExceptionRecoveryPolicy::recoverIfPossible;

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::WriteConflict>& exception) const override {
        // Ask the executor to retry with a more up-to-date snapshot.
        return express::WaitingForYield();
    }

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TemporarilyUnavailable>& exception) const override {
        return express::WaitingForBackoff();
    }

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TransactionTooLargeForCache>&) const override = 0;

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::StaleConfig>& exception) const override {
        return express::WaitingForCondition(std::move(*exception->getCriticalSectionSignal()));
    }
};

class RecoveryPolicyForPrimary : public BaseRecoveryPolicy {
public:
    using BaseRecoveryPolicy::recoverIfPossible;

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TransactionTooLargeForCache>& exception) const override {
        // Primaries do not recover from TransactionTooLargeForCache when processing writes. This is
        // a query fatal error.
        throw exception;
    }
};

static const RecoveryPolicyForPrimary recoveryPolicyForPrimary;

class RecoveryPolicyForSecondary : public BaseRecoveryPolicy {
public:
    using BaseRecoveryPolicy::recoverIfPossible;

    express::PlanProgress recoverIfPossible(
        ExceptionFor<ErrorCodes::TransactionTooLargeForCache>& exception) const override {
        // When applying a write on a secondary, we assume that the write already succeeded on the
        // primary and will eventually succeed here as well if we keep trying.
        return express::WaitingForYield();
    }
};

static const RecoveryPolicyForPrimary recoveryPolicyForSecondary;

template <class Plan>
class PlanExecutorExpress final : public PlanExecutor {
public:
    PlanExecutorExpress(OperationContext* opCtx,
                        std::unique_ptr<CanonicalQuery> cq,
                        typename Plan::CollectionType coll,
                        Plan plan,
                        const express::ExceptionRecoveryPolicy* recoveryPolicy,
                        bool returnOwnedBson);

    CanonicalQuery* getCanonicalQuery() const override {
        return _cq.get();
    }

    Pipeline* getPipeline() const override {
        MONGO_UNREACHABLE_TASSERT(8375801);
    }

    const NamespaceString& nss() const override {
        return _nss;
    }

    const std::vector<NamespaceStringOrUUID>& getSecondaryNamespaces() const override {
        return _secondaryNss;
    }

    OperationContext* getOpCtx() const override {
        return _opCtx;
    }

    void saveState() override {
        _plan.releaseResources();
    }

    void restoreState(const RestoreContext& context) override {
        _plan.restoreResources(_opCtx, context.collection(), _nss);
    }

    void detachFromOperationContext() override {
        _opCtx = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _opCtx = opCtx;
    }

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;

    ExecState getNextDocument(Document* objOut, RecordId* dlOut) override {
        BSONObj bsonDoc;
        auto state = getNext(&bsonDoc, dlOut);
        *objOut = Document(bsonDoc);
        return state;
    }

    bool isEOF() override {
        return _plan.exhausted();
    };

    long long executeCount() override {
        MONGO_UNREACHABLE_TASSERT(8375802);
    }

    UpdateResult executeUpdate() override {
        BSONObj obj;
        getNext(&obj, nullptr);
        return getUpdateResult();
    }

    UpdateResult getUpdateResult() const override {
        return {_writeOperationStats.docsUpdated() > 0, /* existing */
                _writeOperationStats.isModUpdate(),     /* is a $mod update */
                _writeOperationStats.docsUpdated(),     /* numDocsModified */
                _writeOperationStats.docsMatched(),     /* numDocsMatched */
                BSONObj::kEmptyObject,                  /* upserted Doc */
                _writeOperationStats.containsDotsAndDollarsField()};
    }

    long long executeDelete() override {
        BSONObj unusedObj;
        while (getNext(&unusedObj, nullptr /* record id out */) != ExecState::IS_EOF) {
            // Keep deleting!
        }
        return _writeOperationStats.docsDeleted();
    }

    long long getDeleteResult() const override {
        return _writeOperationStats.docsDeleted();
    }

    BatchedDeleteStats getBatchedDeleteStats() override {
        MONGO_UNREACHABLE_TASSERT(8375807);
    }

    void markAsKilled(Status killStatus) override {
        invariant(!killStatus.isOK());
        if (_killStatus.isOK()) {
            _killStatus = killStatus;
        }
    }

    void dispose(OperationContext* opCtx) override {
        _isDisposed = true;
    }

    void stashResult(const BSONObj& obj) override {
        MONGO_UNREACHABLE_TASSERT(8375808);
    }

    bool isMarkedAsKilled() const override {
        return !_killStatus.isOK();
    }

    Status getKillStatus() override {
        invariant(isMarkedAsKilled());
        return _killStatus;
    }

    bool isDisposed() const override {
        return _isDisposed;
    }

    Timestamp getLatestOplogTimestamp() const override {
        return {};
    }

    BSONObj getPostBatchResumeToken() const override {
        return {};
    }

    LockPolicy lockPolicy() const override {
        return LockPolicy::kLockExternally;
    }

    const PlanExplainer& getPlanExplainer() const override {
        return _planExplainer;
    }

    void enableSaveRecoveryUnitAcrossCommandsIfSupported() override {}

    bool isSaveRecoveryUnitAcrossCommandsEnabled() const override {
        return false;
    }

    boost::optional<StringData> getExecutorType() const override {
        return CursorType_serializer(_cursorType);
    }

    QueryFramework getQueryFramework() const override {
        return PlanExecutor::QueryFramework::kClassicOnly;
    }

    void setReturnOwnedData(bool returnOwnedData) override {
        _mustReturnOwnedBson = returnOwnedData;
    }

    bool usesCollectionAcquisitions() const override {
        return std::is_same_v<std::decay<typename Plan::CollectionType>, CollectionAcquisition>;
    }

    const Plan& getPlan() const {
        return _plan;
    }

private:
    void readyPlanExecution(express::Ready,
                            size_t& numUnavailabilityYieldsSinceLastSuccess,
                            size_t& numWriteConflictYieldsSinceLastSuccess);
    void readyPlanExecution(express::WaitingForYield,
                            size_t& numUnavailabilityYieldsSinceLastSuccess,
                            size_t& numWriteConflictYieldsSinceLastSuccess);
    void readyPlanExecution(express::WaitingForBackoff,
                            size_t& numUnavailabilityYieldsSinceLastSuccess,
                            size_t& numWriteConflictYieldsSinceLastSuccess);
    void readyPlanExecution(express::WaitingForCondition result,
                            size_t& numUnavailabilityYieldsSinceLastSuccess,
                            size_t& numWriteConflictYieldsSinceLastSuccess);
    void readyPlanExecution(express::Exhausted,
                            size_t& numUnavailabilityYieldsSinceLastSuccess,
                            size_t& numWriteConflictYieldsSinceLastSuccess);

    OperationContext* _opCtx;
    std::unique_ptr<CanonicalQuery> _cq;
    NamespaceString _nss;  // Copied from _cq.

    mongo::CommonStats _commonStats;
    express::PlanStats _planStats;
    express::IteratorStats _iteratorStats;
    express::WriteOperationStats _writeOperationStats;

    bool _isDisposed{false};
    Status _killStatus = Status::OK();

    PlanExplainerExpress _planExplainer;
    std::vector<NamespaceStringOrUUID> _secondaryNss;

    Plan _plan;
    bool _mustReturnOwnedBson;

    /**
     * Some commands return multiple cursors to the client, which are distinguished by their "cursor
     * type." Express execution is only ever used for the standard case of reading documents from a
     * collection.
     */
    static constexpr CursorTypeEnum _cursorType = CursorTypeEnum::DocumentResult;
};

template <class Plan>
PlanExecutorExpress<Plan>::PlanExecutorExpress(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    typename Plan::CollectionType collection,
    Plan plan,
    const express::ExceptionRecoveryPolicy* recoveryPolicy,
    bool returnOwnedBson)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _nss(express::accessCollection(collection).ns()),
      _commonStats("EXPRESS"),
      _planExplainer(&_commonStats, &_planStats, &_iteratorStats, &_writeOperationStats),
      _plan(std::move(plan)),
      _mustReturnOwnedBson(returnOwnedBson) {
    _plan.open(
        _opCtx, collection, recoveryPolicy, &_planStats, &_iteratorStats, &_writeOperationStats);
}

template <class Plan>
PlanExecutor::ExecState PlanExecutorExpress<Plan>::getNext(BSONObj* out, RecordId* dlOut) {
    bool haveOutput = false;
    size_t numUnavailabilityYieldsSinceLastSuccess = 0;
    size_t numWriteConflictYieldsSinceLastSuccess = 0;

    checkFailPointPlanExecAlwaysFails();

    express::PlanProgress progress((express::Ready()));
    while (!haveOutput) {
        if (_plan.exhausted()) {
            return ExecState::IS_EOF;
        }

        _opCtx->checkForInterrupt();

        progress = _plan.proceed(_opCtx, [&](RecordId rid, BSONObj obj) {
            *out = std::move(obj);
            if (dlOut) {
                *dlOut = std::move(rid);
            }
            if (_mustReturnOwnedBson) {
                out->makeOwned();
            }
            haveOutput = true;
            return express::Ready();
        });

        std::visit(
            [&, this](auto result) {
                this->readyPlanExecution(std::move(result),
                                         numUnavailabilityYieldsSinceLastSuccess,
                                         numWriteConflictYieldsSinceLastSuccess);
            },
            std::move(progress));
    }

    return ExecState::ADVANCED;
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::Ready,
                                                   size_t& numUnavailabilityYieldsSinceLastSuccess,
                                                   size_t& numWriteConflictYieldsSinceLastSuccess) {
    // Born ready B).
    numUnavailabilityYieldsSinceLastSuccess = 0;
    numWriteConflictYieldsSinceLastSuccess = 0;
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::WaitingForYield,
                                                   size_t& numUnavailabilityYieldsSinceLastSuccess,
                                                   size_t& numWriteConflictYieldsSinceLastSuccess) {
    logWriteConflictAndBackoff(numWriteConflictYieldsSinceLastSuccess++,
                               "plan execution",
                               "write contention during express execution"_sd,
                               NamespaceStringOrUUID(_nss));

    // TODO: Is this the desired behavior?
    _plan.temporarilyReleaseResourcesAndYield(_opCtx, []() {
        // No-op.
    });
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::WaitingForBackoff,
                                                   size_t& numUnavailabilityYieldsSinceLastSuccess,
                                                   size_t& numWriteConflictYieldsSinceLastSuccess) {
    handleTemporarilyUnavailableException(
        _opCtx,
        numUnavailabilityYieldsSinceLastSuccess++,
        "plan executor",
        NamespaceStringOrUUID(_nss),
        ExceptionFor<ErrorCodes::TemporarilyUnavailable>(Status(
            ErrorCodes::TemporarilyUnavailable, "resource contention during express execution"_sd)),
        numWriteConflictYieldsSinceLastSuccess);

    // TODO: Is this the desired behavior?
    _plan.temporarilyReleaseResourcesAndYield(_opCtx, []() {
        // No-op.
    });
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::WaitingForCondition result,
                                                   size_t& numUnavailabilityYieldsSinceLastSuccess,
                                                   size_t& numWriteConflictYieldsSinceLastSuccess) {
    _plan.temporarilyReleaseResourcesAndYield(_opCtx, [this, &result]() {
        OperationShardingState::waitForCriticalSectionToComplete(this->_opCtx, result.waitSignal())
            .ignore();
    });
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::Exhausted,
                                                   size_t& numUnavailabilityYieldsSinceLastSuccess,
                                                   size_t& numWriteConflictYieldsSinceLastSuccess) {
    // No execution to get ready for.
    numUnavailabilityYieldsSinceLastSuccess = 0;
    numWriteConflictYieldsSinceLastSuccess = 0;
}

template <class IteratorChoice, class WriteOperationChoice>
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutor(
    OperationContext* opCtx,
    IteratorChoice iterator,
    WriteOperationChoice writeOperation,
    std::unique_ptr<CanonicalQuery> cq,
    typename IteratorChoice::CollectionTypeChoice coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    using ShardFilterForRead = std::variant<express::NoShardFilter, ScopedCollectionFilter>;

    ShardFilterForRead shardFilter = express::NoShardFilter();
    if (collectionFilter) {
        shardFilter = std::move(*collectionFilter);
    }

    using Projection = std::variant<express::IdentityProjection, const projection_ast::Projection*>;
    Projection projection((express::IdentityProjection()));
    if (cq && cq->getProj() != nullptr) {
        projection = cq->getProj();
    }

    return std::visit(
        [&](auto& chosenShardFilter,
            auto& chosenProjection) -> std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> {
            auto plan = express::ExpressPlan(std::move(iterator),
                                             std::move(writeOperation),
                                             std::move(chosenShardFilter),
                                             std::move(chosenProjection));

            return {new PlanExecutorExpress(opCtx,
                                            std::move(cq),
                                            coll,
                                            std::move(plan),
                                            &doNotRecoverPolicy,
                                            returnOwnedBson),
                    PlanExecutor::Deleter(opCtx)};
        },
        shardFilter,
        projection);
}
}  // namespace

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindById(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    return std::visit(
        [&](auto collectionAlternative) {
            const BSONObj& queryFilter = cq->getQueryObj();
            return makeExpressExecutor(
                opCtx,
                express::IdLookupViaIndex<decltype(collectionAlternative)>(queryFilter),
                express::NoWriteOperation(),
                std::move(cq),
                collectionAlternative,
                std::move(collectionFilter),
                returnOwnedBson);
        },
        coll.get());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByClusteredId(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    return std::visit(
        [&](auto collectionAlternative) {
            const BSONObj& queryFilter = cq->getQueryObj();
            return makeExpressExecutor(
                opCtx,
                express::IdLookupOnClusteredCollection<decltype(collectionAlternative)>(
                    queryFilter),
                express::NoWriteOperation(),
                std::move(cq),
                collectionAlternative,
                std::move(collectionFilter),
                returnOwnedBson);
        },
        coll.get());
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByUserIndex(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    const IndexEntry& index,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    auto indexDescriptor = coll.getCollectionPtr()->getIndexCatalog()->findIndexByName(
        opCtx, index.identifier.catalogName);
    tassert(8884404,
            fmt::format("Attempt to build plan for nonexistent index -- namespace: {}, "
                        "CanonicalQuery: {}, IndexEntry: {}",
                        coll.getCollectionPtr()->ns().toStringForErrorMsg(),
                        cq->toStringShortForErrorMsg(),
                        index.toString()),
            indexDescriptor);

    return std::visit(
        [&](auto collectionAlternative) {
            const CollatorInterface* collator = cq->getCollator();
            auto cmpExpr =
                dynamic_cast<ComparisonMatchExpressionBase*>(cq->getPrimaryMatchExpression());
            tassert(10269303, "Invalid match expression", cmpExpr);
            BSONElement queryFilter = cmpExpr->getData();
            return makeExpressExecutor(opCtx,
                                       express::LookupViaUserIndex<decltype(collectionAlternative)>(
                                           queryFilter,
                                           indexDescriptor->getEntry()->getIdent(),
                                           index.identifier.catalogName,
                                           collator),
                                       express::NoWriteOperation(),
                                       std::move(cq),
                                       collectionAlternative,
                                       std::move(collectionFilter),
                                       returnOwnedBson);
        },
        coll.get());
}


/**
 * Determine appropriate recovery policy for write operations in express based on the
 * PlanYieldPolicy from the request. Applies appropriate overrides for multi-statement transactions,
 * failpoints, replication etc.
 */
const express::ExceptionRecoveryPolicy* getExpressRecoveryPolicy(
    OperationContext* opCtx, PlanYieldPolicy::YieldPolicy requestedPolicy) {

    auto policyWithOverride =
        PlanYieldPolicy::getPolicyOverrideForOperation(opCtx, requestedPolicy);
    if (policyWithOverride == PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY ||
        policyWithOverride == PlanYieldPolicy::YieldPolicy::YIELD_MANUAL ||
        MONGO_unlikely(skipWriteConflictRetries.shouldFail())) {
        return &doNotRecoverPolicy;
    } else if (opCtx->writesAreReplicated()) {
        return &recoveryPolicyForPrimary;
    } else {
        return &recoveryPolicyForSecondary;
    }
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForUpdate(
    OperationContext* opCtx,
    CollectionAcquisition collection,
    ParsedUpdate* parsedUpdate,
    bool returnOwnedBson) {

    const UpdateRequest* request = parsedUpdate->getRequest();

    using Iterator = std::variant<express::IdLookupViaIndex<CollectionAcquisition>,
                                  express::IdLookupOnClusteredCollection<CollectionAcquisition>>;
    auto iterator = [&]() -> Iterator {
        bool isClusteredOnId =
            clustered_util::isClusteredOnId(collection.getCollectionPtr()->getClusteredInfo());
        if (isClusteredOnId) {
            return express::IdLookupOnClusteredCollection<CollectionAcquisition>(
                request->getQuery());
        } else {
            return express::IdLookupViaIndex<CollectionAcquisition>(request->getQuery());
        }
    }();

    bool isUserInitiatedWrite = opCtx->writesAreReplicated() &&
        !(request->isFromOplogApplication() ||
          parsedUpdate->getDriver()->type() == UpdateDriver::UpdateType::kDelta ||
          request->source() == OperationSource::kFromMigrate);

    auto writeOperation =
        express::UpdateOperation(parsedUpdate->getDriver(), isUserInitiatedWrite, request);

    using ShardFilter = std::variant<express::NoShardFilter, write_stage_common::PreWriteFilter>;
    auto shardFilter = [&]() -> ShardFilter {
        if (request->getIsExplain()) {
            return ShardFilter{express::NoShardFilter()};
        } else {
            return ShardFilter{write_stage_common::PreWriteFilter(opCtx, collection.nss())};
        }
    }();

    auto recoveryPolicy = getExpressRecoveryPolicy(opCtx, parsedUpdate->yieldPolicy());

    return std::visit(
        [&](auto chosenIterator,
            auto chosenShardFilter) -> std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> {
            auto plan = express::ExpressPlan(std::move(chosenIterator),
                                             std::move(writeOperation),
                                             std::move(chosenShardFilter),
                                             express::IdentityProjection());
            return {
                new PlanExecutorExpress(
                    opCtx, nullptr, collection, std::move(plan), recoveryPolicy, returnOwnedBson),
                PlanExecutor::Deleter(opCtx)};
        },
        std::move(iterator),
        std::move(shardFilter));
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForDelete(
    OperationContext* opCtx, CollectionAcquisition collection, ParsedDelete* parsedDelete) {
    const DeleteRequest* request = parsedDelete->getRequest();

    using Iterator = std::variant<express::IdLookupViaIndex<CollectionAcquisition>,
                                  express::IdLookupOnClusteredCollection<CollectionAcquisition>>;
    auto iterator = [&]() -> Iterator {
        bool isClusteredOnId =
            clustered_util::isClusteredOnId(collection.getCollectionPtr()->getClusteredInfo());
        if (isClusteredOnId) {
            return express::IdLookupOnClusteredCollection<CollectionAcquisition>(
                request->getQuery());
        } else {
            return express::IdLookupViaIndex<CollectionAcquisition>(request->getQuery());
        }
    }();

    using WriteOperation =
        std::variant<express::DeleteOperation, express::DummyDeleteOperationForExplain>;
    using ShardFilter = std::variant<express::NoShardFilter, write_stage_common::PreWriteFilter>;
    auto [writeOperation, shardFilter] = [&]() -> std::pair<WriteOperation, ShardFilter> {
        if (request->getIsExplain()) {
            // We elide the shard filter when executing a delete operation for an explain command.
            // There's no need to strictly check if a write belongs to the shard if we're not going
            // to perform it.
            return {WriteOperation(
                        express::DummyDeleteOperationForExplain(request->getReturnDeleted())),
                    ShardFilter(express::NoShardFilter())};
        } else if (request->getFromMigrate()) {
            // Write commands issued by chunk migration operations should execute whether or not the
            // written document belongs to the chunk, so there is no shard filter.
            return {WriteOperation(express::DeleteOperation(request->getStmtId(),
                                                            request->getFromMigrate(),
                                                            request->getReturnDeleted())),
                    ShardFilter(express::NoShardFilter())};
        } else {
            return {WriteOperation(express::DeleteOperation(request->getStmtId(),
                                                            request->getFromMigrate(),
                                                            request->getReturnDeleted())),
                    ShardFilter(write_stage_common::PreWriteFilter(opCtx, collection.nss()))};
        }
    }();

    auto recoveryPolicy = getExpressRecoveryPolicy(opCtx, parsedDelete->yieldPolicy());

    return std::visit(
        [&](auto chosenIterator,
            auto chosenWriteOperation,
            auto chosenShardFilter) -> std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> {
            auto plan = express::ExpressPlan(std::move(chosenIterator),
                                             std::move(chosenWriteOperation),
                                             std::move(chosenShardFilter),
                                             express::IdentityProjection());
            return {new PlanExecutorExpress(opCtx,
                                            nullptr /* cq */,
                                            collection,
                                            std::move(plan),
                                            recoveryPolicy,
                                            false /* returnOwnedBson */),
                    PlanExecutor::Deleter(opCtx)};
        },
        std::move(iterator),
        std::move(writeOperation),
        std::move(shardFilter));
}

boost::optional<IndexEntry> getIndexForExpressEquality(const CanonicalQuery& cq,
                                                       const QueryPlannerParams& plannerParams) {
    const auto& findCommand = cq.getFindCommandRequest();

    const bool needsShardFilter =
        plannerParams.mainCollectionInfo.options & QueryPlannerParams::INCLUDE_SHARD_FILTER;
    const bool hasLimitOne = (findCommand.getLimit() && findCommand.getLimit().get() == 1);
    auto cmpExpr = dynamic_cast<ComparisonMatchExpressionBase*>(cq.getPrimaryMatchExpression());
    tassert(10269304, "Invalid match expression", cmpExpr);
    const auto& data = cmpExpr->getData();
    const bool collationRelevant = data.type() == BSONType::String ||
        data.type() == BSONType::Object || data.type() == BSONType::Array;

    RelevantFieldIndexMap fields;
    QueryPlannerIXSelect::getFields(cq.getPrimaryMatchExpression(), &fields);
    std::vector<IndexEntry> indexes =
        QueryPlannerIXSelect::findRelevantIndices(fields, plannerParams.mainCollectionInfo.indexes);

    int numFields = -1;
    const IndexEntry* bestEntry = nullptr;
    for (const auto& e : indexes) {
        if (
            // Indexes like geo/hash cannot generate exact bounds. Coarsely filter these out.
            e.type != IndexType::INDEX_BTREE ||
            // Also ensure that the query and index collators match.
            (collationRelevant &&
             !CollatorInterface::collatorsMatch(cq.getCollator(), e.collator)) ||
            // Sparse indexes cannot support comparisons to null.
            (e.sparse && data.isNull()) ||
            // Partial indexes may not be able to answer the query.
            (e.filterExpr &&
             !expression::isSubsetOf(cq.getPrimaryMatchExpression(), e.filterExpr))) {
            continue;
        }
        const auto currNFields = e.keyPattern.nFields();
        if (
            // We cannot guarantee that the result has at most one result doc.
            // TODO SERVER-87016: Support shard filtering for limitOne query with non-unique index.
            ((!e.unique || currNFields != 1) && (!hasLimitOne || needsShardFilter)) ||
            // This index is suitable but has more fields than the best so far.
            (bestEntry && numFields <= currNFields)) {
            continue;
        }
        bestEntry = &e;
        numFields = currNFields;
    }
    return (bestEntry != nullptr) ? boost::make_optional(std::move(*bestEntry)) : boost::none;
}
}  // namespace mongo
