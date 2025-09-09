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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/version_context.h"

#include <memory>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Represents a resolved definition, composed of a base collection namespace and a pipeline
 * built from one or more views.
 */
class ResolvedView final : public ErrorExtraInfo {
public:
    ResolvedView(const NamespaceString& collectionNs,
                 std::vector<BSONObj> pipeline,
                 BSONObj defaultCollation,
                 boost::optional<TimeseriesOptions> timeseriesOptions = boost::none,
                 boost::optional<bool> timeseriesMayContainMixedData = boost::none,
                 boost::optional<bool> timeseriesUsesExtendedRange = boost::none,
                 boost::optional<bool> timeseriesfixedBuckets = boost::none,
                 const bool isNewTimeseriesWithoutView = false)
        : _namespace(collectionNs),
          _pipeline(std::move(pipeline)),
          _defaultCollation(std::move(defaultCollation)),
          _timeseriesOptions(timeseriesOptions),
          _timeseriesMayContainMixedData(timeseriesMayContainMixedData),
          _timeseriesUsesExtendedRange(timeseriesUsesExtendedRange),
          _timeseriesfixedBuckets(timeseriesfixedBuckets) {
        // If we reach here with a timeseries query, it will be because we're working with a
        // view-based timeseries collection. Viewless timeseries collections should be defined
        // already and should not trigger this kickback at all.
        //
        // TODO(SERVER-100862): This check should be removed once the isNewTimeseriesWithoutView
        // parameter has been removed.
        tassert(9950300,
                (std::stringstream{}
                 << "Should not be performing view resolution on viewless timeseries collection: "
                 << collectionNs.toStringForErrorMsg())
                    .str(),
                !isNewTimeseriesWithoutView);
    }

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
        return _namespace;
    }

    const std::vector<BSONObj>& getPipeline() const {
        return _pipeline;
    }

    const BSONObj& getDefaultCollation() const {
        return _defaultCollation;
    }

    bool timeseries() const {
        return _timeseriesOptions.has_value();
    }

    // ErrorExtraInfo API
    static constexpr auto code = ErrorCodes::CommandOnShardedViewNotSupportedOnMongod;
    static constexpr StringData kTimeseriesMayContainMixedData = "timeseriesMayContainMixedData"_sd;
    static constexpr StringData kTimeseriesOptions = "timeseriesOptions"_sd;
    static constexpr StringData kTimeseriesUsesExtendedRange = "timeseriesUsesExtendedRange"_sd;
    static constexpr StringData kTimeseriesfixedBuckets = "timeseriesfixedBuckets"_sd;

    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    /*
     * These methods support IDL parsing of ResolvedView.
     */
    static ResolvedView parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;

private:
    NamespaceString _namespace;
    std::vector<BSONObj> _pipeline;

    // The default collation associated with this view. An empty object means that the default
    // is the simple collation.
    //
    // Currently all operations which run over a view must use the default collation. This means
    // that operations on the view which do not specify a collation inherit the default.
    // Operations on the view which specify any other collation fail with a user error.
    BSONObj _defaultCollation;

    boost::optional<TimeseriesOptions> _timeseriesOptions;
    boost::optional<bool> _timeseriesMayContainMixedData;
    boost::optional<bool> _timeseriesUsesExtendedRange;
    boost::optional<bool> _timeseriesfixedBuckets;
};

class PipelineResolver {
public:
    /**
     * Constructs a new aggregation request which targets the base collection of 'resolvedView'
     * and applies the view pipeline to the original aggregation request pipeline.
     */
    static AggregateCommandRequest buildRequestWithResolvedPipeline(
        const ResolvedView& resolvedView, const AggregateCommandRequest& request);
};

}  // namespace mongo
