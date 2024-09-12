/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/util/assert_util.h"

namespace mongo::timeseries {
/**
 * Translates the given query on the time-series collection to a query on the time-series
 * collection's underlying buckets collection. Creates and returns a translated query document where
 * all occurrences of metaField in query are replaced with the literal "meta". Requires that the
 * given metaField is not empty.
 */
BSONObj translateQuery(const BSONObj& query, StringData metaField);

/**
 * Translates the given update on the time-series collection to an update on the time-series
 * collection's underlying buckets collection. Creates and returns a translated UpdateModification
 * where all occurrences of metaField in updateMod are replaced with the literal "meta". Requires
 * that updateMod is an update document and that the given metaField is not empty. Returns an
 * invalid status if the update cannot be translated.
 */
StatusWith<write_ops::UpdateModification> translateUpdate(
    const write_ops::UpdateModification& updateMod, boost::optional<StringData> metaField);

/**
 * Returns the function to use to count the number of documents updated or deleted.
 */
std::function<size_t(const BSONObj&)> numMeasurementsForBucketCounter(StringData timeField);

/**
 * Translates the query into a query on the time-series collection's underlying buckets collection
 * and splits out the meta field predicate out of the query and renames it to 'meta'.
 */
BSONObj getBucketLevelPredicateForRouting(const BSONObj& originalQuery,
                                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                          const TimeseriesOptions& tsOptions,
                                          bool allowArbitraryWrites);

/**
 * Returns the match expressions for the bucket and residual filters for a timeseries write
 * operation.
 */
TimeseriesWritesQueryExprs getMatchExprsForWrites(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& writeQuery,
    bool fixedBuckets);

template <typename T>
concept HasGetNamespace = requires(const T& t) {
    t.getNamespace();
};

template <typename T>
concept HasGetNsString = requires(const T& t) {
    t.getNsString();
};

// Type requirement 1 for isTimeseriesViewRequest()
template <typename T>
concept HasNsGetter = HasGetNamespace<T> || HasGetNsString<T>;

// Type requirement 2 for isTimeseriesViewRequest()
template <typename T>
concept HasGetIsTimeseriesNamespace = requires(const T& t) {
    t.getIsTimeseriesNamespace();
};

// Type requirements for isTimeseriesViewRequest()
template <typename T>
concept IsRequestableOnTimeseriesView = HasNsGetter<T> || HasGetIsTimeseriesNamespace<T>;

/**
 * Returns a pair of (whether 'request' is made on a timeseries view and the timeseries system
 * bucket collection namespace if so).
 *
 * If the 'request' is not made on a timeseries view, the second element of the pair is same as the
 * namespace of the 'request'.
 *
 * Throws if this is a time-series view request but the buckets collection is not valid.
 */
template <typename T>
requires IsRequestableOnTimeseriesView<T> std::pair<bool, NamespaceString> isTimeseriesViewRequest(
    OperationContext* opCtx, const T& request) {
    const auto nss = [&] {
        if constexpr (HasGetNamespace<T>) {
            return request.getNamespace();
        } else {
            return request.getNsString();
        }
    }();
    uassert(5916400,
            "'isTimeseriesNamespace' parameter can only be set when the request is sent on "
            "system.buckets namespace",
            !request.getIsTimeseriesNamespace() || nss.isTimeseriesBucketsCollection());

    const auto bucketNss =
        request.getIsTimeseriesNamespace() ? nss : nss.makeTimeseriesBucketsNamespace();

    // If the buckets collection exists now, the time-series insert path will check for the
    // existence of the buckets collection later on with a lock.
    // If this check is concurrent with the creation of a time-series collection and the buckets
    // collection does not yet exist, this check may return false unnecessarily. As a result, an
    // insert attempt into the time-series namespace will either succeed or fail, depending on who
    // wins the race.
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto coll = catalog->lookupCollectionByNamespace(opCtx, bucketNss);
    if (!coll) {
        return {false, nss};
    }

    if (auto options = coll->getTimeseriesOptions()) {
        uassert(ErrorCodes::InvalidOptions,
                "Time-series buckets collection is not clustered",
                coll->isClustered());

        uassertStatusOK(validateBucketingParameters(*options));

        return {true, bucketNss};
    }

    return {false, nss};
}
}  // namespace mongo::timeseries
