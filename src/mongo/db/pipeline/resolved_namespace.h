/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <vector>

#include <absl/container/flat_hash_map.h>
#include <boost/optional/optional.hpp>

namespace mongo {

struct MONGO_MOD_PUBLIC TimeseriesViewMetadata {
    boost::optional<TimeseriesOptions> options;
    boost::optional<bool> mayContainMixedData;
    boost::optional<bool> usesExtendedRange;
    boost::optional<bool> fixedBuckets;

    static constexpr StringData kTimeseriesMayContainMixedData = "timeseriesMayContainMixedData"_sd;
    static constexpr StringData kTimeseriesOptions = "timeseriesOptions"_sd;
    static constexpr StringData kTimeseriesUsesExtendedRange = "timeseriesUsesExtendedRange"_sd;
    static constexpr StringData kTimeseriesfixedBuckets = "timeseriesfixedBuckets"_sd;

    void serialize(BSONObjBuilder* optionsBuilder, BSONObjBuilder* subObjBuilder) const;
};

// Forward declare LiteParsedPipeline/LiteParserOptions. The cpp implementation will need to rely on
// the clone() implementation for the copy constructor of ResolvedNamespace.
class LiteParsedPipeline;
struct LiteParserOptions;

struct MONGO_MOD_PUBLIC ResolvedNamespaceViewOptions {
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
class MONGO_MOD_PUBLIC ResolvedNamespace {
public:
    // Callback type for desugaring a parsed view pipeline. This indirection exists because
    // ResolvedNamespace lives in the lite_parsed_document_source target while the desugarer lives
    // in its own target that depends on lite_parsed_document_source. A direct call would create a
    // circular dependency, so the desugarer registers itself at startup via a MONGO_INITIALIZER.
    using ViewPipelineDesugarer =
        std::function<bool(LiteParsedPipeline*, std::shared_ptr<IncrementalFeatureRolloutContext>)>;

    // Registers the desugarer callback. Called once at startup from a MONGO_INITIALIZER in
    // lite_parsed_desugarer.cpp.
    static void setViewPipelineDesugarer(ViewPipelineDesugarer fn);

    // Desugars the internally-parsed view pipeline in place. Callers (e.g. applyViewToLiteParsed)
    // must invoke this before passing the ResolvedNamespace to handleView so that extension stages
    // in view definitions are expanded prior to stitching.
    void desugarViewPipeline();

    ResolvedNamespace();
    ResolvedNamespace(const ResolvedNamespace& other);
    ResolvedNamespace& operator=(const ResolvedNamespace& other);
    ResolvedNamespace(ResolvedNamespace&& other) noexcept;
    ResolvedNamespace& operator=(ResolvedNamespace&& other) noexcept;
    ~ResolvedNamespace();

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

    // The namespace as provided by the user (for views, the view name; for collections, the
    // collection's name).
    const NamespaceString& getNamespace() const;
    // The underlying collection namespace (for views, the view's backing collection; for
    // collections, the collection's name).
    const NamespaceString& getResolvedNamespace() const;
    // The view pipeline as raw BSON; empty for non-view namespaces.
    const std::vector<BSONObj>& getBsonPipeline() const;
    // Default collation for the namespace (empty BSONObj means simple collation).
    const BSONObj& getDefaultCollation() const;
    // True if this namespace is a timeseries view.
    bool isTimeseries() const;
    // Timeseries view metadata if this is a timeseries view; otherwise none.
    boost::optional<TimeseriesViewMetadata> getTimeseriesViewMetadata() const;

    // Parsed view pipeline. Requires that the view pipeline was parsed (e.g. via full view
    // resolution).
    LiteParsedPipeline getViewPipeline() const;
    // Same as getBsonPipeline(): the view pipeline as originally specified (raw BSON).
    std::vector<BSONObj> getOriginalBson() const;
    // Desugared view pipeline as BSON (each stage serialized). Requires a parsed view pipeline.
    std::vector<BSONObj> getSerializedViewPipeline() const;
    // Returns a deep copy of this ResolvedNamespace.
    ResolvedNamespace clone() const;

    // ErrorExtraInfo API
    // TODO SERVER-122118 Change ResolvedNamespace to inherit from ErrorExtraInfo, to replace
    // ResolvedView.
    static ResolvedNamespace fromBSON(const BSONObj& commandResponseObj);
    void serialize(BSONObjBuilder* bob) const;
    static std::shared_ptr<const ResolvedNamespace> parse(const BSONObj&);

    /*
     * These methods support IDL parsing of ResolvedNamespace.
     */
    static ResolvedNamespace parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

    // TODO SERVER-122119 Rename these variables to use the "_" prefix and make them private/add
    // getters.

    // The namespace of the underlying collection. If this namespace
    // represents a view, this contains the namespace of the view's underlying
    // collection, NOT the view namespace.
    // TODO SERVER-122119 It would be helpful to name this _resolvedNss;
    NamespaceString ns;
    // The view's raw BSON object pipeline (empty for collections).
    std::vector<BSONObj> pipeline;
    boost::optional<UUID> uuid = boost::none;
    bool involvedNamespaceIsAView = false;

private:
    void liteParseViewPipeline();

    // Core member variables - these are always set, no matter what.

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
    std::unique_ptr<LiteParsedPipeline> _parsedPipeline;

    inline static ViewPipelineDesugarer _viewPipelineDesugarer;
};

/**
 * Map from view to resolved namespace.
 */
using ResolvedNamespaceMap MONGO_MOD_PUBLIC =
    absl::flat_hash_map<NamespaceString, ResolvedNamespace>;

}  // namespace mongo
