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

#include <absl/container/node_hash_set.h>
#include <boost/intrusive_ptr.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/db/query/tailable_mode_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/scripting/engine.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

namespace mongo {

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

class ExpressionContext : public RefCountable {
public:
    struct ResolvedNamespace {
        ResolvedNamespace() = default;
        ResolvedNamespace(NamespaceString ns,
                          std::vector<BSONObj> pipeline,
                          boost::optional<UUID> uuid = boost::none);

        NamespaceString ns;
        std::vector<BSONObj> pipeline;
        boost::optional<UUID> uuid = boost::none;
    };

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

    /**
     * Constructs an ExpressionContext to be used for find command parsing and evaluation.
     */
    ExpressionContext(OperationContext* opCtx,
                      const FindCommandRequest& findCmd,
                      std::unique_ptr<CollatorInterface> collator,
                      bool mayDbProfile,
                      boost::optional<ExplainOptions::Verbosity> verbosity = boost::none,
                      bool allowDiskUseByDefault = false);

    ExpressionContext(OperationContext* opCtx,
                      const DistinctCommandRequest& distinctCmd,
                      const NamespaceString& nss,
                      std::unique_ptr<CollatorInterface> collator,
                      bool mayDbProfile,
                      boost::optional<ExplainOptions::Verbosity> verbosity);
    /**
     * Constructs an ExpressionContext to be used for Pipeline parsing and evaluation.
     * 'resolvedNamespaces' maps collection names (not full namespaces) to ResolvedNamespaces.
     */
    ExpressionContext(OperationContext* opCtx,
                      const AggregateCommandRequest& request,
                      std::unique_ptr<CollatorInterface> collator,
                      std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
                      StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces,
                      boost::optional<UUID> collUUID,
                      bool mayDbProfile = true,
                      bool allowDiskUseByDefault = false);

    /**
     * Constructs an ExpressionContext to be used for Pipeline parsing and evaluation. This version
     * requires finer-grained parameters but does not require an AggregateCommandRequest.
     * 'resolvedNamespaces' maps collection names (not full namespaces) to ResolvedNamespaces.
     */
    ExpressionContext(OperationContext* opCtx,
                      const boost::optional<ExplainOptions::Verbosity>& explain,
                      bool fromMongos,
                      bool needsMerge,
                      bool allowDiskUse,
                      bool bypassDocumentValidation,
                      bool isMapReduceCommand,
                      const NamespaceString& ns,
                      const boost::optional<LegacyRuntimeConstants>& runtimeConstants,
                      std::unique_ptr<CollatorInterface> collator,
                      const std::shared_ptr<MongoProcessInterface>& mongoProcessInterface,
                      StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces,
                      boost::optional<UUID> collUUID,
                      const boost::optional<BSONObj>& letParameters = boost::none,
                      bool mayDbProfile = true,
                      const SerializationContext& serializationCtx = SerializationContext());

    /**
     * Constructs an ExpressionContext suitable for use outside of the aggregation system, including
     * for MatchExpression parsing and executing pipeline-style operations in the Update system.
     *
     * If 'collator' is null, the simple collator will be used.
     */
    ExpressionContext(OperationContext* opCtx,
                      std::unique_ptr<CollatorInterface> collator,
                      const NamespaceString& ns,
                      const boost::optional<LegacyRuntimeConstants>& runtimeConstants = boost::none,
                      const boost::optional<BSONObj>& letParameters = boost::none,
                      bool allowDiskUse = false,
                      bool mayDbProfile = true,
                      boost::optional<ExplainOptions::Verbosity> explain = boost::none);

    /**
     * Constructs a blank ExpressionContext suitable for creating Query Shapes, but it could be
     * applied to other use cases as well.
     *
     * The process for creating a Query Shape sometimes requires re-parsing the BSON into a proper
     * AST, and for that you need an ExpressionContext.
     *
     * Note: this is meant for introspection and is not suitable for using to execute queries -
     * since it does not contain for example a collation argument or a real MongoProcessInterface
     * for execution.
     */
    static boost::intrusive_ptr<ExpressionContext> makeBlankExpressionContext(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        boost::optional<BSONObj> shapifiedLet = boost::none);

    /**
     * Used by a pipeline to check for interrupts so that killOp() works. Throws a UserAssertion if
     * this aggregation pipeline has been interrupted.
     */
    void checkForInterrupt() {
        if (--_interruptCounter == 0) {
            checkForInterruptSlow();
        }
    }

    /**
     * Returns true if this is a collectionless aggregation on the specified database.
     */
    bool isDBAggregation(const NamespaceString& other) const {
        return ns.isEqualDb(other) && ns.isCollectionlessAggregateNS();
    }

    /**
     * Returns true if this is a collectionless aggregation on the 'admin' database.
     */
    bool isClusterAggregation() const {
        return ns.isAdminDB() && ns.isCollectionlessAggregateNS();
    }

    /**
     * Returns true if this aggregation is running on a single, specific namespace.
     */
    bool isSingleNamespaceAggregation() const {
        return !ns.isCollectionlessAggregateNS();
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
        return static_cast<bool>(explain);
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
     * Returns an ExpressionContext that is identical to 'this' that can be used to execute a
     * separate aggregation pipeline on 'ns' with the optional 'uuid' and an updated collator.
     */
    boost::intrusive_ptr<ExpressionContext> copyWith(
        NamespaceString ns,
        boost::optional<UUID> uuid = boost::none,
        boost::optional<std::unique_ptr<CollatorInterface>> updatedCollator = boost::none) const;

    /**
     * Returns an ExpressionContext that is identical to 'this' except for the 'subPipelineDepth'
     * and 'needsMerge' fields.
     */
    boost::intrusive_ptr<ExpressionContext> copyForSubPipeline(
        NamespaceString nss, boost::optional<UUID> uuid = boost::none) const {
        uassert(ErrorCodes::MaxSubPipelineDepthExceeded,
                str::stream() << "Maximum number of nested sub-pipelines exceeded. Limit is "
                              << internalMaxSubPipelineViewDepth.load(),
                subPipelineDepth < internalMaxSubPipelineViewDepth.load());
        auto newCopy = copyWith(std::move(nss), uuid);
        newCopy->subPipelineDepth += 1;
        // The original expCtx might have been attached to an aggregation pipeline running on the
        // shards. We must reset 'needsMerge' in order to get fully merged results for the
        // subpipeline.
        newCopy->needsMerge = false;
        return newCopy;
    }

    /**
     * Returns the ResolvedNamespace corresponding to 'nss'. It is an error to call this method on a
     * namespace not involved in the pipeline.
     */
    const ResolvedNamespace& getResolvedNamespace(const NamespaceString& nss) const {
        auto it = _resolvedNamespaces.find(nss.coll());
        invariant(it != _resolvedNamespaces.end(),
                  str::stream() << "No resolved namespace provided for "
                                << nss.toStringForErrorMsg());
        return it->second;
    };

    /**
     * Returns true if there are no namespaces in the query other than the namespace the query was
     * issued against. eg if there is no $out, $lookup ect. If namespaces have not yet been resolved
     * then it will also return false.
     */
    bool noForeignNamespaces() const {
        return _resolvedNamespaces.empty();
    }

    /**
     * Returns true if the tailableMode indicates a tailable
     * query.
     */
    bool isTailable() const {
        return tailableMode == TailableModeEnum::kTailableAndAwaitData ||
            tailableMode == TailableModeEnum::kTailable;
    }

    /**
     * Convenience call that returns true if the tailableMode indicates a tailable and awaitData
     * query.
     */
    bool isTailableAwaitData() const {
        return tailableMode == TailableModeEnum::kTailableAndAwaitData;
    }

    /**
     * Returns true if the pipeline is eligible for query sampling for the purpose of shard key
     * selection metrics.
     */
    bool eligibleForSampling() const {
        return !explain;
    }

    void setResolvedNamespaces(StringMap<ResolvedNamespace> resolvedNamespaces) {
        _resolvedNamespaces = std::move(resolvedNamespaces);
    }

    void addResolvedNamespaces(
        const mongo::stdx::unordered_set<mongo::NamespaceString>& resolvedNamespaces) {
        for (const auto& nss : resolvedNamespaces) {
            _resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
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
     *
     * Returns a JsExec and a boolean indicating whether the Scope was created as part of this call.
     */
    auto getJsExecWithScope(bool forceLoadOfStoredProcedures = false) const {
        uassert(31264,
                "Cannot run server-side javascript without the javascript engine enabled",
                getGlobalScriptEngine());
        const auto isMapReduce =
            (variables.hasValue(Variables::kIsMapReduceId) &&
             variables.getValue(Variables::kIsMapReduceId).getType() == BSONType::Bool &&
             variables.getValue(Variables::kIsMapReduceId).coerceToBool());
        if (inMongos) {
            invariant(!forceLoadOfStoredProcedures);
            invariant(!isMapReduce);
        }

        // Stored procedures are only loaded for the $where expression and MapReduce command.
        const bool loadStoredProcedures = forceLoadOfStoredProcedures || isMapReduce;

        if (hasWhereClause && !loadStoredProcedures) {
            uasserted(4649200,
                      "A single operation cannot use both JavaScript aggregation expressions and "
                      "$where.");
        }

        auto scopeObj = BSONObj();
        if (variables.hasValue(Variables::kJsScopeId)) {
            auto scopeVar = variables.getValue(Variables::kJsScopeId);
            invariant(scopeVar.isObject());
            scopeObj = scopeVar.getDocument().toBson();
        }
        return JsExecution::get(opCtx, scopeObj, ns.dbName(), loadStoredProcedures, jsHeapLimitMB);
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

    bool expressionCountersAreActive() {
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
     * Throws if the provided feature flag is not enabled in the current FCV or
     * 'maxFeatureCompatibilityVersion' if set. Will do nothing if the feature flag is enabled
     * or boost::none.
     */
    void throwIfFeatureFlagIsNotEnabledOnFCV(StringData name,
                                             const boost::optional<FeatureFlag>& flag);

    // The explain verbosity requested by the user, or boost::none if no explain was requested.
    boost::optional<ExplainOptions::Verbosity> explain;

    bool fromMongos = false;
    bool needsMerge = false;
    bool inMongos = false;
    bool forPerShardCursor = false;
    bool allowDiskUse = false;
    bool bypassDocumentValidation = false;
    bool hasWhereClause = false;

    NamespaceString ns;

    SerializationContext serializationCtxt;

    // If known, the UUID of the execution namespace for this aggregation command.
    // TODO(SERVER-78226): Replace `ns` and `uuid` with a type which can express "nss and uuid".
    boost::optional<UUID> uuid;

    std::string tempDir;  // Defaults to empty to prevent external sorting in mongos.

    OperationContext* opCtx;

    // When set restricts the global JavaScript heap size limit for any Scope returned by
    // getJsExecWithScope(). This limit is ignored if larger than the global limit dictated by the
    // 'jsHeapLimitMB' server parameter.
    boost::optional<int> jsHeapLimitMB;

    // An interface for accessing information or performing operations that have different
    // implementations on mongod and mongos, or that only make sense on one of the two.
    // Additionally, putting some of this functionality behind an interface prevents aggregation
    // libraries from having large numbers of dependencies. This pointer is always non-null.
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface;

    const TimeZoneDatabase* timeZoneDatabase;

    Variables variables;
    VariablesParseState variablesParseState;

    TailableModeEnum tailableMode = TailableModeEnum::kNormal;

    // For a changeStream aggregation, this is the starting postBatchResumeToken. Empty otherwise.
    BSONObj initialPostBatchResumeToken;

    // Tracks the depth of nested aggregation sub-pipelines. Used to enforce depth limits.
    long long subPipelineDepth = 0;

    // True if this 'ExpressionContext' object is for the inner side of a $lookup or $graphLookup.
    bool inLookup = false;

    // True if this 'ExpressionContext' object is for the inner side of a $unionWith.
    bool inUnionWith = false;

    // If set, this will disallow use of features introduced in versions above the provided version.
    boost::optional<multiversion::FeatureCompatibilityVersion> maxFeatureCompatibilityVersion;

    // True if this ExpressionContext is used to parse a view definition pipeline.
    bool isParsingViewDefinition = false;

    // True if this ExpressionContext is being used to parse an update pipeline.
    bool isParsingPipelineUpdate = false;

    // True if this ExpressionContext is used to parse a collection validator expression.
    bool isParsingCollectionValidator = false;

    // Indicates where there is any chance this operation will be profiled. Must be set at
    // construction.
    const bool mayDbProfile = true;

    // The lowest SBE compatibility level of all expressions which use this expression context.
    SbeCompatibility sbeCompatibility = SbeCompatibility::noRequirements;

    // The lowest SBE compatibility level of all accumulators in the $group stage currently
    // being parsed using this expression context. This value is transient and gets
    // reset for every $group stage we parse. Each $group stage has its own per-stage flag.
    SbeCompatibility sbeGroupCompatibility = SbeCompatibility::noRequirements;

    // The lowest SBE compatibility level of all window functions in the $_internalSetWindowFields
    // stage currently being parsed using this expression context. This value is transient and gets
    // reset for every $_internalSetWindowFields stage we parse. Each $_internalSetWindowFields
    // stage has its own per-stage flag.
    SbeCompatibility sbeWindowCompatibility = SbeCompatibility::noRequirements;

    // In some situations we could lower the collection access and, maybe, a prefix of a pipeline to
    // SBE but doing so would prevent a specific optimization that exists in the classic engine from
    // being applied. Until we implement the same optimization in SBE, we need to fallback to
    // running the query in the classic engine entirely.
    SbeCompatibility sbePipelineCompatibility = SbeCompatibility::noRequirements;

    // These fields can be used in a context when API version validations were not enforced during
    // parse time (Example creating a view or validator), but needs to be enforce while querying
    // later.
    bool exprUnstableForApiV1 = false;
    bool exprDeprectedForApiV1 = false;

    // Tracks whether the collator to use for the aggregation matches the default collation of the
    // collection or view.
    enum class CollationMatchesDefault { kYes, kNo };
    CollationMatchesDefault collationMatchesDefault = CollationMatchesDefault::kYes;

    // When non-empty, contains the unmodified user provided aggregation command.
    BSONObj originalAggregateCommand;

    // If present, the spec associated with the current change stream pipeline.
    boost::optional<DocumentSourceChangeStreamSpec> changeStreamSpec;

    // The resume token version that should be generated by a change stream.
    int changeStreamTokenVersion = ResumeTokenData::kDefaultTokenVersion;

    // True if the expression context is the original one for a given pipeline.
    // False if another context is created for the same pipeline. Used to disable duplicate
    // expression counting.
    bool enabledCounters = true;

    // Returns true if we've received a TemporarilyUnavailableException.
    bool getTemporarilyUnavailableException() {
        return _gotTemporarilyUnavailableException;
    }

    // Sets or clears the flag indicating whether we've received a TemporarilyUnavailableException.
    void setTemporarilyUnavailableException(bool v) {
        _gotTemporarilyUnavailableException = v;
    }

    // Sets or clears a flag which tells DocumentSource parsers whether any involved Collection
    // may contain extended-range dates.
    void setRequiresTimeseriesExtendedRangeSupport(bool v) {
        _requiresTimeseriesExtendedRangeSupport = v;
    }
    bool getRequiresTimeseriesExtendedRangeSupport() const {
        return _requiresTimeseriesExtendedRangeSupport;
    }

    const query_settings::QuerySettings& getQuerySettings() const {
        return _querySettings;
    }

    void setQuerySettings(query_settings::QuerySettings querySettings) {
        _querySettings = querySettings;
    }

    // Forces the plan cache to be used even if there's only one solution available. Queries that
    // are ineligible will still not be cached.
    bool forcePlanCache = false;

    // This is state that is to be shared between the DocumentInternalSearchMongotRemote and
    // DocumentInternalSearchIdLookup stages (these stages are the result of desugaring $search)
    // during runtime.
    class SharedSearchState {
    public:
        SharedSearchState() {}

        long long getDocsReturnedByIdLookup() const {
            return _docsReturnedByIdLookup;
        }

        /**
         * Sets the value of _docsReturnedByIdLookup to 0.
         */
        void resetDocsReturnedByIdLookup() {
            _docsReturnedByIdLookup = 0;
        }

        /**
         * Increments the value of _docsReturnedByIdLookup by 1.
         */
        void incrementDocsReturnedByIdLookup() {
            _docsReturnedByIdLookup++;
        }

    private:
        // When there is an extractable limit in the query, DocumentInternalSearchMongotRemote sends
        // a getMore to mongot that specifies how many more documents it needs to fulfill that
        // limit, and it incorporates the amount of documents returned by the
        // DocumentInternalSearchIdLookup stage into that value.
        long long _docsReturnedByIdLookup = 0;
    } sharedSearchState;

    void setIgnoreCollator() {
        _collator.setIgnore();
    }

    bool getIgnoreCollator() {
        return _collator.getIgnore();
    }

protected:
    static const int kInterruptCheckPeriod = 128;

    friend class CollatorStash;

    // Performs the heavy work of checking whether an interrupt has occurred. Should only be called
    // when _interruptCounter has been decremented to zero.
    void checkForInterruptSlow();

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
            _ptr = collator;
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
    StringMap<ResolvedNamespace> _resolvedNamespaces;

    int _interruptCounter = kInterruptCheckPeriod;

    bool _isCappedDelete = false;

    bool _requiresTimeseriesExtendedRangeSupport = false;

private:
    // Instantiates an ExpressionContext which does not increment expression counters and does not
    // enforce FCV restrictions. It is used for implementing the `makeBlankExpressionContext()`
    // factory method. Please also note that the runtime constants are not given real/accurate
    // values of '$$NOW' and '$$CLUSTER_TIME', in the name of efficiency.
    ExpressionContext(OperationContext* opCtx,
                      const NamespaceString& ns,
                      const boost::optional<BSONObj>& letParameters = boost::none);

    std::unique_ptr<ExpressionCounters> _expressionCounters;
    bool _gotTemporarilyUnavailableException = false;

    // We use this set to indicate whether or not a system variable was referenced in the query that
    // is being executed (if the variable was referenced, it is an element of this set).
    stdx::unordered_set<Variables::Id> _systemVarsReferencedInQuery;

    query_settings::QuerySettings _querySettings = query_settings::QuerySettings();
};

}  // namespace mongo
