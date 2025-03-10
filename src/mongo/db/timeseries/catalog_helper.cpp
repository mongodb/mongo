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

#include "mongo/db/timeseries/catalog_helper.h"

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo {

namespace timeseries {

boost::optional<TimeseriesOptions> getTimeseriesOptions(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        bool convertToBucketsNamespace) {
    auto bucketsNs = convertToBucketsNamespace ? nss.makeTimeseriesBucketsNamespace() : nss;
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsNs);
    if (!bucketsColl) {
        return boost::none;
    }
    return bucketsColl->getTimeseriesOptions();
}

TimeseriesLookupInfo lookupTimeseriesCollection(OperationContext* opCtx,
                                                const NamespaceStringOrUUID& nssOrUUID,
                                                bool skipSystemBucketLookup) {
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);

    auto nss = [&] {
        if (nssOrUUID.isNamespaceString()) {
            return nssOrUUID.nss();
        } else {
            return catalog->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);
        }
    }();

    // If the buckets collection exists now, the time-series insert path will check for the
    // existence of the buckets collection later on with a lock.
    // If this check is concurrent with the creation of a time-series collection and the buckets
    // collection does not yet exist, this check may return false unnecessarily. As a result, an
    // insert attempt into the time-series namespace will either succeed or fail, depending on
    // who wins the race.

    /**
     * Returns true if the given `coll` is a timeseries collection
     * Throws errors in case the timeseries collection exists with invalid options.
     */
    auto isValidTimeseriesColl = [](const Collection* coll) {
        if (coll && coll->isTimeseriesCollection()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Time-series buckets collection is not clustered",
                    coll->isClustered());
            uassertStatusOK(validateBucketingParameters(coll->getTimeseriesOptions().get()));
            return true;
        }
        return false;
    };

    auto coll = catalog->lookupCollectionByNamespace(opCtx, nss);
    if (coll) {
        if (isValidTimeseriesColl(coll)) {
            // Received nss exists and it is timeseries collection
            return {true, false, nss};
        } else {
            // Received nss exists but it is not timeseries collection
            return {false, false, nss};
        }
    }

    // Received nss does not exist.
    // Check the buckets collection directly if we didn't translated the namespace yet.
    if (!skipSystemBucketLookup) {
        const auto bucketsCollNss = nss.makeTimeseriesBucketsNamespace();
        auto bucketsColl = catalog->lookupCollectionByNamespace(opCtx, bucketsCollNss);
        if (isValidTimeseriesColl(bucketsColl)) {
            return {true, true, bucketsCollNss};
        }
        // There could be also buckets collections without timeseries options (SERVER-99290).
    }

    // neither the received nss nor the buckets nss are existing timeseries collections
    return {false, false, nss};
}

}  // namespace timeseries
}  // namespace mongo
