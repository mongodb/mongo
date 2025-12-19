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
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace MONGO_MOD_UNFORTUNATELY_OPEN mongo {

class LiteParsedPipeline;

struct LiteParserOptions {
    // Allows the foreign collection of a lookup to be in a different database than the local
    // collection using "from: {db: ..., coll: ...}" syntax. Currently, this should only be used
    // for streams since this isn't allowed in MQL beyond some exemptions for internal
    // collection in the local database. While this flag also exists on expressionContext, we also
    // need this in the LiteParseContext to correctly throw errors when a non-streams pipeline tries
    // to use foreign db syntax for $lookup beyond the exempted internal collections during lite
    // parsing since lite parsing doesn't have an expressionContext.
    bool allowGenericForeignDbLookup = false;
};

namespace exec::agg {
class ListMqlEntitiesStage;
}

namespace extension::host {
class LoadExtensionsTest;
class LoadNativeVectorSearchTest;
}  // namespace extension::host

// Forward declare LiteParsedDocumentSource.
class LiteParsedDocumentSource;

/**
 * A ViewInfo struct stores the view namespace, resolved namespace (underlying collection), and the
 * desugared view pipeline from ResolvedView.
 */
struct MONGO_MOD_PUBLIC ViewInfo {
    using LiteParsedVec = std::vector<std::unique_ptr<LiteParsedDocumentSource>>;

    ViewInfo() = default;

    // Move-only semantics (viewPipeline contains unique_ptrs which are non-copyable).
    ViewInfo(ViewInfo&&) noexcept = default;
    ViewInfo& operator=(ViewInfo&&) noexcept = default;
    ViewInfo(const ViewInfo&) = delete;
    ViewInfo& operator=(const ViewInfo&) = delete;

    /**
     * Constructs a ViewInfo object from the view namespace, underlying collection's namespace, and
     * parses the bson stages in the view pipeline into LiteParsedDocumentSources.
     *
     * Note that the ViewInfo owns the backing BSONObj for `viewPipeline`.
     */
    ViewInfo(NamespaceString viewName,
             NamespaceString resolvedNss,
             std::vector<BSONObj> viewPipeBson,
             const LiteParserOptions& options = LiteParserOptions{});

    /**
     * Returns the original BSON view pipeline. Note that this is the pre-desugared version of the
     * pipeline.
     */
    std::vector<BSONObj> getOriginalBson() const;

    ViewInfo clone() const;

    NamespaceString viewName;     // Unresolved view namespace.
    NamespaceString resolvedNss;  // Underlying collection that the view runs.

private:
    // Owns the BSON data that viewPipeline's LiteParsedDocumentSource objects reference.
    // Must be declared before viewPipeline so it is destroyed after viewPipeline (C++ destroys
    // members in reverse declaration order, and LiteParsedDocumentSource holds BSONElement
    // references into this data).
    std::vector<BSONObj> _ownedOriginalBsonPipeline;

public:
    // The desugared view pipeline as a vector of LiteParsedDocumentSources. This vector can be
    // added to existing pipelines to apply a view to a pipeline.
    LiteParsedVec viewPipeline;
};

using ViewPolicyCallbackFn = std::function<void(const ViewInfo&, StringData)>;

/**
 * Indicates how this stage will interact with a view. LiteParsedDocumentSources that
 * wish to perform custom behavior for views should override the callback function.
 */
struct ViewPolicy {
    // Describes what the pipeline as a whole should do with a view on the main aggregate
    // collection, if this stage is at the front of the pipeline.
    enum class kFirstStageApplicationPolicy {
        // If this stage is at the front of the pipeline, the pipeline should
        // prepend the view.
        kDefaultPrepend,
        // If this stage is at the front of the pipeline, the pipeline should not
        // prepend the view. The stage will apply the view pipeline itself internally.
        kDoNothing,
    } policy = kFirstStageApplicationPolicy::kDefaultPrepend;

    // Offers a stage the chance to receive/bind to a view definition on the command's resolved view
    // definitions. Receives resolved view information and the stage name.
    ViewPolicyCallbackFn callback = [](const ViewInfo&, StringData) {
        // Default callback is a no-op.
    };
};

/**
 * Default view policy for aggregation stages. This policy allows views to be used with the stage
 * by prepending the view pipeline when the stage is at the front of the pipeline.
 *
 * When a stage uses DefaultViewPolicy:
 * - If the stage is at the front of the pipeline and the main collection is a view, the view
 *   pipeline will be prepended to the aggregation pipeline.
 * - The callback is a no-op, meaning the stage does not need to perform any special handling
 *   when a view is encountered.
 *
 * This is the default behavior for most aggregation stages that support views.
 */
struct DefaultViewPolicy : ViewPolicy {};

/**
 * View policy that disallows views for aggregation stages. This policy prevents views from being
 * used with the stage by throwing an error when a view is encountered.
 *
 * When a stage uses DisallowViewsPolicy:
 * - The policy is set to kDoNothing, meaning the view pipeline will not be automatically prepended.
 * - The callback throws a CommandNotSupportedOnView error (or a custom error if a custom callback
 *   is provided) when a view is detected.
 *
 * Use this policy for stages that cannot operate on views.
 */
struct DisallowViewsPolicy : public ViewPolicy {
    DisallowViewsPolicy();
    DisallowViewsPolicy(ViewPolicyCallbackFn&&);
};

/**
 * A lightly parsed version of a DocumentSource. It is not executable and not guaranteed to return a
 * parse error when encountering an invalid specification. Instead, the purpose of this class is to
 * make certain DocumentSource properties available before full parsing (e.g., getting the involved
 * foreign collections).
 */
class MONGO_MOD_UNFORTUNATELY_OPEN LiteParsedDocumentSource {
public:
    /*
     * This is the type of parser you should register using REGISTER_DOCUMENT_SOURCE. It need not
     * do any validation of options, only enough parsing to be able to implement the interface.
     *
     * The NamespaceString can be used to determine the namespace on which this aggregation is being
     * performed, and the BSONElement will be the element whose field name is the name of this stage
     * (e.g. the first and only element in {$limit: 1}).
     */
    using Parser = std::function<std::unique_ptr<LiteParsedDocumentSource>(
        const NamespaceString&, const BSONElement&, const LiteParserOptions&)>;

    struct LiteParserInfo {
        Parser parser;
        AllowedWithApiStrict allowedWithApiStrict;
        AllowedWithClientType allowedWithClientType;
    };

    /*
     * A LiteParserRegistration encapsulates the set of all parsers that can be used to parse a
     * stage into a LiteParsedDocumentSource, controlled by the value of a feature flag.
     */
    class LiteParserRegistration {
    public:
        const LiteParserInfo& getParserInfo() const;

        void setPrimaryParser(LiteParserInfo&& lpi);

        // TODO SERVER-114028 Update when fallback parsing supports all feature flags.
        void setFallbackParser(LiteParserInfo&& lpi,
                               IncrementalRolloutFeatureFlag* ff,
                               bool isStub = false);

        bool isPrimarySet() const;

        bool isFallbackSet() const;

        // Returns true if the parser is executable, meaning it has either a primary or a non-stub
        // fallback.
        bool isExecutable() const {
            return _primaryIsSet || !_isStub;
        }

    private:
        // The preferred method of parsing this LiteParsedDocumentSource. If the feature flag is
        // enabled, the primary parser will be used to parse the stage.
        LiteParserInfo _primaryParser;

        // The fallback method of parsing this LiteParsedDocumentSource. If the feature flag is
        // disabled, the fallback parser will be used to parse the stage.
        LiteParserInfo _fallbackParser;

        // When enabled, signals to use the primary parser; when disabled, signals to use the
        // fallback parser.
        // TODO SERVER-114028 Generalize this to be FeatureFlag*.
        IncrementalRolloutFeatureFlag* _primaryParserFeatureFlag = nullptr;

        // Whether or not the primary parser has been registered or not.
        bool _primaryIsSet = false;

        // Whether or not the fallback parser has been registered or not.
        bool _fallbackIsSet = false;

        // Whether the fallback parser is a stub parser that just throws an error.
        bool _isStub = false;
    };

    using ParserMap = StringMap<LiteParsedDocumentSource::LiteParserRegistration>;

    /**
     * Constructs a LiteParsedDocumentSource from the user-supplied BSON.
     *
     * IMPORTANT: We store the BSONElement view into the original BSONObj stage spec, so the caller
     * is responsible for ensuring the lifetime of the original BSONObj exceeds that of this class.
     */
    LiteParsedDocumentSource(const BSONElement& originalBson)
        : _originalBson(originalBson), _parseTimeName(originalBson.fieldNameStringData()) {}

    virtual ~LiteParsedDocumentSource() = default;

    /**
     * Registers a DocumentSource with a spec parsing function, so that when a stage with the given
     * name is encountered, it will call 'parser' to construct that stage's specification object.
     * The flag 'allowedWithApiStrict' is used to control the allowance of the stage when
     * 'apiStrict' is set to true.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_DOCUMENT_SOURCE macro defined in
     * document_source.h.
     */
    static void registerParser(const std::string& name,
                               Parser parser,
                               AllowedWithApiStrict allowedWithApiStrict,
                               AllowedWithClientType allowedWithClientType);

    static void registerFallbackParser(const std::string& name,
                                       Parser parser,
                                       FeatureFlag* parserFeatureFlag,
                                       AllowedWithApiStrict allowedWithApiStrict,
                                       AllowedWithClientType allowedWithClientType,
                                       bool isStub = false);

    /**
     * Function that will be used as an alternate parser for a document source that has been
     * disabled.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parseDisabled(
        NamespaceString nss,
        const BSONElement& spec,
        const LiteParserOptions& options = LiteParserOptions{}) {
        uasserted(
            ErrorCodes::QueryFeatureNotAllowed,
            str::stream() << spec.fieldName()
                          << " is not allowed with the current configuration. You may need to "
                             "enable the corresponding feature flag");
    }

    void setApiStrict(AllowedWithApiStrict& apiStrict) {
        _apiStrict = apiStrict;
    }

    void setClientType(AllowedWithClientType& clientType) {
        _clientType = clientType;
    }

    const AllowedWithApiStrict& getApiStrict() {
        return _apiStrict;
    };
    const AllowedWithClientType& getClientType() {
        return _clientType;
    };

    /**
     * Constructs a LiteParsedDocumentSource from the user-supplied BSON, or throws a
     * AssertionException.
     *
     * Extracts the first field name from 'spec', and delegates to the parser that was registered
     * with that field name using registerParser() above.
     */
    static std::unique_ptr<LiteParsedDocumentSource> parse(
        const NamespaceString& nss,
        const BSONObj& spec,
        const LiteParserOptions& options = LiteParserOptions{});

    /**
     * Returns the foreign collection(s) referenced by this stage (that is, any collection that
     * the pipeline references, but doesn't get locked), if any.
     */
    virtual stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const = 0;

    /**
     * Returns the foreign collections(s) referenced by this stage that potentially will be
     * involved in query execution (that is, a collection that the pipeline references, and gets
     * locked for the purposes of query execution), if any.
     */
    virtual void getForeignExecutionNamespaces(stdx::unordered_set<NamespaceString>& nssSet) const {
    }

    /**
     * Returns a list of the privileges required for this stage.
     */
    virtual PrivilegeVector requiredPrivileges(bool isMongos,
                                               bool bypassDocumentValidation) const = 0;

    /**
     * Does any custom assertions necessary to validate this stage is permitted in the given API
     * Version. For example, if certain stage parameters are permitted but others excluded, that
     * should happen here.
     */
    virtual void assertPermittedInAPIVersion(const APIParameters&) const {
        // By default there are no custom checks needed. The 'AllowedWithApiStrict' flag should take
        // care of most cases.
    }

    virtual std::unique_ptr<StageParams> getStageParams() const = 0;

    /**
     * Retrieve the ViewPolicy for this stage.
     */
    virtual ViewPolicy getViewPolicy() const {
        return DefaultViewPolicy{};
    }

    /**
     * Returns true if this is a $collStats stage.
     */
    virtual bool isCollStats() const {
        return false;
    }

    /**
     * Returns true if this is a $indexStats stage.
     */
    virtual bool isIndexStats() const {
        return false;
    }

    /**
     * Returns true if this is a $changeStream stage.
     */
    virtual bool isChangeStream() const {
        return false;
    }

    /**
     * Returns true if this is a write stage.
     */
    virtual bool isWriteStage() const {
        return false;
    }

    /**
     * Returns true if this stage is an initial source and should run just once on the entire
     * cluster.
     */
    virtual bool generatesOwnDataOnce() const {
        return false;
    }

    /**
     * Returns true if this stage does not require an input source.
     */
    virtual bool isInitialSource() const {
        return false;
    }

    /**
     * Returns true if this stage should make the aggregation command exempt from ingress admission
     * control.
     */
    virtual bool isExemptFromIngressAdmissionControl() const {
        return false;
    }

    /**
     * Returns true if this is a search stage ($search, $vectorSearch, $rankFusion, etc.)
     */
    virtual bool isSearchStage() const {
        return false;
    }

    /**
     * Returns true if this is a $rankFusion pipeline
     */
    virtual bool isHybridSearchStage() const {
        return false;
    }

    /**
     * Returns true if this stage require knowledge of the collection default collation at parse
     * time, false otherwise. This is useful to know as it could save a network request to discern
     * the collation.
     * TODO SERVER-81991: Delete this function once all unsharded collections are tracked in the
     * sharding catalog as unsplittable along with their collation.
     */
    virtual bool requiresCollationForParsingUnshardedAggregate() const {
        return false;
    }

    /**
     * Returns false if aggregation stages manually opt out of mandatory authorization checks, true
     otherwise. Will enable mandatory authorization checks by default.
     */
    virtual bool requiresAuthzChecks() const {
        return true;
    }

    /**
     * Returns Status::OK() if the involved namespace 'nss' is allowed to be sharded. The behavior
     * is to allow by default. Stages should opt-out if foreign collections are not allowed to be
     * sharded by returning a Status with a message explaining why.
     */
    virtual Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                                  bool inMultiDocumentTransaction) const {
        return Status::OK();
    }

    /**
     * Verifies that this stage is allowed to run with the specified read concern level.
     */
    virtual ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                         bool isImplicitDefault) const {
        return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
    }

    /**
     * Verifies that this stage is allowed to run in a multi-document transaction. Throws a
     * UserException if not compatible. This should only be called if the caller has determined the
     * current operation is part of a transaction.
     */
    virtual void assertSupportsMultiDocumentTransaction() const {}

    /**
     * Returns this document source's subpipelines. If none exist, a reference to an empty vector
     * is returned.
     */
    virtual const std::vector<LiteParsedPipeline>& getSubPipelines() const;

    /**
     * Returns the name of the stage that this LiteParsedDocumentSource represents.
     */
    const std::string& getParseTimeName() const {
        return _parseTimeName;
    }

    BSONElement getOriginalBson() const {
        return _originalBson;
    }

protected:
    BSONElement _originalBson;

    void transactionNotSupported(StringData stageName) const {
        uasserted(ErrorCodes::OperationNotSupportedInTransaction,
                  str::stream() << "Operation not permitted in transaction :: caused by :: "
                                << "Aggregation stage " << stageName << " cannot run within a "
                                << "multi-document transaction.");
    }

    ReadConcernSupportResult onlySingleReadConcernSupported(StringData stageName,
                                                            repl::ReadConcernLevel supportedLevel,
                                                            repl::ReadConcernLevel candidateLevel,
                                                            bool isImplicitDefault) const {
        return {{candidateLevel != supportedLevel && !isImplicitDefault,
                 {ErrorCodes::InvalidOptions,
                  str::stream() << "Aggregation stage " << stageName
                                << " cannot run with a readConcern other than '"
                                << repl::readConcernLevels::toString(supportedLevel)
                                << "'. Current readConcern: "
                                << repl::readConcernLevels::toString(candidateLevel)}},
                {{ErrorCodes::InvalidOptions,
                  str::stream() << "Aggregation stage " << stageName
                                << " does not permit default readConcern to be applied."}}};
    }

    ReadConcernSupportResult onlyReadConcernLocalSupported(StringData stageName,
                                                           repl::ReadConcernLevel level,
                                                           bool isImplicitDefault) const {
        return onlySingleReadConcernSupported(
            stageName, repl::ReadConcernLevel::kLocalReadConcern, level, isImplicitDefault);
    }

private:
    /**
     * Give access to 'parserMap' so we can remove a registered parser with
     * 'unregisterParser_forTest'.
     */
    friend class LiteParserRegistrationTest;
    friend class LiteParsedDocumentSourceParseTest;
    friend class extension::host::LoadExtensionsTest;
    friend class extension::host::LoadNativeVectorSearchTest;

    /**
     * Give access to 'getParserMap()' for the implementation of $listMqlEntities but hiding
     * it from all other stages.
     */
    friend class exec::agg::ListMqlEntitiesStage;

    /**
     * 'unregisterParser_forTest' is only meant to be used in the context of unit tests. This is
     * because the 'parserMap' is not thread safe, so modifying it at runtime is unsafe.
     */
    static void unregisterParser_forTest(const std::string& name);
    /**
     * 'getParserInfo_forTest' is only meant to be used in the context of unit tests as well,
     * since the 'FirstFallbackParserTakesPrecedence*' tests do assertions with the 'apiStrict'
     * and 'clientType' members directly from the 'LiteParserRegistration' in the 'parserMap'.
     * The normal getters rely on a constructed instance of a 'LiteParsedDocumentSource' which
     * doesn't exist in the tests.
     */
    static const LiteParserInfo& getParserInfo_forTest(const std::string& name);

    /**
     * Returns the map of registered lite parsers.
     */
    static const ParserMap& getParserMap();

    std::string _parseTimeName;
    AllowedWithApiStrict _apiStrict;
    AllowedWithClientType _clientType;
};

/**
 * Implementers must define getStageParams() and a parse() function. This should be used with
 * caution. Make sure your stage doesn't need to communicate any special behavior before registering
 * a DocumentSource using this parser. Additionally, explicitly ensure your stage does not require
 * authorization checks.
 */
class MONGO_MOD_OPEN LiteParsedDocumentSourceDefault : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceDefault(const BSONElement& originalBson)
        : LiteParsedDocumentSource(originalBson) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    /**
     * requiresAuthzChecks() is overriden to false because requiredPrivileges() returns an empty
     * vector and has no authz checks by default.
     */
    bool requiresAuthzChecks() const final {
        return false;
    }
};

/**
 * Implementers must define getStageParams() and a parse() function. Note that this requires the
 * privilege on 'internal' actions. This should still be used with caution. Make sure your stage
 * doesn't need to communicate any special behavior before registering a DocumentSource using this
 * parser.
 */
class MONGO_MOD_OPEN LiteParsedDocumentSourceInternal : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceInternal(const BSONElement& originalBson)
        : LiteParsedDocumentSource(originalBson) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {Privilege(ResourcePattern::forClusterResource(boost::none),
                          ActionSet{ActionType::internal})};
    }
};

/**
 * Helper class for DocumentSources which reference a foreign collection.
 */
class LiteParsedDocumentSourceForeignCollection : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceForeignCollection(const BSONElement& originalBson,
                                              NamespaceString foreignNss)
        : LiteParsedDocumentSource(originalBson), _foreignNss(std::move(foreignNss)) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return {_foreignNss};
    }

    PrivilegeVector requiredPrivileges(bool isMongos,
                                       bool bypassDocumentValidation) const override = 0;

protected:
    NamespaceString _foreignNss;
};

/**
 * Helper class for DocumentSources which can reference one or more child pipelines.
 */
class LiteParsedDocumentSourceNestedPipelines : public LiteParsedDocumentSource {
public:
    LiteParsedDocumentSourceNestedPipelines(const BSONElement& originalBson,
                                            boost::optional<NamespaceString> foreignNss,
                                            std::vector<LiteParsedPipeline> pipelines);

    LiteParsedDocumentSourceNestedPipelines(const BSONElement& originalBson,
                                            boost::optional<NamespaceString> foreignNss,
                                            boost::optional<LiteParsedPipeline> pipeline);

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final;

    void getForeignExecutionNamespaces(stdx::unordered_set<NamespaceString>& nssSet) const override;

    bool isExemptFromIngressAdmissionControl() const override;

    Status checkShardedForeignCollAllowed(const NamespaceString& nss,
                                          bool inMultiDocumentTransaction) const override;

    const std::vector<LiteParsedPipeline>& getSubPipelines() const override {
        return _pipelines;
    }

    /**
     * Check the read concern constraints of all sub-pipelines. If the stage that owns the
     * sub-pipelines has its own constraints this should be overridden to take those into account.
     */
    ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override;

protected:
    /**
     * Simple implementation that only gets the privileges needed by children pipelines.
     */
    PrivilegeVector requiredPrivilegesBasic(bool isMongos, bool bypassDocumentValidation) const;

    boost::optional<NamespaceString> _foreignNss;
    std::vector<LiteParsedPipeline> _pipelines;
};

/**
 * Declares and defines a lite-parsed document source class that uses the default implementation and
 * provides stage-specific parameters.
 *
 * This macro creates:
 *   1. A stage-specific parameter class `{stageName}StageParams` using
 * DECLARE_STAGE_PARAMS_DERIVED_DEFAULT.
 *   2. A lite-parsed document source class `{stageName}LiteParsed` that inherits from
 *      LiteParsedDocumentSourceDefault and returns the stage-specific params via getStageParams().
 *
 * Use this macro when a stage needs its own parameter type for identification purposes but
 * doesn't require custom lite parsing behavior beyond the default (which provides no involved
 * namespaces, no required privileges, and no authorization checks).
 *
 * @param stageName The name of the stage (without the "$" prefix). This will be used to generate:
 *                  - The parameter class name: `{stageName}StageParams`.
 *                  - The lite-parsed class name: `{stageName}LiteParsed`.
 *                  - A unique ID for type identification.
 *
 * Example usage:
 *   DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(Test);
 *   // Creates TestLiteParsed class that can be instantiated with:
 *   // auto liteParsed = std::make_unique<TestLiteParsed>(bsonElement);
 *   // auto params = liteParsed->getStageParams(); // Returns TestStageParams.
 */
#define DEFINE_LITE_PARSED_STAGE_DEFAULT_DERIVED(stageName) \
    DEFINE_LITE_PARSED_STAGE_DERIVED_IMPL(stageName, LiteParsedDocumentSourceDefault)

#define DEFINE_LITE_PARSED_STAGE_INTERNAL_DERIVED(stageName) \
    DEFINE_LITE_PARSED_STAGE_DERIVED_IMPL(stageName, LiteParsedDocumentSourceInternal)

#define DEFINE_LITE_PARSED_STAGE_DERIVED_IMPL(stageName, baseType)          \
    DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(stageName);                        \
    class stageName##LiteParsed : public mongo::baseType {                  \
    public:                                                                 \
        stageName##LiteParsed(const mongo::BSONElement& originalBson)       \
            : mongo::baseType(originalBson) {}                              \
        static std::unique_ptr<stageName##LiteParsed> parse(                \
            const mongo::NamespaceString& nss,                              \
            const mongo::BSONElement& spec,                                 \
            const mongo::LiteParserOptions& options) {                      \
            return std::make_unique<stageName##LiteParsed>(spec);           \
        }                                                                   \
        std::unique_ptr<mongo::StageParams> getStageParams() const final {  \
            return std::make_unique<stageName##StageParams>(_originalBson); \
        }                                                                   \
    };

}  // namespace MONGO_MOD_UNFORTUNATELY_OPEN mongo
