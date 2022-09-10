/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/base/status.h"
#include "mongo/db/cst/c_node.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/inner_pipeline_stage_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/projection.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/sort_pattern.h"

namespace mongo {

class OperationContext;

class CanonicalQuery {
public:
    // A type that encodes the notion of query shape suitable for use with the plan cache. Encodes
    // the query's match, projection, sort, etc. potentially with some constants removed or replaced
    // with parameter markers.
    typedef std::string QueryShapeString;
    // A second encoding of query shape similar to 'QueryShapeString' above, except designed to work
    // with index filters and the 'planCacheClear' command. A caller can encode a query into an
    // 'PlanCacheCommandKey' in order to look for for matching index filters that should apply to
    // the query, or plan cache entries to which the 'planCacheClear' command should be applied.
    typedef std::string PlanCacheCommandKey;

    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * 'opCtx' must point to a valid OperationContext, but 'opCtx' does not need to outlive the
     * returned CanonicalQuery.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        OperationContext* opCtx,
        std::unique_ptr<FindCommandRequest> findCommand,
        bool explain = false,
        const boost::intrusive_ptr<ExpressionContext>& expCtx = nullptr,
        const ExtensionsCallback& extensionsCallback = ExtensionsCallbackNoop(),
        MatchExpressionParser::AllowedFeatureSet allowedFeatures =
            MatchExpressionParser::kDefaultSpecialFeatures,
        const ProjectionPolicies& projectionPolicies = ProjectionPolicies::findProjectionPolicies(),
        std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline = {});

    /**
     * For testing or for internal clients to use.
     */

    /**
     * Used for creating sub-queries from an existing CanonicalQuery.
     *
     * 'root' must be an expression in baseQuery.root().
     *
     * Does not take ownership of 'root'.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                                    const CanonicalQuery& baseQuery,
                                                                    MatchExpression* root);

    /**
     * Returns true if "query" describes an exact-match query on _id.
     */
    static bool isSimpleIdQuery(const BSONObj& query);

    /**
     * Validates the match expression 'root' as well as the query specified by 'request', checking
     * for illegal combinations of operators. Returns a non-OK status if any such illegal
     * combination is found.
     *
     * This method can be called both on normalized and non-normalized 'root'. However, some checks
     * can only be performed once the match expressions is normalized. To perform these checks one
     * can call 'isValidNormalized()'.
     *
     * On success, returns a bitset indicating which types of metadata are *unavailable*. For
     * example, if 'root' does not contain a $text predicate, then the returned metadata bitset will
     * indicate that text score metadata is unavailable. This means that if subsequent
     * $meta:"textScore" expressions are found during analysis of the query, we should raise in an
     * error.
     */
    static StatusWith<QueryMetadataBitSet> isValid(const MatchExpression* root,
                                                   const FindCommandRequest& findCommand);

    /**
     * Perform additional validation checks on the normalized 'root'.
     */
    static Status isValidNormalized(const MatchExpression* root);

    NamespaceString nss() const {
        invariant(_findCommand->getNamespaceOrUUID().nss());
        return *_findCommand->getNamespaceOrUUID().nss();
    }
    std::string ns() const {
        return nss().ns();
    }

    //
    // Accessors for the query
    //
    MatchExpression* root() const {
        return _root.get();
    }
    const BSONObj& getQueryObj() const {
        return _findCommand->getFilter();
    }
    const FindCommandRequest& getFindCommandRequest() const {
        return *_findCommand;
    }

    /**
     * Returns the projection, or nullptr if none.
     */
    const projection_ast::Projection* getProj() const {
        return _proj.get_ptr();
    }

    projection_ast::Projection* getProj() {
        return _proj.get_ptr();
    }

    const boost::optional<SortPattern>& getSortPattern() const {
        return _sortPattern;
    }

    const CollatorInterface* getCollator() const {
        return _expCtx->getCollator();
    }

    /**
     * Returns a bitset indicating what metadata has been requested in the query.
     */
    const QueryMetadataBitSet& metadataDeps() const {
        return _metadataDeps;
    }

    /**
     * Allows callers to request metadata in addition to that needed as part of the query.
     */
    void requestAdditionalMetadata(const QueryMetadataBitSet& additionalDeps) {
        _metadataDeps |= additionalDeps;
    }

    /**
     * Compute the "shape" of this query by encoding the match, projection and sort, and stripping
     * out the appropriate values. Note that different types of PlanCache use different encoding
     * approaches.
     */
    QueryShapeString encodeKey() const;

    /**
     * Similar to 'encodeKey()' above, but intended for use with plan cache commands rather than
     * the plan cache itself.
     */
    PlanCacheCommandKey encodeKeyForPlanCacheCommand() const;

    /**
     * Sets this CanonicalQuery's collator, and sets the collator on this CanonicalQuery's match
     * expression tree.
     *
     * This setter can be used to override the collator that was created from the query request
     * during CanonicalQuery construction.
     */
    void setCollator(std::unique_ptr<CollatorInterface> collator);

    // Debugging
    std::string toString() const;
    std::string toStringShort() const;

    /**
     * Returns a count of 'type' nodes in expression tree.
     */
    static size_t countNodes(const MatchExpression* root, MatchExpression::MatchType type);

    /**
     * Returns true if this canonical query may have converted extensions such as $where and $text
     * into no-ops during parsing. This will be the case if it allowed $where and $text in parsing,
     * but parsed using an ExtensionsCallbackNoop. This does not guarantee that a $where or $text
     * existed in the query.
     *
     * Queries with a no-op extension context are special because they can be parsed and planned,
     * but they cannot be executed.
     */
    bool canHaveNoopMatchNodes() const {
        return _canHaveNoopMatchNodes;
    }

    bool getExplain() const {
        return _explain;
    }

    bool getForceClassicEngine() const {
        return _forceClassicEngine;
    }

    void setSbeCompatible(bool sbeCompatible) {
        _sbeCompatible = sbeCompatible;
    }

    bool isSbeCompatible() const {
        return _sbeCompatible;
    }

    bool isParameterized() const {
        return !_inputParamIdToExpressionMap.empty();
    }

    const std::vector<const MatchExpression*>& getInputParamIdToMatchExpressionMap() const {
        return _inputParamIdToExpressionMap;
    }

    void setExplain(bool explain) {
        _explain = explain;
    }

    OperationContext* getOpCtx() const {
        tassert(6508300, "'CanonicalQuery' does not have an 'ExpressionContext'", _expCtx);
        return _expCtx->opCtx;
    }

    auto& getExpCtx() const {
        return _expCtx;
    }
    auto getExpCtxRaw() const {
        return _expCtx.get();
    }

    void setPipeline(std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline) {
        _pipeline = std::move(pipeline);
    }

    const std::vector<std::unique_ptr<InnerPipelineStageInterface>>& pipeline() const {
        return _pipeline;
    }

private:
    // You must go through canonicalize to create a CanonicalQuery.
    CanonicalQuery() {}

    Status init(OperationContext* opCtx,
                boost::intrusive_ptr<ExpressionContext> expCtx,
                std::unique_ptr<FindCommandRequest> findCommand,
                bool canHaveNoopMatchNodes,
                std::unique_ptr<MatchExpression> root,
                const ProjectionPolicies& projectionPolicies,
                std::vector<std::unique_ptr<InnerPipelineStageInterface>> pipeline);

    // Initializes '_sortPattern', adding any metadata dependencies implied by the sort.
    //
    // Throws a UserException if the sort is illegal, or if any metadata type in
    // 'unavailableMetadata' is required.
    void initSortPattern(QueryMetadataBitSet unavailableMetadata);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<FindCommandRequest> _findCommand;

    std::unique_ptr<MatchExpression> _root;

    boost::optional<projection_ast::Projection> _proj;

    boost::optional<SortPattern> _sortPattern;

    // A query can include a post-processing pipeline here. Logically it is applied after all the
    // other operations (filter, sort, project, skip, limit).
    std::vector<std::unique_ptr<InnerPipelineStageInterface>> _pipeline;

    // Keeps track of what metadata has been explicitly requested.
    QueryMetadataBitSet _metadataDeps;

    bool _canHaveNoopMatchNodes = false;

    bool _explain = false;

    // Determines whether the classic engine must be used.
    bool _forceClassicEngine = true;

    // True if this query can be executed by the SBE.
    bool _sbeCompatible = false;

    // A map from assigned InputParamId's to parameterised MatchExpression's.
    std::vector<const MatchExpression*> _inputParamIdToExpressionMap;
};

}  // namespace mongo
