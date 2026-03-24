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
#include "mongo/base/error_extra_info.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Represents a resolved definition, composed of a base collection namespace and a pipeline
 * built from one or more views.
 *
 * TODO SERVER-122118 Remove this class once all callers have been transitioned to the new
 * ResolvedNamespace.
 */
class MONGO_MOD_PUBLIC ResolvedView final : public ErrorExtraInfo {
public:
    ResolvedView(const NamespaceString& collectionNs,
                 std::vector<BSONObj> pipeline,
                 BSONObj defaultCollation,
                 boost::optional<TimeseriesOptions> timeseriesOptions = boost::none,
                 boost::optional<bool> timeseriesMayContainMixedData = boost::none,
                 boost::optional<bool> timeseriesUsesExtendedRange = boost::none,
                 boost::optional<bool> timeseriesFixedBuckets = boost::none,
                 const bool isNewTimeseriesWithoutView = false)
        : _wrappedNamespace(ResolvedNamespace(
              collectionNs,
              collectionNs,
              std::move(pipeline),
              std::move(defaultCollation),
              ResolvedNamespaceViewOptions{.timeseriesMetadata =
                                               TimeseriesViewMetadata{timeseriesOptions,
                                                                      timeseriesMayContainMixedData,
                                                                      timeseriesUsesExtendedRange,
                                                                      timeseriesFixedBuckets},
                                           .validateIsNotViewlessTimeseries = true,
                                           .isViewlessTimeseries = isNewTimeseriesWithoutView})) {}

    ResolvedView(const NamespaceString& userNss,
                 const NamespaceString& collectionNs,
                 std::vector<BSONObj> pipeline,
                 BSONObj defaultCollation,
                 boost::optional<TimeseriesOptions> timeseriesOptions = boost::none,
                 boost::optional<bool> timeseriesMayContainMixedData = boost::none,
                 boost::optional<bool> timeseriesUsesExtendedRange = boost::none,
                 boost::optional<bool> timeseriesFixedBuckets = boost::none,
                 const bool isNewTimeseriesWithoutView = false)
        : _wrappedNamespace(ResolvedNamespace(
              userNss,
              collectionNs,
              std::move(pipeline),
              std::move(defaultCollation),
              ResolvedNamespaceViewOptions{.timeseriesMetadata =
                                               TimeseriesViewMetadata{timeseriesOptions,
                                                                      timeseriesMayContainMixedData,
                                                                      timeseriesUsesExtendedRange,
                                                                      timeseriesFixedBuckets},
                                           .validateIsNotViewlessTimeseries = true,
                                           .isViewlessTimeseries = isNewTimeseriesWithoutView})) {}

    ResolvedView(const ResolvedNamespace& resolvedNamespace)
        : _wrappedNamespace(resolvedNamespace) {}

    static ResolvedView fromBSON(const BSONObj& commandResponseObj);

    /**
     * Applies timeseries-specific rewrites to the resolved pipeline.
     */
    void applyTimeseriesRewrites(std::vector<BSONObj>* resolvedPipeline) const;

    /**
     * Rewrites an index hint over a time-series view to be a hint over the underlying buckets
     * collection. If the hint cannot or does not need to be rewritten, returns boost::none.
     */
    boost::optional<BSONObj> rewriteIndexHintForTimeseries(const BSONObj& originalHint) const;

    const NamespaceString& getNamespace() const {
        return _wrappedNamespace.getResolvedNamespace();
    }

    const std::vector<BSONObj>& getPipeline() const {
        return _wrappedNamespace.getBsonPipeline();
    }

    const BSONObj& getDefaultCollation() const {
        return _wrappedNamespace.getDefaultCollation();
    }

    bool isTimeseries() const {
        return _wrappedNamespace.isTimeseries();
    }

    boost::optional<TimeseriesOptions> getTimeseriesOptions() const {
        auto metadata = _wrappedNamespace.getTimeseriesViewMetadata();
        return metadata ? metadata->options : boost::none;
    }

    boost::optional<bool> getMayContainMixedData() const {
        auto metadata = _wrappedNamespace.getTimeseriesViewMetadata();
        return metadata ? metadata->mayContainMixedData : boost::none;
    }

    boost::optional<bool> getUsesExtendedRange() const {
        auto metadata = _wrappedNamespace.getTimeseriesViewMetadata();
        return metadata ? metadata->usesExtendedRange : boost::none;
    }

    boost::optional<bool> getFixedBuckets() const {
        auto metadata = _wrappedNamespace.getTimeseriesViewMetadata();
        return metadata ? metadata->fixedBuckets : boost::none;
    }

    // ErrorExtraInfo API
    static constexpr auto code = ErrorCodes::CommandOnShardedViewNotSupportedOnMongod;
    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);
    static ResolvedView parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

    /**
     * Constructs a ViewInfo struct from the ResolvedView.
     */
    ViewInfo toViewInfo(const NamespaceString& viewNss,
                        const LiteParserOptions& options = LiteParserOptions{}) const {
        return ViewInfo{viewNss,
                        _wrappedNamespace.getResolvedNamespace(),
                        _wrappedNamespace.getBsonPipeline(),
                        options};
    }

private:
    ResolvedNamespace _wrappedNamespace;
};

}  // namespace mongo
