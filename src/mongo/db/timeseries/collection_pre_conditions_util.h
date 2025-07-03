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

#include "mongo/db/namespace_string.h"
#include "mongo/db/timeseries/catalog_helper.h"

namespace mongo::timeseries {

/**
 * Struct that bundles properties about a collection, to be used in an operation that requires
 * checking metadata about a collection repeatedly without holding on to an acquisition for that
 * collection. The pre-conditions should be check against the actual ultimate acquisition.
 *
 * 'wasNssTranslatedToBuckets' is a boolean indicating if the acquisition has been made after
 * translating the namespace to the underlying timeseries system buckets collection. Only true for
 * legacy time-series collections.
 */
struct ExistingCollectionPreConditions {
    UUID collectionUUID;
    bool isTimeseriesCollection;
    bool isViewlessTimeseriesCollection;
    bool wasNssTranslatedToBuckets;
};

struct NonExistentCollectionPreConditions {};

using CollectionPreConditions =
    std::variant<ExistingCollectionPreConditions, NonExistentCollectionPreConditions>;

/**
 * Given an operationContext and a namespace, constructs a CollectionPreConditions struct.
 */
CollectionPreConditions getCollectionPreConditions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    boost::optional<UUID> expectedUUID = boost::none);


/**
 * Given a CollectionPreConditions struct and a CollectionAcquisition, checks whether any of the
 * properties of interest for our collection has changed since constructing the
 * CollectionPreConditions object.
 *
 * Throws if any of the properties have changed.
 */
void checkAcquisitionAgainstPreConditions(const CollectionPreConditions& preConditions,
                                          const CollectionAcquisition& acquisition);

}  // namespace mongo::timeseries
