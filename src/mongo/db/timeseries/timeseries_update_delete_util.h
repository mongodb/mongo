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

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/ops/parsed_writes_common.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/timeseries/timeseries_gen.h"

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
                                          const TimeseriesOptions& tsOptions);

/**
 * Returns the match expressions for the bucket and residual filters for a timeseries write
 * operation.
 */
TimeseriesWritesQueryExprs getMatchExprsForWrites(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const TimeseriesOptions& tsOptions,
    const BSONObj& writeQuery);

// Type requirement 1 for isTimeseries()
template <typename T>
constexpr bool isRequestableWithTimeseriesBucketNamespace =
    std::is_same_v<T, write_ops::InsertCommandRequest> ||
    std::is_same_v<T, write_ops::UpdateCommandRequest> ||
    std::is_same_v<T, write_ops::DeleteCommandRequest>;

// Type requirement 2 for isTimeseries()
template <typename T>
constexpr bool isRequestableOnUserTimeseriesNamespace =
    std::is_same_v<T, DeleteRequest> || std::is_same_v<T, write_ops::FindAndModifyCommandRequest>;

// Disjuction of type requirements for isTimeseries()
template <typename T>
constexpr bool isRequestableOnTimeseries =
    isRequestableWithTimeseriesBucketNamespace<T> || isRequestableOnUserTimeseriesNamespace<T>;

/**
 * Returns a pair of (whether 'request' is made on a timeseries collection and the timeseries
 * system bucket collection namespace if so).
 *
 * If the 'request' is not made on a timeseries collection, the second element of the pair is same
 * as the namespace of the 'request'.
 */
template <typename T>
requires isRequestableOnTimeseries<T> std::pair<bool, NamespaceString> isTimeseries(
    OperationContext* opCtx, const T& request) {
    const auto [nss, bucketNss] = [&] {
        if constexpr (isRequestableWithTimeseriesBucketNamespace<T>) {
            auto nss = request.getNamespace();
            uassert(5916400,
                    "'isTimeseriesNamespace' parameter can only be set when the request is sent on "
                    "system.buckets namespace",
                    !request.getIsTimeseriesNamespace() || nss.isTimeseriesBucketsCollection());
            return request.getIsTimeseriesNamespace()
                ? std::pair{nss, nss}
                : std::pair{nss, nss.makeTimeseriesBucketsNamespace()};
        } else if constexpr (std::is_same_v<T, write_ops::FindAndModifyCommandRequest>) {
            auto nss = request.getNamespace();
            return std::pair{nss, nss.makeTimeseriesBucketsNamespace()};
        } else {
            auto nss = request.getNsString();
            return std::pair{nss, nss.makeTimeseriesBucketsNamespace()};
        }
    }();

    // If the buckets collection exists now, the time-series insert path will check for the
    // existence of the buckets collection later on with a lock.
    // If this check is concurrent with the creation of a time-series collection and the buckets
    // collection does not yet exist, this check may return false unnecessarily. As a result, an
    // insert attempt into the time-series namespace will either succeed or fail, depending on who
    // wins the race.
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto coll = catalog->lookupCollectionByNamespace(opCtx, bucketNss);
    bool isTimeseries = (coll && coll->getTimeseriesOptions());

    return {isTimeseries, isTimeseries ? bucketNss : nss};
}
}  // namespace mongo::timeseries
