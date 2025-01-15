/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo::timeseries {

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
