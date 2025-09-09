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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/javascript_execution.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_configuration.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/version_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/filesystem/path.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class FindCommandRequest;
class DistinctCommandRequest;
class AggregateCommandRequest;


enum struct SbeCompatibility {
    // Not implemented in SBE.
    notCompatible,
    // Requires "featureFlagSbeFull" to be set. New SBE features which are under
    // development can live under this feature flag until they are ready to be shipped.
    requiresSbeFull,
    // Requires the framework control knob to be "trySbeEngine". Fully tested, complete
    // SBE features belong here.
    requiresTrySbe,
    // Used for the narrow feature set that we expose when "trySbeRestricted" is on. These features
    // are always SBE compatible unless SBE is completely disabled with "forceClassicEngine".
    noRequirements,
};

// Indicator to a pipeline how/if the consumer intends to merge its output.
enum class MergeType {
    // Consumer will not merge the output of this pipeline with another one. $group accumulators
    // should produce full results and $sortKey does not need to be populated.
    noMerge,
    // Consumer will merge the output of this pipeline with another one in an order-insensitive way.
    // $group accumulators should produce partial results but $sortKey does not need to be
    // populated.
    unsortedMerge,
    // Consumer will merge the output of this pipeline with another one using merge sort. $group
    // accumulator should produce partial results and $sortKey must be populated.
    sortedMerge,
};

std::ostream& operator<<(std::ostream& os, SbeCompatibility sbeCompat);

struct ResolvedNamespace {
    ResolvedNamespace() = default;
    ResolvedNamespace(NamespaceString ns,
                      std::vector<BSONObj> pipeline,
                      boost::optional<UUID> uuid = boost::none,
                      bool involvedNamespaceIsAView = false);

    NamespaceString ns;
    std::vector<BSONObj> pipeline;
    boost::optional<UUID> uuid = boost::none;
    bool involvedNamespaceIsAView = false;
    // TODO (SERVER-100170): Add a LiteParsedPipeline member. We often need this information when
    // resolving views and currently recompute the object every time it's requested. Once added, go
    // through the rest of the codebase to ensure that we aren't unnecessarily creating a
    // LiteParsedPipeline object when it's already being stored here.
};

using ResolvedNamespaceMap = absl::flat_hash_map<NamespaceString, ResolvedNamespace>;

enum class ExpressionContextCollationMatchesDefault { kYes, kNo };

class ExpressionContext : public RefCountable {
public:
    /**
     * An RAII type that will temporarily change the ExpressionContext's collator. Resets the
     * collator to the previous value upon destruction.
     */
    class CollatorStash {
    public:
        /**
         * Resets the collator on '_expCtx' to the original collator present at the time this
         * CollatorStash was constructed.
         */
        ~CollatorStash();

    private:
        /**
         * Temporarily changes the collator on 'expCtx' to be 'newCollator'. The collator will be
         * set back to the original value when this CollatorStash is deleted.
         *
         * This constructor is private, all CollatorStashes should be created by calling
         * ExpressionContext::temporarilyChangeCollator().
         */
        CollatorStash(ExpressionContext* expCtx, std::unique_ptr<CollatorInterface> newCollator);

        friend class ExpressionContext;

        boost::intrusive_ptr<ExpressionContext> _expCtx;

        std::shared_ptr<CollatorInterface> _originalCollator;
    };

    /**
     * The structure ExpressionCounters encapsulates counters for match, aggregate, and other
     * expression types as seen in end-user queries.
     */
    struct ExpressionCounters {
        StringMap<uint64_t> aggExprCountersMap;
        StringMap<uint64_t> matchExprCountersMap;
        StringMap<uint64_t> groupAccumulatorExprCountersMap;
        StringMap<uint64_t> windowAccumulatorExprCountersMap;
    };

    // Keep track of the server-side Javascript operators used in the query. These flags will be set
    // at parse time if applicable.
    struct ServerSideJsConfig {
        bool accumulator = false;
        bool function = false;
        bool where = false;
    };

    // TODO: variables are heavily used everywhere, move these inside ExpressionContextParams at
    // some point
    Variables variables;
    VariablesParseState variablesParseState;

    /**
     * Used by a pipeline to check for interrupts so that killOp() works. Throws a UserAssertion if
     * this aggregation pipeline has been interrupted.
     */
    MONGO_COMPILER_ALWAYS_INLINE void checkForInterrupt() {
        _interruptChecker.checkForInterrupt();
    }

    /**
     * Returns true if this is a collectionless aggregation on the specified database.
     */
    bool isDBAggregation(const NamespaceString& other) const {
        return _params.ns.isEqualDb(other) && _params.ns.isCollectionlessAggregateNS();
    }

    /**
     * Returns true if this is a collectionless aggregation on the 'admin' database.
     */
    bool isClusterAggregation() const {
        return _params.ns.isAdminDB() && _params.ns.isCollectionlessAggregateNS();
    }

    /**
     * Returns true if this aggregation is running on a single, specific namespace.
     */
    bool isSingleNamespaceAggregation() const {
        return !_params.ns.isCollectionlessAggregateNS();
    }

    const CollatorInterface* getCollator() const {
        return _collator.getCollator();
    }

    std::shared_ptr<CollatorInterface> getCollatorShared() const {
        return _collator.getCollatorShared();
    }

    /**
     * Whether to track timing information and "work" counts in the agg layer.
     */
    bool shouldCollectDocumentSourceExecStats() const {
        return static_cast<bool>(_params.explain);
    }

    /**
     * Returns the BSON spec for the ExpressionContext's collator, or the simple collator spec if
     * the collator is null.
     *
     * The ExpressionContext is always set up with the fully-resolved collation. So even though
     * SERVER-24433 describes an ambiguity between a null collator, here we can say confidently that
     * null must mean simple since we have already handled "absence of a collator" before creating
     * the ExpressionContext.
     */
    BSONObj getCollatorBSON() const {
        if (_collator.getIgnore()) {
            return BSONObj();
        }
        auto* collator = _collator.getCollator();
        return collator ? collator->getSpec().toBSON() : CollationSpec::kSimpleSpec;
    }

    /**
     * Sets '_collator' and resets 'documentComparator' and 'valueComparator'.
     *
     * Use with caution - '_collator' is used in the context of a Pipeline, and it is illegal
     * to change the collation once a Pipeline has been parsed with this ExpressionContext.
     */
    void setCollator(std::shared_ptr<CollatorInterface> collator) {
        _collator.setCollator(std::move(collator));

        // Document/Value comparisons must be aware of the collation.
        auto* ptr = _collator.getCollator();
        _documentComparator = DocumentComparator(ptr);
        _valueComparator = ValueComparator(ptr);
    }

    const DocumentComparator& getDocumentComparator() const {
        return _documentComparator;
    }

    const ValueComparator& getValueComparator() const {
        return _valueComparator;
    }

    /**
     * Temporarily resets the collator to be 'newCollator'. Returns a CollatorStash which will reset
     * the collator back to the old value upon destruction.
     */
    std::unique_ptr<CollatorStash> temporarilyChangeCollator(
        std::unique_ptr<CollatorInterface> newCollator);

    /**
     * Returns the ResolvedNamespace corresponding to 'nss'. It is an error to call this method on a
     * namespace not involved in the pipeline.
     */
    const ResolvedNamespace& getResolvedNamespace(const NamespaceString& nss) const {
        auto it = _params.resolvedNamespaces.find(nss);
        tassert(9453000,
                str::stream() << "No resolved namespace provided for " << nss.toStringForErrorMsg(),
                it != _params.resolvedNamespaces.end());
        return it->second;
    }

    const ResolvedNamespaceMap& getResolvedNamespaces() const {
        return _params.resolvedNamespaces;
    }

    bool hasResolvedNamespace(const NamespaceString& nss) const {
        auto it = _params.resolvedNamespaces.find(nss);
        return it != _params.resolvedNamespaces.end();
    }

    /**
     * Returns true if there are no namespaces in the query other than the namespace the query was
     * issued against. eg if there is no $out, $lookup ect. If namespaces have not yet been resolved
     * then it will also return false.
     */
    bool noForeignNamespaces() const {
        return _params.resolvedNamespaces.empty();
    }

    /**
     * Returns true if the tailableMode indicates a tailable
     * query.
     */
    bool isTailable() const {
        return _params.tailableMode == TailableModeEnum::kTailableAndAwaitData ||
            _params.tailableMode == TailableModeEnum::kTailable;
    }

    /**
     * Convenience call that returns true if the tailableMode indicates a tailable and awaitData
     * query.
     */
    bool isTailableAwaitData() const {
        return _params.tailableMode == TailableModeEnum::kTailableAndAwaitData;
    }

    /**
     * Returns true if the pipeline is eligible for query sampling for the purpose of shard key
     * selection metrics.
     */
    bool eligibleForSampling() const {
        return !_params.explain;
    }

    void setResolvedNamespaces(ResolvedNamespaceMap resolvedNamespaces) {
        _params.resolvedNamespaces = std::move(resolvedNamespaces);
    }

    void addResolvedNamespace(const NamespaceString& nss, const ResolvedNamespace& resolvedNs) {
        auto it = _params.resolvedNamespaces.find(nss);

        // Assert that the resolved namespace we are adding either doesn't exist in the map or we
        // are reassigning the same value (no modification allowed). Only perform the uuid check if
        // both uuids exist.
        uassert(9825500,
                "Cannot overwrite an existing namespace with a different value",
                it == _params.resolvedNamespaces.end() ||
                    (it->second.involvedNamespaceIsAView == resolvedNs.involvedNamespaceIsAView &&
                     it->second.ns == resolvedNs.ns &&
                     (!it->second.uuid.has_value() || !resolvedNs.uuid.has_value() ||
                      it->second.uuid.value() == resolvedNs.uuid.value())));

        _params.resolvedNamespaces[nss] = resolvedNs;
    }

    void addResolvedNamespaces(
        const mongo::stdx::unordered_set<mongo::NamespaceString>& resolvedNamespaces) {
        for (const auto& nss : resolvedNamespaces) {
            _params.resolvedNamespaces.try_emplace(nss, nss, std::vector<BSONObj>{});
        }
    }

    void setIsCappedDelete() {
        _isCappedDelete = true;
    }

    bool getIsCappedDelete() const {
        return _isCappedDelete;
    }

    /**
     * Retrieves the Javascript Scope for the current thread or creates a new one if it has not been
     * created yet. Initializes the Scope with the 'jsScope' variables from the runtimeConstants.
     * Loads the Scope with the functions stored in system.js if the expression isn't executed on
     * mongos and is called from a MapReduce command or `forceLoadOfStoredProcedures` is true.
     */
    JsExecution* getJsExecWithScope(bool forceLoadOfStoredProcedures = false) const {
        uassert(31264,
                "Cannot run server-side javascript without the javascript engine enabled",
                getGlobalScriptEngine());
        const auto isMapReduce =
            (variables.hasValue(Variables::kIsMapReduceId) &&
             variables.getValue(Variables::kIsMapReduceId).getType() == BSONType::boolean &&
             variables.getValue(Variables::kIsMapReduceId).coerceToBool());
        if (_params.inRouter) {
            invariant(!forceLoadOfStoredProcedures);
            invariant(!isMapReduce);
        }

        // Stored procedures are only loaded for the $where expression and MapReduce command.
        const bool loadStoredProcedures = forceLoadOfStoredProcedures || isMapReduce;

        if (_params.hasWhereClause && !loadStoredProcedures) {
            uasserted(4649200,
                      "A single operation cannot use both JavaScript aggregation expressions and "
                      "$where.");
        }

        // If there is a cached JsExecution object, return it. This is a performance optimization
        // that avoids potentially copying the scope object, which is not needed if a cached exec
        // object already exists.
        JsExecution* jsExec = JsExecution::getCached(_params.opCtx, loadStoredProcedures);
        if (jsExec) {
            return jsExec;
        }

        BSONObj scopeObj = BSONObj();
        if (variables.hasValue(Variables::kJsScopeId)) {
            Value scopeVar = variables.getValue(Variables::kJsScopeId);
            invariant(scopeVar.isObject());
            scopeObj = scopeVar.getDocument().toBson();
        }
        return JsExecution::get(_params.opCtx,
                                scopeObj,
                                _params.ns.dbName(),
                                loadStoredProcedures,
                                _params.jsHeapLimitMB);
    }

    /**
     * Create optional internal expression counters and start counting.
     */
    void startExpressionCounters();

    /**
     * Increment the counter for the match expression with a given name.
     */
    void incrementMatchExprCounter(StringData name);

    /**
     * Increment the counter for the aggregate expression with a given name.
     */
    void incrementAggExprCounter(StringData name);

    /**
     * Increment the counter for the $group accumulator expression with a given name.
     */
    void incrementGroupAccumulatorExprCounter(StringData name);

    /**
     * Increment the counter for the $setWindowFields accumulator expression with a given name.
     */
    void incrementWindowAccumulatorExprCounter(StringData name);

    /**
     * Merge expression counters from the current expression context into the global maps
     * and stop counting.
     */
    void stopExpressionCounters();

    bool expressionCountersAreActive() const {
        return static_cast<bool>(_expressionCounters);
    }

    /**
     * Initializes the value of system variables that are referenced by the query. This allows for
     * lazy initialization of resources which may be expensive to construct (e.g. constructing
     * cluster timestamp invokes taking a mutex). This function should be invoked after the parsing
     * of all aggregation expressions in the query.
     */
    void initializeReferencedSystemVariables();

    /**
     * Record that we have seen the given system variable in the query. Used for lazy initialization
     * of variables.
     */
    void setSystemVarReferencedInQuery(Variables::Id var) {
        tassert(7612600,
                "Cannot track references to user-defined variables.",
                !Variables::isUserDefinedVariable(var));
        _systemVarsReferencedInQuery.insert(var);
    }

    /**
     * Returns true if the given system variable is referenced in the query and false otherwise.
     */
    bool isSystemVarReferencedInQuery(Variables::Id var) const {
        tassert(
            7612601,
            "Cannot access whether a variable is referenced to or not for a user-defined variable.",
            !Variables::isUserDefinedVariable(var));
        return _systemVarsReferencedInQuery.count(var);
    }

    /**
     * Throws if the provided feature flag is not enabled according to the expressions
     * VersionContext and IncrementalFeatureRolloutContext. This function assumes the caller has
     * verified that the feature flag should be checked.
     */
    void throwIfParserShouldRejectFeature(StringData name, FeatureFlag& flag);

    /**
     * Returns true if parsers should not check if feature flags are enabled on the expressions
     * VersionContext or IncrementalFeatureRolloutContext and false otherwise.
     *
     * TODO SERVER-99552 remove function when support is added for 'VersionContext' for all DDL
     * operations in replica sets. Currently, secondaries can fail to apply oplog entries when
     * creating collections that have validators or views that depend on expressions with new query
     * features.
     */
    bool shouldParserIgnoreFeatureFlagCheck() const {
        tassert(10499200, "Operation context is not initialized", _params.opCtx);
        return (_params.isParsingCollectionValidator || _params.isParsingViewDefinition) &&
            !_params.opCtx->isEnforcingConstraints();
    }

    /**
     * Throws only if the parser should check the feature flag and the feature flag provided is not
     * enabled in the expressions VersionContext and IncrementalFeatureRolloutContext
     */
    void ignoreFeatureInParserOrRejectAndThrow(StringData name, FeatureFlag& flag);

    void setOperationContext(OperationContext* opCtx) {
        _params.opCtx = opCtx;
    }

    OperationContext* getOperationContext() const {
        return _params.opCtx;
    }

    VersionContext& getVersionContext() {
        return _params.vCtx;
    }

    const VersionContext& getVersionContext() const {
        return _params.vCtx;
    }

    IncrementalFeatureRolloutContext& getIfrContext() {
        return _params.ifrContext;
    }

    const IncrementalFeatureRolloutContext& getIfrContext() const {
        return _params.ifrContext;
    }

    const NamespaceString& getNamespaceString() const {
        return _params.ns;
    }

    const NamespaceString& getUserNss() const {
        return _params.originalNs;
    }

    void setUserNss(NamespaceString ns) {
        _params.originalNs = std::move(ns);
    }

    void setNamespaceString(NamespaceString ns) {
        _params.ns = std::move(ns);
    }

    boost::optional<UUID> getUUID() const {
        return _params.collUUID;
    }

    void setUUID(boost::optional<UUID> uuid) {
        _params.collUUID = std::move(uuid);
    }

    // TODO SERVER-107416: Delete 'getNeedsMerge()' once all callers use 'needsUnsortedMerge()' or
    // 'needsSortedMerge()'.
    bool getNeedsMerge() const {
        return _params.mergeType == MergeType::unsortedMerge ||
            _params.mergeType == MergeType::sortedMerge;
    }

    void setNeedsMerge(bool needsMerge) {
        _params.mergeType = needsMerge ? MergeType::sortedMerge : MergeType::noMerge;
    }

    bool needsUnsortedMerge() const {
        return _params.mergeType == MergeType::unsortedMerge;
    }

    bool needsSortedMerge() const {
        return _params.mergeType == MergeType::sortedMerge;
    }

    MergeType mergeType() const {
        return _params.mergeType;
    }

    bool getInRouter() const {
        return _params.inRouter;
    }

    void setInRouter(bool inRouter) {
        _params.inRouter = inRouter;
    }

    bool getFromRouter() const {
        return _params.fromRouter;
    }

    void setFromRouter(bool fromRouter) {
        _params.fromRouter = fromRouter;
    }

    bool getHasWhereClause() const {
        return _params.hasWhereClause;
    }

    void setHasWhereClause(bool hasWhereClause) {
        _params.hasWhereClause = hasWhereClause;
    }

    void setBypassDocumentValidation(bool bypassDocumentValidation) {
        _params.bypassDocumentValidation = bypassDocumentValidation;
    }

    bool getBypassDocumentValidation() const {
        return _params.bypassDocumentValidation;
    }

    void setIsUpsert(bool isUpsert) {
        _params.isUpsert = isUpsert;
    }

    bool getIsUpsert() const {
        return _params.isUpsert;
    }

    void setForPerShardCursor(bool forPerShardCursor) {
        _params.forPerShardCursor = forPerShardCursor;
    }

    bool getForPerShardCursor() const {
        return _params.forPerShardCursor;
    }

    std::string getTempDir() const {
        // TODO SERVER-109634: Return boost::filesystem::path directly when it is supported by
        // SortOptions.
        return _params.tmpDir.string();
    }

    void setTempDir(boost::filesystem::path tempDir) {
        _params.tmpDir = std::move(tempDir);
    }

    boost::optional<ExplainOptions::Verbosity> getExplain() const {
        return _params.explain;
    }

    void setExplain(boost::optional<ExplainOptions::Verbosity> verbosity) {
        _params.explain = std::move(verbosity);
    }

    ExpressionContextCollationMatchesDefault getCollationMatchesDefault() const {
        return _params.collationMatchesDefault;
    }

    void setCollationMatchesDefault(
        ExpressionContextCollationMatchesDefault collationMatchesDefault) {
        _params.collationMatchesDefault = collationMatchesDefault;
    }

    std::shared_ptr<MongoProcessInterface> getMongoProcessInterface() const {
        return _params.mongoProcessInterface;
    }

    void setMongoProcessInterface(std::shared_ptr<MongoProcessInterface> interface) {
        _params.mongoProcessInterface = std::move(interface);
    }

    const SerializationContext& getSerializationContext() const {
        return _params.serializationContext;
    }

    void setSerializationContext(SerializationContext serializationContext) {
        _params.serializationContext = std::move(serializationContext);
    }

    bool getMayDbProfile() const {
        return _params.mayDbProfile;
    }

    bool getAllowDiskUse() const {
        return _params.allowDiskUse;
    }

    void setAllowDiskUse(bool allowDiskUse) {
        _params.allowDiskUse = allowDiskUse;
    }

    bool getInLookup() const {
        return _params.inLookup;
    }

    void setInLookup(bool inLookup) {
        _params.inLookup = inLookup;
    }

    bool getInUnionWith() const {
        return _params.inUnionWith;
    }

    void setInUnionWith(bool inUnionWith) {
        _params.inUnionWith = inUnionWith;
    }

    bool getIsParsingViewDefinition() const {
        return _params.isParsingViewDefinition;
    }

    void setIsParsingViewDefinition(bool isParsingViewDefinition) {
        _params.isParsingViewDefinition = isParsingViewDefinition;
    }

    bool getIsParsingPipelineUpdate() const {
        return _params.isParsingPipelineUpdate;
    }

    void setIsParsingPipelineUpdate(bool isParsingPipelineUpdate) {
        _params.isParsingPipelineUpdate = isParsingPipelineUpdate;
    }

    bool getIsParsingCollectionValidator() const {
        return _params.isParsingCollectionValidator;
    }

    void setIsParsingCollectionValidator(bool isParsingCollectionValidator) {
        _params.isParsingCollectionValidator = isParsingCollectionValidator;
    }

    bool getExprUnstableForApiV1() const {
        return _params.exprUnstableForApiV1;
    }

    void setExprUnstableForApiV1(bool exprUnstableForApiV1) {
        _params.exprUnstableForApiV1 = exprUnstableForApiV1;
    }

    bool getExprDeprecatedForApiV1() const {
        return _params.exprDeprecatedForApiV1;
    }

    void setExprDeprecatedForApiV1(bool exprDeprecatedForApiV1) {
        _params.exprDeprecatedForApiV1 = exprDeprecatedForApiV1;
    }

    bool getEnabledCounters() const {
        return _params.enabledCounters;
    }

    boost::optional<int> getJsHeapLimitMB() const {
        return _params.jsHeapLimitMB;
    }

    void setEnabledCounters(bool enabledCounters) {
        _params.enabledCounters = enabledCounters;
    }

    bool getForcePlanCache() const {
        return _params.forcePlanCache;
    }

    void setForcePlanCache(bool forcePlanCache) {
        _params.forcePlanCache = forcePlanCache;
    }

    bool getAllowGenericForeignDbLookup() const {
        return _params.allowGenericForeignDbLookup;
    }

    // Should only be used to test parsing with the flag. Otherwise, this flag should only be set
    // when creating a new ExpressionContext.
    bool setAllowGenericForeignDbLookup_forTest(bool allowGenericForeignDbLookup) {
        return _params.allowGenericForeignDbLookup = allowGenericForeignDbLookup;
    }

    const TimeZoneDatabase* getTimeZoneDatabase() const {
        return _params.timeZoneDatabase;
    }

    int getChangeStreamTokenVersion() const {
        return _params.changeStreamTokenVersion;
    }

    void setChangeStreamTokenVersion(int changeStreamTokenVersion) {
        _params.changeStreamTokenVersion = changeStreamTokenVersion;
    }

    boost::optional<DocumentSourceChangeStreamSpec> getChangeStreamSpec() const {
        return _params.changeStreamSpec;
    }

    void setChangeStreamSpec(boost::optional<DocumentSourceChangeStreamSpec> changeStreamSpec) {
        _params.changeStreamSpec = std::move(changeStreamSpec);
    }

    const BSONObj& getOriginalAggregateCommand() const {
        return _params.originalAggregateCommand;
    }

    const BSONObj& getInitialPostBatchResumeToken() const {
        return _params.initialPostBatchResumeToken;
    }

    void setInitialPostBatchResumeToken(BSONObj initialPostBatchResumeToken) {
        _params.initialPostBatchResumeToken = std::move(initialPostBatchResumeToken);
    }

    void setOriginalAggregateCommand(BSONObj originalAggregateCommand) {
        _params.originalAggregateCommand = std::move(originalAggregateCommand);
    }

    SbeCompatibility getSbeCompatibility() const {
        return _params.sbeCompatibility;
    }

    SbeCompatibility sbeCompatibilityExchange(SbeCompatibility other) {
        return std::exchange(_params.sbeCompatibility, other);
    }

    void setSbeCompatibility(SbeCompatibility sbeCompatibility) {
        _params.sbeCompatibility = sbeCompatibility;
    }

    SbeCompatibility getSbeGroupCompatibility() const {
        return _params.sbeGroupCompatibility;
    }

    void setSbeGroupCompatibility(SbeCompatibility sbeGroupCompatibility) {
        _params.sbeGroupCompatibility = sbeGroupCompatibility;
    }

    SbeCompatibility getSbeWindowCompatibility() const {
        return _params.sbeWindowCompatibility;
    }

    void setSbeWindowCompatibility(SbeCompatibility sbeWindowCompatibility) {
        _params.sbeWindowCompatibility = sbeWindowCompatibility;
    }

    SbeCompatibility getSbePipelineCompatibility() const {
        return _params.sbePipelineCompatibility;
    }

    void setSbePipelineCompatibility(SbeCompatibility sbePipelineCompatibility) {
        _params.sbePipelineCompatibility = sbePipelineCompatibility;
    }

    const ServerSideJsConfig& getServerSideJsConfig() const {
        return _params.serverSideJsConfig;
    }

    void setServerSideJsConfigFunction(bool function) {
        _params.serverSideJsConfig.function = function;
    }

    void setServerSideJsConfigAccumulator(bool accumulator) {
        _params.serverSideJsConfig.accumulator = accumulator;
    }

    void setServerSideJsConfigWhere(bool where) {
        _params.serverSideJsConfig.where = where;
    }

    long long getSubPipelineDepth() const {
        return _params.subPipelineDepth;
    }

    void setSubPipelineDepth(long long subPipelineDepth) {
        _params.subPipelineDepth = subPipelineDepth;
    }

    TailableModeEnum getTailableMode() const {
        return _params.tailableMode;
    }

    void setTailableMode(TailableModeEnum tailableMode) {
        _params.tailableMode = tailableMode;
    }

    const boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>>& getView() const {
        return _params.view;
    }

    bool isFeatureFlagMongotIndexedViewsEnabled() const {
        return _featureFlagMongotIndexedViews.get(
            VersionContext::getDecoration(getOperationContext()));
    }

    void setView(boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>> view) {
        _params.view = std::move(view);
    }

    // Returns true if we've received a TemporarilyUnavailableException.
    bool getTemporarilyUnavailableException() const {
        return _gotTemporarilyUnavailableException;
    }

    // Sets or clears the flag indicating whether we've received a TemporarilyUnavailableException.
    void setTemporarilyUnavailableException(bool v) {
        _gotTemporarilyUnavailableException = v;
    }

    // Sets a flag which tells DocumentSource parsers whether the pipeline contains an exchange
    // stage.
    void setIncompatibleWithMemoryTracking(bool v) {
        _isIncompatibleWithMemoryTracking = v;
    }
    bool isIncompatibleWithMemoryTracking() const {
        return _isIncompatibleWithMemoryTracking;
    }

    // TODO SERVER-108400: reconsider API for accessing QuerySettings instance.
    const query_settings::QuerySettings& getQuerySettings() const {
        static const auto kEmptySettings = query_settings::QuerySettings();
        return _querySettings.get_value_or(kEmptySettings);
    }

    // TODO SERVER-108400: reconsider API for accessing QuerySettings instance.
    const boost::optional<query_settings::QuerySettings>& getOptionalQuerySettings() const {
        return _querySettings;
    }

    /**
     * Attaches 'querySettings' to context if they were not previously set.
     *
     * TODO SERVER-108400: reconsider API for accessing QuerySettings instance.
     */
    void setQuerySettingsIfNotPresent(query_settings::QuerySettings querySettings) {
        if (_querySettings.has_value()) {
            return;
        }

        tassert(8827100,
                "Query knobs shouldn't be initialized before query settings are set",
                !_queryKnobConfiguration.isInitialized());

        _querySettings = std::move(querySettings);
    }

    // TODO SERVER-108400: reconsider API for accessing QuerySettings instance.
    void setQuerySettings(const boost::optional<query_settings::QuerySettings>& querySettings) {
        _querySettings = querySettings;
    }

    const QueryKnobConfiguration& getQueryKnobConfiguration() const {
        return _queryKnobConfiguration.get(getQuerySettings());
    }

    void setIgnoreCollator() {
        _collator.setIgnore();
    }

    bool getIgnoreCollator() const {
        return _collator.getIgnore();
    }

    bool isFeatureFlagShardFilteringDistinctScanEnabled() const {
        return _featureFlagShardFilteringDistinctScan.get(
            VersionContext::getDecoration(getOperationContext()));
    }

    /**
     * Returns if the query is IDHACK query.
     */
    inline bool isIdHackQuery() const {
        return _params.isIdHackQuery;
    }

    /**
     * Returns if query contains encryption information as part of the request.
     */
    inline bool isFleQuery() const {
        return _params.isFleQuery;
    }

    /**
     * Returns if query can be rejected via query settings.
     */
    inline bool canBeRejected() const {
        return _params.canBeRejected;
    }

    bool isBasicRankFusionEnabled() const {
        return _featureFlagRankFusionBasic.get(
            VersionContext::getDecoration(getOperationContext()));
    }

    bool shouldParserAllowStreams() const {
        return shouldParserIgnoreFeatureFlagCheck() || _featureFlagStreams.get(_params.vCtx);
    }

    bool isMapReduceCommand() const {
        return _params.isMapReduceCommand;
    }

    void setIsHybridSearch() {
        _params.isHybridSearch = true;
    }

    bool isHybridSearch() const {
        return _params.isHybridSearch;
    }

    // Sets or clears a flag which tells DocumentSource parsers whether any involved Collection
    // may contain extended-range dates.
    void setRequiresTimeseriesExtendedRangeSupport(bool v) {
        _params.requiresTimeseriesExtendedRangeSupport = v;
    }
    bool getRequiresTimeseriesExtendedRangeSupport() const {
        return _params.requiresTimeseriesExtendedRangeSupport;
    }

    void setWasRateLimited(bool v) {
        _params.wasRateLimited = v;
    }

    bool wasRateLimited() const {
        return _params.wasRateLimited;
    }

    bool isFeatureFlagMqlJsEngineGapEnabled() const {
        return _featureFlagMqlJsEngineGap.get(VersionContext::getDecoration(getOperationContext()));
    }

protected:
    struct ExpressionContextParams {
        OperationContext* opCtx = nullptr;
        VersionContext vCtx;
        IncrementalFeatureRolloutContext ifrContext;
        std::unique_ptr<CollatorInterface> collator = nullptr;
        // An interface for accessing information or performing operations that have different
        // implementations on mongod and mongos, or that only make sense on one of the two.
        // Additionally, putting some of this functionality behind an interface prevents aggregation
        // libraries from having large numbers of dependencies. This pointer is always non-null.
        std::shared_ptr<MongoProcessInterface> mongoProcessInterface = nullptr;
        NamespaceString ns;
        NamespaceString originalNs;
        // A map from the user namespace to the resolved namespace (underlying nss and resolved view
        // pipeline), in case any views are involved. This map is only for *secondary* namespaces.
        // See `view` below for information on the resolved *primary* namespace.
        ResolvedNamespaceMap resolvedNamespaces;
        SerializationContext serializationContext;
        // If known, the UUID of the execution namespace for this aggregation command.
        // TODO(SERVER-78226): Replace `ns` and `uuid` with a type which can express "nss and uuid".
        boost::optional<UUID> collUUID = boost::none;
        // The explain verbosity requested by the user, or boost::none if no explain was requested.
        boost::optional<ExplainOptions::Verbosity> explain = boost::none;
        boost::optional<LegacyRuntimeConstants> runtimeConstants = boost::none;
        boost::optional<BSONObj> letParameters = boost::none;

        // The *view's* namespace with the view's effective pipeline. Note that this is different
        // than ResolvedNamespace as that holds the *underlying collections's* namespace with the
        // view's effective pipeline.
        boost::optional<std::pair<NamespaceString, std::vector<BSONObj>>> view = boost::none;
        // Defaults to empty to prevent external sorting in mongos.
        boost::filesystem::path tmpDir;
        // Tracks whether the collator to use for the aggregation matches the default collation of
        // the collection or view.
        ExpressionContextCollationMatchesDefault collationMatchesDefault =
            ExpressionContextCollationMatchesDefault::kYes;
        // When set restricts the global JavaScript heap size limit for any Scope returned by
        // getJsExecWithScope(). This limit is ignored if larger than the global limit dictated by
        // the 'jsHeapLimitMB' server parameter.
        boost::optional<int> jsHeapLimitMB;
        const TimeZoneDatabase* timeZoneDatabase = nullptr;
        // The lowest SBE compatibility level of all expressions which use this expression context.
        SbeCompatibility sbeCompatibility = SbeCompatibility::noRequirements;
        // The lowest SBE compatibility level of all accumulators in the $group stage currently
        // being parsed using this expression context. This value is transient and gets
        // reset for every $group stage we parse. Each $group stage has its own per-stage flag.
        SbeCompatibility sbeGroupCompatibility = SbeCompatibility::noRequirements;
        // The lowest SBE compatibility level of all window functions in the
        // $_internalSetWindowFields stage currently being parsed using this expression context.
        // This value is transient and gets reset for every $_internalSetWindowFields stage we
        // parse. Each $_internalSetWindowFields stage has its own per-stage flag.
        SbeCompatibility sbeWindowCompatibility = SbeCompatibility::noRequirements;
        // In some situations we could lower the collection access and, maybe, a prefix of a
        // pipeline to SBE but doing so would prevent a specific optimization that exists in the
        // classic engine from being applied. Until we implement the same optimization in SBE, we
        // need to fallback to running the query in the classic engine entirely.
        SbeCompatibility sbePipelineCompatibility = SbeCompatibility::noRequirements;
        // When non-empty, contains the unmodified user provided aggregation command.
        BSONObj originalAggregateCommand;
        // For a changeStream aggregation, this is the starting postBatchResumeToken. Empty
        // otherwise.
        BSONObj initialPostBatchResumeToken;
        // If present, the spec associated with the current change stream pipeline.
        boost::optional<DocumentSourceChangeStreamSpec> changeStreamSpec;
        // The resume token version that should be generated by a change stream.
        int changeStreamTokenVersion = ResumeTokenData::kDefaultTokenVersion;
        ServerSideJsConfig serverSideJsConfig;
        // Tracks the depth of nested aggregation sub-pipelines. Used to enforce depth limits.
        long long subPipelineDepth = 0;
        TailableModeEnum tailableMode = TailableModeEnum::kNormal;
        // Indicates where there is any chance this operation will be profiled. Must be set at
        // construction.
        bool mayDbProfile = true;
        bool fromRouter = false;
        bool inRouter = false;
        // Indicates whether/what type of merge the current pipeline needs to support. For example,
        // if this value is 'unsortedMerge', then group accumulators need to output partial results,
        // so they can be combined by the merging pipeline.
        MergeType mergeType = MergeType::noMerge;
        bool forPerShardCursor = false;
        bool allowDiskUse = false;
        bool bypassDocumentValidation = false;
        bool isMapReduceCommand = false;
        bool hasWhereClause = false;
        bool isUpsert = false;
        bool blankExpressionContext = false;
        // True if this 'ExpressionContext' object is for the inner side of a $lookup or
        // $graphLookup.
        bool inLookup = false;
        // True if this 'ExpressionContext' object is for the inner side of a $unionWith.
        bool inUnionWith = false;
        // True if this ExpressionContext is used to parse a view definition pipeline.
        bool isParsingViewDefinition = false;
        // True if this ExpressionContext is being used to parse an update pipeline.
        bool isParsingPipelineUpdate = false;
        // True if this ExpressionContext is used to parse a collection validator expression.
        bool isParsingCollectionValidator = false;
        // These fields can be used in a context when API version validations were not enforced
        // during parse time (Example creating a view or validator), but needs to be enforce while
        // querying later.
        bool exprUnstableForApiV1 = false;
        bool exprDeprecatedForApiV1 = false;
        // True if the expression context is the original one for a given pipeline.
        // False if another context is created for the same pipeline. Used to disable duplicate
        // expression counting.
        bool enabledCounters = true;
        // Forces the plan cache to be used even if there's only one solution available. Queries
        // that are ineligible will still not be cached.
        bool forcePlanCache = false;

        // Indicates if query is IDHACK query.
        bool isIdHackQuery = false;

        // Indicates if query contains encryption information as part of the request.
        bool isFleQuery = false;

        // Indicates if query can be rejected via query settings.
        bool canBeRejected = true;

        // Allows the foreign collection of a lookup to be in a different database than the local
        // collection using "from: {db: ..., coll: ...}" syntax. Currently, this should only be used
        // for streams since this isn't allowed in MQL beyond some exemptions for internal
        // collection in the local database.
        bool allowGenericForeignDbLookup = false;

        // Indicates that the pipeline is a desugared representation of a user's $rankFusion or
        // $scoreFusion pipeline.
        bool isHybridSearch = false;

        bool requiresTimeseriesExtendedRangeSupport = false;

        // Indicates that the query is replanned after being rate-limited.
        bool wasRateLimited = false;
    };

    ExpressionContextParams _params;

    /**
     * Construct an expression context using ExpressionContextParams. Consider using
     * ExpressionContextBuilder instead.
     */
    ExpressionContext(ExpressionContextParams&& config);

    friend class CollatorStash;
    friend class ExpressionContextBuilder;

    /**
     * Internal helper class that keeps track of how many times we called 'checkForInterrupt()', and
     * only performs an actual check for interrupts every x calls. This is a performance
     * optimization to reduce the cost of interrupt checking.
     */
    class InterruptChecker {
        InterruptChecker(const InterruptChecker&) = delete;
        InterruptChecker& operator=(const InterruptChecker&) = delete;

    public:
        explicit InterruptChecker(ExpressionContext* expCtx) : _expressionContext(expCtx) {}

        MONGO_COMPILER_ALWAYS_INLINE void checkForInterrupt() {
            if (--_tick == 0) {
                // Perform the actual, slow check.
                checkForInterruptSlow();
            }
        }

    private:
        // Performs the heavy work of checking whether an interrupt has occurred. For performance
        // reasons, this should only be called every now and then.
        MONGO_COMPILER_NOINLINE void checkForInterruptSlow() {
            _tick = kInterruptCheckPeriod;

            invariant(_expressionContext->getOperationContext());
            _expressionContext->getOperationContext()->checkForInterrupt();
        }

        static constexpr int kInterruptCheckPeriod = 128;

        ExpressionContext* _expressionContext;
        int _tick = kInterruptCheckPeriod;
    };


    // Class responsible for tracking the collator used for comparisons. Specifically, this
    // collator enforces the following contract:
    // - When used in a replica set or a standalone, '_collator' will have the correct collation.
    // - When routing to a tracked collection, '_collator' will have the correct collation according
    // to the ChunkManager and can use it.
    // - When routing to an untracked collection, '_collator' will be set incorrectly as there is
    // no way to know the collation of an untracked collection on the router. However,
    // 'ignore' will be set to 'true', which indicates that we will not attach '_collator' when
    // routing commands to the target collection.
    // TODO SERVER-81991: Delete this class once we branch for 8.0.
    class RoutingCollator {
    public:
        RoutingCollator(std::shared_ptr<CollatorInterface> collator) : _ptr(std::move(collator)) {}
        void setIgnore() {
            _ignore = true;
        }

        bool getIgnore() const {
            return _ignore;
        }

        void setCollator(std::shared_ptr<CollatorInterface> collator) {
            _ptr = std::move(collator);
            // If we are manually setting the collator, we shouldn't ignore it.
            _ignore = false;
        }
        std::shared_ptr<CollatorInterface> getCollatorShared() const {
            if (_ignore) {
                return nullptr;
            }
            return _ptr;
        }

        CollatorInterface* getCollator() const {
            if (_ignore) {
                return nullptr;
            }
            return _ptr.get();
        }

    private:
        std::shared_ptr<CollatorInterface> _ptr = nullptr;
        bool _ignore = false;
    } _collator;

    // Used for all comparisons of Document/Value during execution of the aggregation operation.
    // Must not be changed after parsing a Pipeline with this ExpressionContext.
    DocumentComparator _documentComparator;
    ValueComparator _valueComparator;

    // A map from namespace to the resolved namespace, in case any views are involved.
    ResolvedNamespaceMap _resolvedNamespaces;

private:
    std::unique_ptr<ExpressionCounters> _expressionCounters;
    bool _gotTemporarilyUnavailableException = false;

    bool _isCappedDelete = false;

    bool _isIncompatibleWithMemoryTracking = false;

    InterruptChecker _interruptChecker;

    // We use this set to indicate whether or not a system variable was referenced in the query that
    // is being executed (if the variable was referenced, it is an element of this set).
    stdx::unordered_set<Variables::Id> _systemVarsReferencedInQuery;

    boost::optional<query_settings::QuerySettings> _querySettings = boost::none;

    DeferredFn<QueryKnobConfiguration, const query_settings::QuerySettings&>
        _queryKnobConfiguration{[](const auto& querySettings) {
            return QueryKnobConfiguration(querySettings);
        }};

    Deferred<bool (*)(const VersionContext&)> _featureFlagShardFilteringDistinctScan{
        [](const VersionContext& vCtx) {
            return feature_flags::gFeatureFlagShardFilteringDistinctScan
                .isEnabledUseLastLTSFCVWhenUninitialized(
                    vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        }};

    Deferred<bool (*)(const VersionContext&)> _featureFlagRankFusionBasic{
        [](const VersionContext& vCtx) {
            return feature_flags::gFeatureFlagRankFusionBasic
                .isEnabledUseLastLTSFCVWhenUninitialized(
                    vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        }};

    Deferred<bool (*)(const VersionContext&)> _featureFlagMongotIndexedViews{
        [](const VersionContext& vCtx) {
            return feature_flags::gFeatureFlagMongotIndexedViews
                .isEnabledUseLatestFCVWhenUninitialized(
                    vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        }};

    Deferred<bool (*)(const VersionContext&)> _featureFlagMqlJsEngineGap{
        [](const VersionContext& vCtx) {
            return feature_flags::gFeatureFlagMqlJsEngineGap.isEnabledUseLatestFCVWhenUninitialized(
                vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
        }};

    // Initialized in constructor to avoid including server_feature_flags_gen.h
    // in this header file.
    Deferred<bool (*)(const VersionContext&)> _featureFlagStreams;
};

}  // namespace mongo
