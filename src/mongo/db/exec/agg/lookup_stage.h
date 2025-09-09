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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sequential_document_cache.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"

#include <cstddef>
#include <memory>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the lookup aggregation stage and is part of the
 * execution pipeline. Its construction is based on DocumentSourceLookUp, which handles the
 * optimization part.
 */
class LookUpStage final : public Stage {
public:
    LookUpStage(StringData stageName,
                const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                boost::intrusive_ptr<ExpressionContext> fromExpCtx,
                NamespaceString fromNs,
                FieldPath as,
                boost::optional<FieldPath> localField,
                boost::optional<FieldPath> foreignField,
                boost::optional<size_t> fieldMatchPipelineIdx,
                std::vector<LetVariable> letVariables,
                Variables variables,
                VariablesParseState variablesParseState,
                bool hasUserPipeline,
                bool hasAbsorbedUnwindSrc,
                boost::optional<FieldPath> unwindIndexPathField,
                bool unwindPreserveNullsAndEmptyArrays,
                BSONObj additionalFilter,
                std::shared_ptr<LookUpSharedState> sharedState);

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

    bool validateOperationContext(const OperationContext* opCtx) const final;

    bool usedDisk() const final;

    const SpecificStats* getSpecificStats() const final {
        return &_stats;
    }

    Document getExplainOutput(
        const SerializationOptions& opts = SerializationOptions{}) const final;

    /**
     * Builds the $lookup pipeline and resolves any variables using the passed 'inputDoc', adding a
     * cursor and/or cache source as appropriate.
     */
    // TODO SERVER-84208: Refactor this method so as to clearly separate the logic for the streams
    // engine from the logic for the classic $lookup.
    template <bool isStreamsEngine = false>
    std::unique_ptr<mongo::Pipeline> buildPipeline(
        const boost::intrusive_ptr<ExpressionContext>& fromExpCtx, const Document& inputDoc);

    /**
     * Reinitialize the cache with a new max size. May only be called if this DSLookup was created
     * with pipeline syntax only, the cache has not been frozen or abandoned, and no data has been
     * added to it.
     */
    void reInitializeCache_forTest(size_t maxCacheSizeBytes);

private:
    GetNextResult doGetNext() final;

    void doDispose() final;

    /**
     * Delegate of doGetNext() in the case where an $unwind stage has been absorbed into _unwindSrc.
     * This returns the next record resulting from unwinding the lookup's "as" field.
     */
    GetNextResult unwindResult();

    /**
     * Builds the $lookup pipeline using the resolved view definition for a sharded foreign view and
     * updates the '_sharedState->resolvedPipeline', as well as '_fieldMatchPipelineIdx' in the case
     * of a 'foreign' join.
     */
    std::unique_ptr<mongo::Pipeline> buildPipelineFromViewDefinition(
        std::vector<BSONObj> serializedPipeline, ResolvedNamespace resolvedNamespace);

    /**
     * Method to add a DocumentSourceSequentialDocumentCache stage and optimize the pipeline to
     * move the cache to its final position.
     */
    void addCacheStageAndOptimize(mongo::Pipeline& pipeline);

    bool hasLocalFieldForeignFieldJoin() const {
        return _localField != boost::none;
    }

    /**
     * Returns true if we are not in a transaction.
     */
    bool foreignShardedLookupAllowed() const;

    /**
     * Resolves let defined variables against 'localDoc' and stores the results in 'variables'.
     */
    void resolveLetVariables(const Document& localDoc, Variables* variables);

    // The ExpressionContext used when performing aggregation pipelines against the '_resolvedNs'
    // namespace.
    boost::intrusive_ptr<ExpressionContext> _fromExpCtx;

    NamespaceString _fromNs;

    // Path to the "as" field of the $lookup where the matches output array will be created.
    FieldPath _as;

    // For use when $lookup is specified with localField/foreignField syntax.
    boost::optional<FieldPath> _localField;
    boost::optional<FieldPath> _foreignField;
    // Indicates the index in '_sharedState->resolvedPipeline' where the local/foreignField $match
    // resides.
    boost::optional<size_t> _fieldMatchPipelineIdx;

    // Holds 'let' variables defined in $lookup stage. 'let' variables are stored in the vector in
    // order to ensure the stability in the query shape serialization.
    std::vector<LetVariable> _letVariables;

    // Holds 'let' defined variables defined both in this stage and in parent pipelines.
    // These are copied to the '_fromExpCtx' ExpressionContext's 'variables' and
    // 'variablesParseState' for use in foreign pipeline execution.
    Variables _variables;
    VariablesParseState _variablesParseState;

    bool _hasUserPipeline;

    bool _hasAbsorbedUnwindSrc;
    boost::optional<FieldPath> _unwindIndexPathField;
    bool _unwindPreserveNullsAndEmptyArrays;

    BSONObj _additionalFilter;

    std::shared_ptr<LookUpSharedState> _sharedState;

    // The following members are used to hold onto state across getNext() calls when '_unwindSrc' is
    // not null.
    long long _cursorIndex = 0;
    boost::optional<Document> _input;
    boost::optional<Document> _nextValue;

    DocumentSourceLookupStats _stats;

    // Caches documents returned by the non-correlated prefix of the $lookup pipeline during the
    // first iteration, up to a specified size limit in bytes. If this limit is not exceeded by the
    // time we hit EOF, subsequent iterations of the pipeline will draw from the cache rather than
    // from a cursor source.
    std::shared_ptr<SequentialDocumentCache> _cache;
};

}  // namespace mongo::exec::agg
