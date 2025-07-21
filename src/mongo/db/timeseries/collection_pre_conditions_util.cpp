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

#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/db/timeseries/write_ops/timeseries_write_ops_utils.h"


namespace mongo::timeseries {

bool CollectionPreConditions::exists() const {
    return _uuid.has_value();
}

UUID CollectionPreConditions::uuid() const {
    tassert(10664100, "Attemped to get the uuid for a collection that doesn't exist", exists());
    return _uuid.get();
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

CollectionPreConditions CollectionPreConditions::getCollectionPreConditions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    bool isRawDataRequest,
    boost::optional<UUID> expectedUUID) {
    // Hold reference to the catalog for collection lookup without locks to be safe.
    auto catalog = CollectionCatalog::get(opCtx);
    auto lookupInfo = lookupTimeseriesCollection(opCtx, nss, /*skipSystemBucketLookup=*/false);

    // We pass a nullptr into the checkCollectionUUIDMismatch machinery so that we always throw
    // CollectionUUIDMismatch if an expectedUUID is specified for an operation on the user-facing
    // view namespace of a legacy time-series collection.
    if (expectedUUID && lookupInfo.isTimeseries && !lookupInfo.isViewlessTimeseries &&
        !isRawDataRequest) {
        checkCollectionUUIDMismatch(
            opCtx,
            (nss.isTimeseriesBucketsCollection()) ? nss.getTimeseriesViewNamespace() : nss,
            nullptr,
            expectedUUID);
    } else if (lookupInfo.uuid || expectedUUID) {
        checkCollectionUUIDMismatch(
            opCtx,
            (nss.isTimeseriesBucketsCollection()) ? nss.getTimeseriesViewNamespace() : nss,
            catalog->lookupCollectionByNamespace(opCtx, lookupInfo.targetNss),
            expectedUUID);
    }

    if (!lookupInfo.uuid) {
        return CollectionPreConditions();
    }

    boost::optional<NamespaceString> translatedNss = boost::none;
    if (lookupInfo.wasNssTranslated) {
        translatedNss = lookupInfo.targetNss;
    }

    return CollectionPreConditions(lookupInfo.uuid.get(),
                                   lookupInfo.isTimeseries,
                                   lookupInfo.isViewlessTimeseries,
                                   translatedNss);
}

void CollectionPreConditions::checkAcquisitionAgainstPreConditions(
    OperationContext* opCtx,
    const CollectionPreConditions& preConditions,
    const CollectionAcquisition& acquisition) {

    if (!preConditions.exists()) {
        uassert(10685100,
                "Collection did not exist at the beginning of operation but has "
                "subsequently been created as a time-series collection",
                !acquisition.exists() || !acquisition.getCollectionPtr()->isTimeseriesCollection());
    } else {
        if (!acquisition.exists()) {
            if (preConditions.isTimeseriesCollection()) {
                if (preConditions.isViewlessTimeseriesCollection()) {
                    uasserted(ErrorCodes::NamespaceNotFound,
                              str::stream()
                                  << "Timeseries collection "
                                  << acquisition.nss().toStringForErrorMsg()
                                  << " got dropped during the executing of the operation.");
                } else {
                    uasserted(ErrorCodes::NamespaceNotFound,
                              str::stream()
                                  << "Buckets collection not found for time-series collection "
                                  << acquisition.nss()
                                         .getTimeseriesViewNamespace()
                                         .toStringForErrorMsg());
                }
            }
            return;
        }
        uassert(10685101,
                fmt::format("Collection with ns {} has been dropped and "
                            "recreated since the "
                            "beginning of the operation",
                            acquisition.nss().toStringForErrorMsg()),
                preConditions.uuid() == acquisition.uuid() ||
                    (preConditions.isTimeseriesCollection() ==
                         acquisition.getCollectionPtr()->isTimeseriesCollection() &&
                     preConditions.isViewlessTimeseriesCollection() ==
                         acquisition.getCollectionPtr()->isNewTimeseriesWithoutView()));
    }
}
}  // namespace mongo::timeseries
