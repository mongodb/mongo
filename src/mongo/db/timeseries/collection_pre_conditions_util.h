// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

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
        : _expectedUUID(expectedUUID) {}

    CollectionPreConditions(UUID collectionUUID,
                            bool isTimeseriesCollection,
                            bool isViewlessTimeseriesCollection,
                            boost::optional<NamespaceString> translatedNss,
                            boost::optional<UUID> expectedUUID)
        : _uuid(collectionUUID),
          _isTimeseriesCollection(isTimeseriesCollection),
          _expectedUUID(expectedUUID),
          _isViewlessTimeseriesCollection(isViewlessTimeseriesCollection),
          _translatedNss(translatedNss) {}

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

    /**
     * Acquires the collection described by this precondition instance and verifies
     * that its current state is still compatible with the CollectionPreConditions
     * captured at the start of the operation, uasserting on any mismatch.
     */
    CollectionAcquisition acquireCollectionAndCheck(OperationContext* opCtx,
                                                    CollectionAcquisitionRequest acquisitionReq,
                                                    LockMode mode) const;

private:
    boost::optional<UUID> _uuid;
    bool _isTimeseriesCollection = false;
    boost::optional<UUID> _expectedUUID;
    // TODO SERVER-101784 Remove the three fields below once 9.0 becomes LTS, at which point only
    // viewless time-series collections will exist.
    bool _isViewlessTimeseriesCollection = false;
    bool _isTimeseriesLogicalRequest = false;
    boost::optional<NamespaceString> _translatedNss;
};

}  // namespace mongo::timeseries
