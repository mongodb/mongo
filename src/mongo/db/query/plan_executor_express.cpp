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
#include <utility>
#include <variant>

#include "mongo/db/query/plan_executor_express.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
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
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
template <class Plan>
class PlanExecutorExpress final : public PlanExecutor {
public:
    PlanExecutorExpress(OperationContext* opCtx,
                        std::unique_ptr<CanonicalQuery> cq,
                        VariantCollectionPtrOrAcquisition coll,
                        Plan plan,
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
        _coll = context.collection();
        _plan.restoreResources(_opCtx, &_coll.getCollectionPtr());
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
        MONGO_UNREACHABLE_TASSERT(8375803);
    }

    UpdateResult getUpdateResult() const override {
        MONGO_UNREACHABLE_TASSERT(8375804);
    }

    long long executeDelete() override {
        MONGO_UNREACHABLE_TASSERT(8375805);
    }

    long long getDeleteResult() const override {
        MONGO_UNREACHABLE_TASSERT(8375806);
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
        return _coll.isAcquisition();
    }

    const Plan& getPlan() const {
        return _plan;
    }

private:
    void readyPlanExecution(express::Ready);
    void readyPlanExecution(express::Exhausted);

    OperationContext* _opCtx;
    std::unique_ptr<CanonicalQuery> _cq;
    NamespaceString _nss;  // Copied from _cq.
    VariantCollectionPtrOrAcquisition _coll;

    mongo::CommonStats _commonStats;
    express::PlanStats _planStats;
    express::IteratorStats _iteratorStats;
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
PlanExecutorExpress<Plan>::PlanExecutorExpress(OperationContext* opCtx,
                                               std::unique_ptr<CanonicalQuery> cq,
                                               VariantCollectionPtrOrAcquisition coll,
                                               Plan plan,
                                               bool returnOwnedBson)
    : _opCtx(opCtx),
      _cq(std::move(cq)),
      _nss(_cq->nss()),
      _coll(coll),
      _commonStats("EXPRESS"),
      _planExplainer(&_commonStats, &_planStats, &_iteratorStats),
      _plan(std::move(plan)),
      _mustReturnOwnedBson(returnOwnedBson) {
    _plan.open(_opCtx, &coll.getCollectionPtr(), &_planStats, &_iteratorStats);
}

template <class Plan>
PlanExecutor::ExecState PlanExecutorExpress<Plan>::getNext(BSONObj* out, RecordId* dlOut) {
    bool haveOutput = false;

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

        std::visit([&, this](auto result) { this->readyPlanExecution(std::move(result)); },
                   std::move(progress));
    }

    return ExecState::ADVANCED;
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::Ready) {
    // Born ready B).
}

template <class Plan>
void PlanExecutorExpress<Plan>::readyPlanExecution(express::Exhausted) {
    // No execution to get ready for.
}

template <class IteratorChoice>
std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutor(
    OperationContext* opCtx,
    IteratorChoice iterator,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    using ShardFilterForRead = std::variant<express::NoShardFilter, ScopedCollectionFilter>;

    ShardFilterForRead shardFilter = express::NoShardFilter();
    if (collectionFilter) {
        shardFilter = std::move(*collectionFilter);
    }

    using Projection = std::variant<express::IdentityProjection, const projection_ast::Projection*>;
    Projection projection((express::IdentityProjection()));
    if (cq->getProj() != nullptr) {
        projection = cq->getProj();
    }

    fastPathQueryCounters.incrementExpressQueryCounter();

    return std::visit(
        [&](auto& chosenShardFilter,
            auto& chosenProjection) -> std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> {
            auto plan = express::ExpressPlan(
                std::move(iterator), std::move(chosenShardFilter), std::move(chosenProjection));

            return {new PlanExecutorExpress(
                        opCtx, std::move(cq), coll, std::move(plan), returnOwnedBson),
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
    const BSONObj& queryFilter = cq->getQueryObj();
    return makeExpressExecutor(opCtx,
                               express::IdLookupViaIndex(queryFilter),
                               std::move(cq),
                               coll,
                               std::move(collectionFilter),
                               returnOwnedBson);
}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExpressExecutorForFindByClusteredId(
    OperationContext* opCtx,
    std::unique_ptr<CanonicalQuery> cq,
    VariantCollectionPtrOrAcquisition coll,
    boost::optional<ScopedCollectionFilter> collectionFilter,
    bool returnOwnedBson) {
    const BSONObj& queryFilter = cq->getQueryObj();
    return makeExpressExecutor(opCtx,
                               express::IdLookupOnClusteredCollection(queryFilter),
                               std::move(cq),
                               coll,
                               std::move(collectionFilter),
                               returnOwnedBson);
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

    const CollatorInterface* collator = cq->getCollator();
    BSONElement queryFilter =
        static_cast<ComparisonMatchExpressionBase*>(cq->getPrimaryMatchExpression())->getData();
    return makeExpressExecutor(opCtx,
                               express::LookupViaUserIndex(queryFilter,
                                                           indexDescriptor->getEntry()->getIdent(),
                                                           index.identifier.catalogName,
                                                           collator),
                               std::move(cq),
                               coll,
                               std::move(collectionFilter),
                               returnOwnedBson);
}

boost::optional<IndexEntry> getIndexForExpressEquality(const CanonicalQuery& cq,
                                                       const QueryPlannerParams& plannerParams) {
    const auto& findCommand = cq.getFindCommandRequest();

    const bool needsShardFilter =
        plannerParams.mainCollectionInfo.options & QueryPlannerParams::INCLUDE_SHARD_FILTER;
    const bool hasLimitOne = (findCommand.getLimit() && findCommand.getLimit().get() == 1);
    const auto& data =
        static_cast<ComparisonMatchExpressionBase*>(cq.getPrimaryMatchExpression())->getData();
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
            ((!e.unique || currNFields != 1) && !hasLimitOne) ||
            // TODO SERVER-87016: Support shard filtering for limitOne query with non-unique index.
            (!e.unique && needsShardFilter) ||
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
