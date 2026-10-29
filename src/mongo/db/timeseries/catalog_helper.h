// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo::timeseries {

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

/**
 * Similar to `acquireCollectionOrViewWithBucketsLookup`, but it will acquire _both_ the buckets
 * collection and the view for viewful timeseries collections.
 * This is useful for DDLs like `collMod` or `drop`, which operate over both.
 */
struct CollectionOrViewAcquisitionPlusTimeseriesView {
    // The acquired collection or view.
    // For viewful timeseries, this is the system.buckets collection.
    CollectionOrViewAcquisition target;
    // If target is a system.buckets collection with timeseries options,
    // the corresponding timeseries view, if one exists.
    boost::optional<ViewAcquisition> timeseriesView;
    // The system.views collection acquisition. Populated when 'target' is a view
    // or when 'timeseriesView' is present (i.e., whenever the view catalog may be modified).
    boost::optional<CollectionAcquisition> systemViews;
};

CollectionOrViewAcquisitionPlusTimeseriesView acquireCollectionOrViewPlusTimeseriesView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionReq);

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

}  // namespace mongo::timeseries
