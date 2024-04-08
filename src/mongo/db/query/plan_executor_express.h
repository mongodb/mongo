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
#pragma once

#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer_express.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {

/**
 * Tries to find an index suitable for use in the express equality path. Excludes indexes which
 * cannot 1) satisfy the given query with exact bounds and 2) provably return at most one result
 * doc. If at least one suitable index remains, returns the name of the index with the fewest
 * fields. If not, returns boost::none.
 */
boost::optional<std::string> getIndexForExpressEquality(const CanonicalQuery& cq,
                                                        const QueryPlannerParams& plannerParams);

class PlanExecutorExpress;
class PlanExecutorExpressParams {
public:
    /**
     * Make the executor params for an express executor which will use the _id index or, if the
     * collection is clustered on _id, the clustered ordering, to answer the query.
     */
    static PlanExecutorExpressParams makeExecutorParamsForIdQuery(
        OperationContext* opCtx,
        VariantCollectionPtrOrAcquisition coll,
        boost::optional<ScopedCollectionFilter> collectionFilter,
        bool isClusteredOnId);

    /**
     * Make the executor params for an express executor which will use the index specified by
     * 'indexName' to answer the query.
     */
    static PlanExecutorExpressParams makeExecutorParamsForIndexedEqualityQuery(
        OperationContext* opCtx,
        VariantCollectionPtrOrAcquisition coll,
        boost::optional<ScopedCollectionFilter> collectionFilter,
        std::string indexName);

private:
    friend class PlanExecutorExpress;

    PlanExecutorExpressParams(OperationContext* opCtx,
                              VariantCollectionPtrOrAcquisition coll,
                              boost::optional<ScopedCollectionFilter> collectionFilter,
                              bool isClusteredOnId,
                              boost::optional<const std::string> indexName);

    OperationContext* _opCtx;
    VariantCollectionPtrOrAcquisition _coll;
    boost::optional<ScopedCollectionFilter> _collectionFilter;
    bool _isClusteredOnId;
    boost::optional<const std::string> _indexName;
};

/**
 * The Express executor supports bypassing normal query machinery in favor of going directly to
 * the index and record store for the equivalent of an IXSCAN + FETCH plan.
 *
 * It supports single-field equalities which can use an index on that field and will return at most
 * one document, including point queries on _id.
 */
class PlanExecutorExpress final : public PlanExecutor {
public:
    /**
     * Builds the express executor according to the executor params.
     *
     * If '_indexName' is specified, invariants if the index corresponding to '_indexName' does not
     * exist. Otherwise, if the collection is not clustered, invariants if the _id index does not
     * exist.
     */
    static std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> cq, PlanExecutorExpressParams params);

    ExecState getNext(BSONObj* out, RecordId* dlOut) override;

    ExecState getNextDocument(Document* objOut, RecordId* dlOut) override {
        BSONObj bsonDoc;
        auto state = getNext(&bsonDoc, dlOut);
        *objOut = Document(bsonDoc);
        return state;
    }

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

    void saveState() override {}

    void restoreState(const RestoreContext& context) override {
        _coll = context.collection();
    }

    void detachFromOperationContext() override {
        _opCtx = nullptr;
    }

    void reattachToOperationContext(OperationContext* opCtx) override {
        _opCtx = opCtx;
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

    QueryFramework getQueryFramework() const override {
        return PlanExecutor::QueryFramework::kClassicOnly;
    }

    bool usesCollectionAcquisitions() const override {
        return _coll.isAcquisition();
    }

    bool isEOF() override {
        return _done || isMarkedAsKilled();
    }

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

    const mongo::CommonStats* getCommonStats() const {
        return &_commonStats;
    }

private:
    PlanExecutorExpress(std::unique_ptr<CanonicalQuery> cq,
                        OperationContext* opCtx,
                        VariantCollectionPtrOrAcquisition coll,
                        boost::optional<ScopedCollectionFilter> collectionFilter,
                        bool isClusteredOnId,
                        boost::optional<const std::string> indexName);

    /**
     * Fast path for finding a RecordId: searches for the first RID where the first element of the
     * index has value 'pointBound'. Uses the clustered order to answer the query, if
     * '_isClusteredOnId'. Otherwise, uses the index identified by '_entry'.
     *
     * Will uassert() if the index indicated  by '_entry' has been dropped.
     */
    RecordId getRIDForPoint(const BSONElement& pointBound) const;


    OperationContext* _opCtx;
    std::unique_ptr<CanonicalQuery> _cq;
    bool _isDisposed{false};
    bool _done{false};
    const bool _isClusteredOnId;
    VariantCollectionPtrOrAcquisition _coll;

    // The index to use to answer the query. If _indexName is boost::none, we should use the
    // clustered ordering. Both _indexName and _entry are either present, or boost::none/nullptr.
    boost::optional<const std::string> _indexName;
    const IndexCatalogEntry* _entry;

    mongo::CommonStats _commonStats;
    const NamespaceString _nss;
    Status _killStatus = Status::OK();
    PlanExplainerExpress _planExplainer;
    std::vector<NamespaceStringOrUUID> _secondaryNss;
    boost::optional<ShardFiltererImpl> _shardFilterer;
};

}  // namespace mongo
