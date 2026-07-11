// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {

struct [[MONGO_MOD_PUBLIC]] TimeseriesViewMetadata {
    boost::optional<TimeseriesOptions> options;
    boost::optional<bool> mayContainMixedData;
    boost::optional<bool> usesExtendedRange;
    boost::optional<bool> fixedBuckets;

    static constexpr std::string_view kTimeseriesMayContainMixedData =
        "timeseriesMayContainMixedData";
    static constexpr std::string_view kTimeseriesOptions = "timeseriesOptions";
    static constexpr std::string_view kTimeseriesUsesExtendedRange = "timeseriesUsesExtendedRange";
    static constexpr std::string_view kTimeseriesfixedBuckets = "timeseriesfixedBuckets";

    void serialize(BSONObjBuilder* subObjBuilder) const;
};

// Forward declare LiteParsedPipeline/LiteParserOptions. The cpp implementation will need to rely on
// the clone() implementation for the copy constructor of ResolvedNamespace.
class LiteParsedPipeline;
struct LiteParserOptions;
class OwnedLiteParsedPipeline;

struct [[MONGO_MOD_PUBLIC]] ResolvedNamespaceViewOptions {
    boost::optional<UUID> collUUID = boost::none;
    bool involvedNamespaceIsAView = true;
    boost::optional<TimeseriesViewMetadata> timeseriesMetadata = boost::none;
    std::shared_ptr<LiteParserOptions> options = nullptr;
    bool shouldParseLpp = false;
    bool validateIsNotViewlessTimeseries = false;
    bool isViewlessTimeseries = false;
};
extern const ResolvedNamespaceViewOptions kSimpleViewOptions;

/**
 * Result of resolving a namespace (collection or view) for aggregation pipelines.
 * For views, holds the user-facing namespace, the underlying collection namespace,
 * the view pipeline (raw BSON and optionally parsed), collation, and timeseries
 * metadata. Supports BSON serialization for the ErrorExtraInfo API.
 */
class [[MONGO_MOD_PUBLIC]] ResolvedNamespace final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::CommandOnShardedViewNotSupportedOnMongod;
    // Callback type for desugaring a parsed view pipeline. This indirection exists because
    // ResolvedNamespace lives in the lite_parsed_document_source target while the desugarer lives
    // in its own target that depends on lite_parsed_document_source. A direct call would create a
    // circular dependency, so the desugarer registers itself at startup via a MONGO_INITIALIZER.
    using ViewPipelineDesugarer =
        std::function<bool(LiteParsedPipeline*, std::shared_ptr<IncrementalFeatureRolloutContext>)>;

    // Registers the desugarer callback. Called once at startup from a MONGO_INITIALIZER in
    // lite_parsed_desugarer.cpp.
    static void setViewPipelineDesugarer(ViewPipelineDesugarer fn);

    // (Re-)parses the raw BSON view pipeline into the internal LiteParsedPipeline using this
    // entry's own LiteParserOptions (which carry the operation's IFR context). Always parses fresh
    // from the raw BSON, replacing any previously-parsed pipeline. Callers that need a clean,
    // options-consistent parse of the view definition use this instead of pulling the options out
    // of the entry.
    void liteParseViewPipeline();

    // Desugars the internally-parsed view pipeline in place. Callers must invoke this before
    // passing the ResolvedNamespace to handleView so that extension stages in view definitions
    // are expanded prior to stitching.
    void desugarViewPipeline();

    // Returns a desugared owned clone of the internally-parsed view pipeline.
    LiteParsedPipeline desugarAndCloneViewPipeline() const;

    ResolvedNamespace();
    ResolvedNamespace(const ResolvedNamespace& other);
    ResolvedNamespace& operator=(const ResolvedNamespace& other);
    ResolvedNamespace(ResolvedNamespace&& other) noexcept;
    ResolvedNamespace& operator=(ResolvedNamespace&& other) noexcept;
    ~ResolvedNamespace() override;

    // Constructor for collections or minimal view resolution (used for secondary namespaces).
    // "Secondary" in this context means a namespace discovered while resolving the pipeline (for
    // example via $lookup/$unionWith), not the top-level namespace targeted by the user request.
    ResolvedNamespace(NamespaceString ns_,
                      std::vector<BSONObj> pipeline_,
                      boost::optional<UUID> collUUID_ = boost::none,
                      bool involvedNamespaceIsAView_ = false);

    // Full constructor used for primary view resolution (includes collation and timeseries
    // metadata). "Primary" in this context means the top-level namespace targeted by the user
    // request.
    ResolvedNamespace(NamespaceString userNss,
                      NamespaceString resolvedNss,
                      std::vector<BSONObj> pipeline_,
                      BSONObj defaultCollation,
                      ResolvedNamespaceViewOptions metadata = {});

    // Builds a view ResolvedNamespace whose pipeline is lite-parsed eagerly with the given
    // options (default LiteParserOptions for the overload without one).
    static ResolvedNamespace makeForView(NamespaceString viewName,
                                         NamespaceString resolvedNss,
                                         std::vector<BSONObj> viewPipeBson);
    static ResolvedNamespace makeForView(NamespaceString viewName,
                                         NamespaceString resolvedNss,
                                         std::vector<BSONObj> viewPipeBson,
                                         const LiteParserOptions& options);

    // The namespace as provided by the user (for views, the view name; for collections, the
    // collection's name).
    const NamespaceString& getNamespace() const;
    // The underlying collection namespace (for views, the view's backing collection; for
    // collections, the collection's name).
    const NamespaceString& getResolvedNamespace() const;
    // The view pipeline as raw BSON; empty for non-view namespaces.
    const std::vector<BSONObj>& getBsonPipeline() const;
    // The resolved collection's UUID, when known.
    const boost::optional<UUID>& getCollUUID() const;
    void setCollUUID(boost::optional<UUID> collUUID) {
        _uuid = std::move(collUUID);
    }
    // True if the involved namespace resolved from a view.
    bool isInvolvedNamespaceAView() const;
    // Default collation for the namespace (empty BSONObj means simple collation).
    const BSONObj& getDefaultCollation() const;
    // True if this namespace is a timeseries view.
    bool isTimeseries() const;
    // Timeseries view metadata if this is a timeseries view; otherwise none.
    boost::optional<TimeseriesViewMetadata> getTimeseriesViewMetadata() const;

    // Parsed view pipeline. Requires that the view pipeline was parsed (e.g. via full view
    // resolution).
    LiteParsedPipeline getViewPipeline() const;

    // Mutable raw access to the parsed view pipeline. Returns nullptr if the pipeline was not
    // parsed (e.g. shouldParseLpp was false).
    OwnedLiteParsedPipeline* getMutableParsedPipeline();
    // Read-only access to the parsed view pipeline.
    const LiteParsedPipeline* getParsedPipeline() const;
    // Set/replace the LiteParserOptions on this entry. Used by callers that received an entry over
    // the wire (e.g. mongos harvesting additionalResolvedNamespaces from a kickback) and need to
    // restore the operation's IFR context — the wire format doesn't carry options.
    void setLiteParserOptions(std::shared_ptr<LiteParserOptions> opts) {
        _lpOptions = std::move(opts);
    }
    // Returns the LiteParserOptions associated with this namespace, or nullptr if none were set.
    const std::shared_ptr<LiteParserOptions>& getLiteParserOptions() const {
        return _lpOptions;
    }

    // Same as getBsonPipeline(): the view pipeline as originally specified (raw BSON).
    std::vector<BSONObj> getOriginalBson() const;
    // Desugared view pipeline as BSON (each stage serialized). Requires a parsed view pipeline.
    std::vector<BSONObj> getSerializedViewPipeline() const;
    // Returns a deep copy of this ResolvedNamespace.
    ResolvedNamespace clone() const;

    void setAdditionalResolvedNamespaces(std::vector<ResolvedNamespace> additional) {
        _additionalResolvedNamespaces = std::move(additional);
    }
    const std::vector<ResolvedNamespace>& getAdditionalResolvedNamespaces() const {
        return _additionalResolvedNamespaces;
    }

    // TODO SERVER-125515 Remove sentinel primary notion when last LTS can understand
    // additionalResolvedNamespaces serialization.
    static ResolvedNamespace makeWithSentinelPrimary(std::vector<ResolvedNamespace> additional) {
        ResolvedNamespace rn(NamespaceString(), std::vector<BSONObj>{});
        rn._additionalResolvedNamespaces = std::move(additional);
        rn._hasSentinelPrimary = true;
        return rn;
    }
    bool hasSentinelPrimary() const {
        return _hasSentinelPrimary;
    }

    void applyTimeseriesRewrites(std::vector<BSONObj>* resolvedPipeline) const;
    boost::optional<BSONObj> rewriteIndexHintForTimeseries(const BSONObj& originalHint) const;

    // ErrorExtraInfo API
    static ResolvedNamespace fromBSON(const BSONObj& commandResponseObj);
    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    /*
     * These methods support IDL parsing of ResolvedNamespace.
     */
    static ResolvedNamespace parseFromBSON(const BSONElement& elem);
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const;

private:
    // Core member variables - these are always set, no matter what.

    // The namespace of the underlying collection. If this namespace represents a view, this is the
    // view's underlying collection namespace, not the view namespace.
    NamespaceString _resolvedNss;

    // The view's raw BSON object pipeline (empty for collections). The constructor enforces that
    // every stage is owned, so this map can outlive the view-catalog entry it was built from.
    std::vector<BSONObj> _pipeline;
    boost::optional<UUID> _uuid = boost::none;
    bool _involvedNamespaceIsAView = false;

    // The namespace provided by the user - in the case of the view, this
    // is the view namespace, NOT the underlying collection.
    NamespaceString _userNss;

    // Extended fields for view resolution. These may not be set depending on how this
    // ResolvedNamespace is used/at what time it is created in the query lifecycle.

    // An empty BSONObj indicates simple collation.
    BSONObj _defaultCollation;
    // If this is a timeseries view, this contains the timeseries metadata.
    boost::optional<TimeseriesViewMetadata> _timeseriesMetadata = boost::none;

    std::shared_ptr<LiteParserOptions> _lpOptions = nullptr;
    std::unique_ptr<OwnedLiteParsedPipeline> _parsedPipeline;

    std::vector<ResolvedNamespace> _additionalResolvedNamespaces;
    bool _hasSentinelPrimary = false;

    inline static ViewPipelineDesugarer _viewPipelineDesugarer;
};

/**
 * Map from view to resolved namespace.
 */
using ResolvedNamespaceMap [[MONGO_MOD_PUBLIC]] =
    absl::flat_hash_map<NamespaceString, ResolvedNamespace>;

}  // namespace mongo
