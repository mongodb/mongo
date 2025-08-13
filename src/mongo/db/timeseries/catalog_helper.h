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

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/timeseries/timeseries_gen.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class NamespaceString;
class OperationContext;

/**
 * Namespace for helper functions related to time-series collections.
 */
namespace timeseries {

/**
 * This function is a wrapper of `acquireCollectionOrView`.
 *
 * It returns a pair where the first element is the resulting collection or view acquisition.
 *
 * The second element is a boolean indicating if the acquisition has been made after translating the
 * namespace to the underlying timeseries system buckets collection. This boolean will be set to
 * true only for existing legacy timeseries collection (view + buckets).
 *
 * MODE_IS acquisition requests are implicitly converted to `maybeLockFree`.
 *
 * TODO SERVER-101784 remove this function once 9.0 becomes last LTS. By then only viewless
 * timeseries collection will exist.
 */
std::pair<CollectionOrViewAcquisition, bool> acquireCollectionOrViewWithBucketsLookup(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionReq, LockMode mode);

/**
 * Similar to acquireCollectionOrViewWithBucketsLookup above, but it will throw
 * `CommandNotSupportedOnView` if the target namespace is a normal view.
 *
 * For timeseries view instead it will return the CollectionAcquistion of the underlying
 * system.buckets collection.
 *
 * TODO SERVER-101784 remove this function once 9.0 becomes last LTS. By then only viewless
 * timeseries collection will exist.
 */
std::pair<CollectionAcquisition, bool> acquireCollectionWithBucketsLookup(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionReq, LockMode mode);

struct TimeseriesLookupInfo {
    // If the namespace refer to a timeseries collection
    bool isTimeseries;
    // If the namespace refers to a viewless time-series collection
    bool isViewlessTimeseries;
    // If the namespace was translated from view to system.buckets collection
    bool wasNssTranslated;
    // The namespace of the target buckets collection
    NamespaceString targetNss;
    // The UUID for the collection. boost::none if no such collection exists
    boost::optional<UUID> uuid;
};

/**
 * Returns timeseries information about the given namespace timeseries.
 *
 * Throws if this is a time-series collection but the timeseries options are not valid.
 *
 * TODO SERVER-101784 simplify this function once 9.0 becomes last LTS, considering that we will not
 * need to take into account legacy timeseries collection.
 */
TimeseriesLookupInfo lookupTimeseriesCollection(OperationContext* opCtx,
                                                const NamespaceStringOrUUID& nssOrUUID,
                                                bool skipSystemBucketLookup);
/**
 * Returns a copy of the time-series options for namespace 'nss', if 'nss' refers to a time-series
 * collection. Otherwise returns boost::none.
 */
boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        bool convertToBucketsNamespace);

}  // namespace timeseries
}  // namespace mongo
