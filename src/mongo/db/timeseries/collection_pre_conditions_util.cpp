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

#include "mongo/db/timeseries/collection_pre_conditions_util.h"

#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"


namespace mongo::timeseries {

bool CollectionPreConditions::exists() const {
    return _uuid.has_value();
}

UUID CollectionPreConditions::uuid() const {
    tassert(10664100, "Attemped to get the uuid for a collection that doesn't exist", exists());
    return _uuid.get();
}

boost::optional<UUID> CollectionPreConditions::expectedUUID() const {
    return _expectedUUID;
}

bool CollectionPreConditions::isTimeseriesCollection() const {
    return exists() && _isTimeseriesCollection;
}

bool CollectionPreConditions::isViewlessTimeseriesCollection() const {
    return exists() && _isViewlessTimeseriesCollection;
}

bool CollectionPreConditions::isLegacyTimeseriesCollection() const {
    return exists() && isTimeseriesCollection() && !isViewlessTimeseriesCollection();
}

bool CollectionPreConditions::wasNssTranslated() const {
    return exists() && _translatedNss.has_value();
}

NamespaceString CollectionPreConditions::getTargetNs(const NamespaceString& ns) const {
    return wasNssTranslated() ? _translatedNss.get() : ns;
}

void CollectionPreConditions::setIsTimeseriesLogicalRequest(bool isTimeseriesLogicalRequest) {
    _isTimeseriesLogicalRequest = isTimeseriesLogicalRequest;
}

bool CollectionPreConditions::getIsTimeseriesLogicalRequest() const {
    return _isTimeseriesLogicalRequest;
}

CollectionPreConditions CollectionPreConditions::getCollectionPreConditions(
    OperationContext* opCtx, const NamespaceString& nss, boost::optional<UUID> expectedUUID) {

    auto lookupInfo = lookupTimeseriesCollection(opCtx, nss, /*skipSystemBucketLookup=*/false);

    if (!lookupInfo.uuid) {
        return CollectionPreConditions(expectedUUID);
    }

    boost::optional<NamespaceString> translatedNss = boost::none;
    if (lookupInfo.wasNssTranslated) {
        translatedNss = lookupInfo.targetNss;
    }

    return CollectionPreConditions(lookupInfo.uuid.get(),
                                   lookupInfo.isTimeseries,
                                   lookupInfo.isViewlessTimeseries,
                                   translatedNss,
                                   expectedUUID);
}

void CollectionPreConditions::checkAcquisitionAgainstPreConditions(
    OperationContext* opCtx,
    const CollectionPreConditions& preConditions,
    const CollectionAcquisition& acquisition) {

    const auto& nss = acquisition.nss();

    // We expect the collection acquisition to be the primary place where we check the acquired uuid
    // against the expectedUUID. For viewful time-series collections where an expectedUUID is passed
    // in while performing a request on the view namespace, we have the additional expected behavior
    // that we will unconditionally return a CollectionUUIDMismatch error, since a view cannot have
    // a collection UUID. TODO SERVER-101784: Remove this check once 9.0 is LTS and viewful
    // time-series collections are no longer around.
    //
    // We also have an additional safeguard against a scenario where an expectedUUID was passed in
    // for our request, but the expectedUUID was not passed into the acquisition.
    if (preConditions.getIsTimeseriesLogicalRequest() &&
        preConditions.isLegacyTimeseriesCollection()) {
        checkCollectionUUIDMismatch(opCtx, nss, nullptr, preConditions.expectedUUID());
    } else if (preConditions.expectedUUID()) {
        tassert(10811400,
                str::stream() << "Collection UUID does not match that specified for collection "
                              << nss.toStringForErrorMsg() << ", expected "
                              << *preConditions.expectedUUID(),
                acquisition.exists() && preConditions.expectedUUID() == acquisition.uuid());
    }

    if (!preConditions.exists()) {
        uassert(10685100,
                "Collection did not exist at the beginning of operation but has "
                "subsequently been created as a time-series collection",
                !acquisition.exists() || !acquisition.getCollectionPtr()->isTimeseriesCollection());
    } else {
        if (!acquisition.exists()) {
            if (preConditions.isTimeseriesCollection() && !isRawDataOperation(opCtx)) {
                if (preConditions.isViewlessTimeseriesCollection()) {
                    uasserted(ErrorCodes::NamespaceNotFound,
                              str::stream()
                                  << "Timeseries collection " << nss.toStringForErrorMsg()
                                  << " got dropped during the executing of the operation.");
                } else {
                    uasserted(ErrorCodes::NamespaceNotFound,
                              str::stream()
                                  << "Buckets collection not found for time-series collection "
                                  << nss.getTimeseriesViewNamespace().toStringForErrorMsg());
                }
            }
            return;
        }
        uassert(10685101,
                fmt::format("Collection with ns {} has been dropped and "
                            "recreated since the "
                            "beginning of the operation",
                            nss.toStringForErrorMsg()),
                preConditions.uuid() == acquisition.uuid() ||
                    (preConditions.isTimeseriesCollection() ==
                         acquisition.getCollectionPtr()->isTimeseriesCollection() &&
                     preConditions.isViewlessTimeseriesCollection() ==
                         acquisition.getCollectionPtr()->isNewTimeseriesWithoutView()));
    }
}
}  // namespace mongo::timeseries
