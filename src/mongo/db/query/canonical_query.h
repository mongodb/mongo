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
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/logical_model/projection/projection.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/logic/tribool.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class OperationContext;

struct CanonicalQueryParams {
    boost::intrusive_ptr<ExpressionContext> expCtx;
    std::variant<std::unique_ptr<ParsedFindCommand>, ParsedFindCommandParams> parsedFind;
    std::vector<boost::intrusive_ptr<DocumentSource>> pipeline = {};
    bool isCountLike = false;
    bool isSearchQuery = false;
};

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

    CanonicalQuery(CanonicalQueryParams&& params);

    /**
     * Deprecated factory method for creating CanonicalQuery.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> make(CanonicalQueryParams&& params);

    /**
     * Used for creating sub-queries from an existing CanonicalQuery, only for use by
     * 'makeForSubplanner()'.
     */
    CanonicalQuery(OperationContext* opCtx, const CanonicalQuery& baseQuery, size_t i);

    /**
     * Construct a 'CanonicalQuery' for a subquery of the given query. This function should only be
     * invoked by the subplanner. 'baseQuery' must contain a MatchExpression with rooted $or. This
     * function returns a 'CanonicalQuery' housing a copy of the i'th child of the root.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> makeForSubplanner(
        OperationContext* opCtx, const CanonicalQuery& baseQuery, size_t i);

    /**
     * Perform validation checks on the normalized 'root' which could not be checked before
     * normalization - those should happen in
     * parsed_find_command::validateAndGetAvailableMetadata().
     */
    static Status isValidNormalized(const MatchExpression* root);

    const NamespaceString& nss() const {
        invariant(_findCommand->getNamespaceOrUUID().isNamespaceString());
        return _findCommand->getNamespaceOrUUID().nss();
    }

    //
    // Accessors for the query
    //
    MatchExpression* getPrimaryMatchExpression() const {
        return _primaryMatchExpression.get();
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

    void resetSortPattern() {
        _findCommand->setSort(BSONObj());
        _sortPattern = boost::none;
    }

    void resetDistinct() {
        _distinct = boost::none;
    }

    const boost::optional<CanonicalDistinct>& getDistinct() const {
        return _distinct;
    }

    CanonicalDistinct* getDistinct() {
        return _distinct.get_ptr();
    }

    const CollatorInterface* getCollator() const {
        return _expCtx->getCollator();
    }

    std::shared_ptr<CollatorInterface> getCollatorShared() const {
        return _expCtx->getCollatorShared();
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

    void setDistinct(CanonicalDistinct&& distinct) {
        _distinct.emplace(std::move(distinct));
    }

    /**
     * Serializes this CanonicalQuery to a BSON object of the following form:
     * {
     *   filter: <filter object>,            // empty object if there is no filter
     *   projection: <projection object>,    // field only included if there is a projection
     *   sort: <sort object>                 // field only included if there is a sort
     * }
     *
     * This function is used for CQF explain purposes.
     */
    void serializeToBson(BSONObjBuilder* out) const;

    // Debugging
    std::string toString(bool forErrMsg = false) const;
    std::string toStringShort(bool forErrMsg = false) const;

    std::string toStringForErrorMsg() const {
        return toString(true);
    }
    std::string toStringShortForErrorMsg() const {
        return toStringShort(true);
    }

    boost::optional<ExplainOptions::Verbosity> getExplain() const {
        invariant(_expCtx);
        return _expCtx->getExplain();
    }

    void setSbeCompatible(bool sbeCompatible) {
        _sbeCompatible = sbeCompatible;
    }

    bool isSbeCompatible() const {
        return _sbeCompatible;
    }

    void setUsingSbePlanCache(bool usingSbePlanCache) {
        _usingSbePlanCache = usingSbePlanCache;
    }

    // setUsingSbePlanCache() must be invoked before this function.
    bool isUsingSbePlanCache() const {
        tassert(9421201,
                "_usingSbePlanCache should be initialized",
                !boost::indeterminate(_usingSbePlanCache));
        return static_cast<bool>(_usingSbePlanCache);
    }

    bool isParameterized() const {
        return !_inputParamIdToExpressionMap.empty();
    }

    const std::vector<const MatchExpression*>& getInputParamIdToMatchExpressionMap() const {
        return _inputParamIdToExpressionMap;
    }

    bool getDisablePlanCache() const {
        return _disablePlanCache;
    }

    boost::optional<size_t> getMaxMatchExpressionParams() const {
        return _maxMatchExpressionParams;
    }

    bool getForceGenerateRecordId() const {
        return _forceGenerateRecordId;
    }

    void setForceGenerateRecordId(bool value) {
        _forceGenerateRecordId = value;
    }

    OperationContext* getOpCtx() const {
        tassert(6508300, "'CanonicalQuery' does not have an 'ExpressionContext'", _expCtx);
        return _expCtx->getOperationContext();
    }

    auto& getExpCtx() const {
        return _expCtx;
    }
    auto getExpCtxRaw() const {
        return _expCtx.get();
    }

    void setCqPipeline(std::vector<boost::intrusive_ptr<DocumentSource>> cqPipeline,
                       bool containsEntirePipeline);

    const std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline() const {
        return _cqPipeline;
    }

    std::vector<boost::intrusive_ptr<DocumentSource>>& cqPipeline() {
        return _cqPipeline;
    }

    bool containsEntirePipeline() const {
        return _containsEntirePipeline;
    }

    /**
     * Returns true if the query is a count-like query, i.e. has no dependencies on inputs (see
     * DepsTracker::hasNoRequirements()). These queries can be served without accessing the source
     * documents (e.g. {$group: {_id: null, c: {$min: 42}}}) in which case we might be able to avoid
     * scanning the collection and instead use COUNT_SCAN or other optimizations.
     *
     * Note that this applies to the find/non-pipeline portion of the query. If the count-like group
     * is pushed down, later execution stages cannot be treated like a count. In other words, a
     * query with a pushed-down group may be considered a count at the data access layer but not
     * above the canonical query.
     */
    bool isCountLike() const {
        return _isCountLike;
    }

    /**
     * Called to indicate the query execution plan should not be cached for SBE. See comments on the
     * '_isUncacheableSbe' member for more details.
     */
    void setUncacheableSbe() {
        _isUncacheableSbe = true;
    }

    /**
     * Check if the query execution plan should not be cached for SBE. See comments on the
     * '_isUncacheableSbe' member for more details.
     */
    bool isUncacheableSbe() const {
        return _isUncacheableSbe;
    }

    /**
     * Tests whether a 'matchExpr' from this query should be parameterized for the SBE plan cache.
     */
    bool shouldParameterizeSbe(MatchExpression* matchExpr) const;

    /**
     * Tests if limit and skip amounts from find command request should be parameterized for the SBE
     * plan cache.
     */
    bool shouldParameterizeLimitSkip() const;

    /**
     * Add parameters for match expressions that were pushed down via '_cqPipeline'.
     */
    void addMatchParams(const std::vector<const MatchExpression*>& newParams) {
        _inputParamIdToExpressionMap.insert(
            _inputParamIdToExpressionMap.end(), newParams.begin(), newParams.end());
    }

    int numParams() {
        return _inputParamIdToExpressionMap.size();
    }

    // Return true if the cqPipeline starts with $search or $searchMeta.
    bool isSearchQuery() const {
        return _isSearchQuery;
    }

    bool isExplainAndCacheIneligible() const {
        return getExplain() && !getExpCtxRaw()->getInLookup();
    }

    void optimizeProjection() {
        if (_proj) {
            _proj->optimize();
        }
    }

    /**
     * Indicates whether this query was created specifically for the sub planner.
     */
    bool forSubPlanner() const {
        return _forSubPlanner;
    }

    /**
     * Return the solution hash for the forced plan.
     */
    boost::optional<int64_t> getForcedPlanSolutionHash() const {
        return _findCommand->getForcedPlanSolutionHash();
    }

private:
    void initCq(boost::intrusive_ptr<ExpressionContext> expCtx,
                std::unique_ptr<ParsedFindCommand> parsedFind,
                std::vector<boost::intrusive_ptr<DocumentSource>> cqPipeline,
                bool isCountLike,
                bool isSearchQuery,
                bool optimizeMatchExpression);

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    std::unique_ptr<FindCommandRequest> _findCommand;

    // The match expression at the base of the query tree.
    std::unique_ptr<MatchExpression> _primaryMatchExpression;

    boost::optional<projection_ast::Projection> _proj;

    boost::optional<SortPattern> _sortPattern;

    boost::optional<CanonicalDistinct> _distinct;

    // A query can include a post-processing pipeline here. Logically it is applied after all the
    // other operations (filter, sort, project, skip, limit).
    std::vector<boost::intrusive_ptr<DocumentSource>> _cqPipeline;

    // True iff '_cqPipeline' contains all aggregation pipeline stages in the query. When
    // '_containsEntirePipeline' is false, the output of '_cqPipeline' may need to be processed by
    // further 'DocumentSource' stages.
    bool _containsEntirePipeline{false};

    // Keeps track of what metadata has been explicitly requested.
    QueryMetadataBitSet _metadataDeps;

    // True if this query can be executed by the SBE.
    bool _sbeCompatible = false;

    // Indicate whether this query will be cached using the SBE plan cache.
    // Use a tribool because this value is uninitialized for a large part of the life of a
    // CanonicalQuery. If this value is not boost::indeterminate, that means it has been not set.
    // We chose to use a tribool instead of optional<bool> to avoid confusion of operator bool.
    boost::tribool _usingSbePlanCache = boost::indeterminate;

    // True if this query must produce a RecordId output in addition to the BSON objects that
    // constitute the result set of the query. Any generated query solution must not discard record
    // ids, even if the optimizer detects that they are not going to be consumed downstream.
    bool _forceGenerateRecordId = false;

    // Tells whether plan caching is disabled.
    bool _disablePlanCache = false;

    // A map from assigned InputParamId's to parameterised MatchExpression's.
    std::vector<const MatchExpression*> _inputParamIdToExpressionMap;

    // This limits the number of MatchExpression parameters we create for the CanonicalQuery before
    // stopping. (We actually stop at this + 1.) A value of boost::none means unlimited.
    boost::optional<size_t> _maxMatchExpressionParams = boost::none;

    // "True" for queries that after doing a scan of an index can produce an empty document and
    // still be correct. For example, this applies to queries like [{$match: {x: 42}}, {$count:
    // "c"}] in presence of index on "x". The stage that follows the index scan doesn't have to be
    // $count but it must have no dependencies on the fields from the prior stages. Note, that
    // [{$match: {x: 42}}, {$group: {_id: "$y"}}, {$count: "c"}]] is _not_ "count like" because
    // the first $group stage needs to access field "y" and this access cannot be incorporated into
    // the index scan.
    bool _isCountLike = false;

    // If true, indicates that we should not cache this plan in the SBE plan cache. This gets set to
    // true if a MatchExpression was not parameterized because it contains a large number of
    // predicates (usally > 512). This flag can be reused for additional do-not-cache conditions in
    // the future.
    bool _isUncacheableSbe = false;

    bool _isSearchQuery = false;

    bool _forSubPlanner = false;
};

}  // namespace mongo
