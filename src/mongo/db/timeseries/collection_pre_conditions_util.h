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

#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo::timeseries {

/**
 * Class that bundles properties about a collection, to be used in an operation that requires
 * checking metadata about a collection repeatedly without holding on to an acquisition for that
 * collection. The pre-conditions should be check against the actual ultimate acquisition.
 *
 * 'wasNssTranslatedToBuckets' is a boolean indicating if the acquisition has been made after
 * translating the namespace to the underlying timeseries system buckets collection. Only true for
 * legacy time-series collections.
 */
class CollectionPreConditions {
public:
    explicit CollectionPreConditions(boost::optional<UUID> expectedUUID)
        : _uuid(boost::none),
          _isTimeseriesCollection(false),
          _expectedUUID(expectedUUID),
          _isViewlessTimeseriesCollection(false),
          _isTimeseriesLogicalRequest(false),
          _translatedNss(boost::none) {};

    CollectionPreConditions(UUID collectionUUID,
                            bool isTimeseriesCollection,
                            bool isViewlessTimeseriesCollection,
                            boost::optional<NamespaceString> translatedNss,
                            boost::optional<UUID> expectedUUID)
        : _uuid(collectionUUID),
          _isTimeseriesCollection(isTimeseriesCollection),
          _expectedUUID(expectedUUID),
          _isViewlessTimeseriesCollection(isViewlessTimeseriesCollection),
          _isTimeseriesLogicalRequest(false),
          _translatedNss(translatedNss) {};
    ~CollectionPreConditions() = default;

    /**
     * Returns whether an existing collection was found when constructing a CollectionPreConditions
     * object.
     */
    bool exists() const;

    /*
     * Returns the UUID of the target collection. Fails if the collection does not exist.
     */
    UUID uuid() const;

    /**
     * Returns the expected UUID if one was passed in, boost::none otherwise.
     */
    boost::optional<UUID> expectedUUID() const;

    /**
     * Returns a bool indicating whether the target collection is a time-series collection.
     * Returns false if the collection does not exist.
     */
    bool isTimeseriesCollection() const;

    /**
     * Returns a bool indicating whether the target collection is a viewless time-series
     * collection. Returns false if the collection does not exist.
     */
    bool isViewlessTimeseriesCollection() const;

    /**
     * Returns a bool indicating whether this the target collection is a legacy time-series
     * collection. Returns false if the collection does not exist.
     */
    bool isLegacyTimeseriesCollection() const;

    /**
     * Returns the translated namespace for a request, if the the collection acquisition that
     * constructed this CollectionPreConditions instance has been made after translating the
     * namespace to the underlying timeseries system buckets collection. This is only the case for
     * legacy time-series collections. Otherwise, returns the same namespace that is passed in.
     * TODO SERVER-101784 Remove this once 9.0 becomes last-LTS and there are no more legacy
     * time-series collections.
     */
    NamespaceString getTargetNs(const NamespaceString& ns) const;

    /**
     * Returns whether the namespace was translated from the request when constructing this
     * CollectionPreConditions object. Only true for legacy time-series collections.
     * TODO SERVER-101784 Remove this once 9.0 becomes last-LTS and there are no more legacy
     * time-series collections.
     */
    bool wasNssTranslated() const;

    /**
     * Returns whether the request was on a time-series view. This can not always be re-constructed
     * from the other information in this struct, because of the edge case where operations on
     * sharded legacy time-series collections operate on the buckets namespace and the only way to
     * differentiate between logical and buckets-level operations is by checking the
     * isTimeseriesNamespace flag on the request.
     *
     * TODO SERVER-101784 Remove this once 9.0 becomes last-LTS and there are no more legacy
     * time-series collections, and it becomes possible to differentiate raw data requests based
     * only on information in the operationContext
     */
    bool getIsTimeseriesLogicalRequest() const;

    void setIsTimeseriesLogicalRequest(bool isTimeseriesLogicalRequest);

    /**
     * Given a namespace, performs a catalog lookup of the collection for that namespace if one
     * exists and constructs a CollectionPreConditions object.
     */
    static CollectionPreConditions getCollectionPreConditions(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              boost::optional<UUID> expectedUUID);

    /**
     * Given a CollectionPreConditions struct and a CollectionAcquisition, checks whether any of the
     * properties of interest for our collection has changed since constructing the
     * CollectionPreConditions object.
     *
     * Throws if any of the properties have changed.
     */
    static void checkAcquisitionAgainstPreConditions(OperationContext* opCtx,
                                                     const CollectionPreConditions& preConditions,
                                                     const CollectionAcquisition& acquisition);

private:
    boost::optional<UUID> _uuid;
    bool _isTimeseriesCollection;
    boost::optional<UUID> _expectedUUID;
    // TODO SERVER-101784 Remove the three fields below once 9.0 becomes LTS, at which point only
    // viewless time-series collections will exist.
    bool _isViewlessTimeseriesCollection;
    bool _isTimeseriesLogicalRequest;
    boost::optional<NamespaceString> _translatedNss;
};

}  // namespace mongo::timeseries
