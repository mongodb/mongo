// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/collection_pre_conditions_util.h"

#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"

#include <fmt/format.h>

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

    // For viewful time-series collections where an expectedUUID is passed
    // in while performing a request on the view namespace, we have the additional expected behavior
    // that we will unconditionally return a CollectionUUIDMismatch error, since a view cannot have
    // a collection UUID. TODO SERVER-101784: Remove this check once 9.0 is LTS and viewful
    // time-series collections are no longer around.
    if (preConditions.getIsTimeseriesLogicalRequest() &&
        preConditions.isLegacyTimeseriesCollection()) {
        checkCollectionUUIDMismatch(opCtx, nss, nullptr, preConditions.expectedUUID());
    } else {
        checkCollectionUUIDMismatch(
            opCtx, nss, acquisition.getCollectionPtr(), preConditions.expectedUUID());
    }

    if (!preConditions.exists()) {
        uassert(10685100,
                fmt::format("Collection '{}' did not exist at the beginning of operation but has "
                            "subsequently been created as a time-series collection",
                            nss.toStringForErrorMsg()),
                !acquisition.exists() || !acquisition.getCollectionPtr()->isTimeseriesCollection());
    } else {
        if (!acquisition.exists()) {
            if (preConditions.isTimeseriesCollection() && !isRawDataOperation(opCtx)) {
                uasserted(ErrorCodes::NamespaceNotFound,
                          fmt::format("Timeseries collection '{}' got dropped during the executing "
                                      "of the operation.",
                                      (nss.isTimeseriesBucketsCollection()
                                           ? nss.getTimeseriesViewNamespace()
                                           : nss)
                                          .toStringForErrorMsg()));
            }
            return;
        }
        uassert(10685101,
                fmt::format("Collection with ns {} has been dropped and "
                            "recreated since the "
                            "beginning of the operation",
                            nss.toStringForErrorMsg()),
                preConditions.uuid() == acquisition.uuid() ||
                    (!preConditions.isTimeseriesCollection() &&
                     !acquisition.getCollectionPtr()->isTimeseriesCollection()));
    }
}

CollectionAcquisition CollectionPreConditions::acquireCollectionAndCheck(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionReq, LockMode mode) const {

    tassert(11564800,
            fmt::format("Mismatching expected UUID between CollectionPreConditions `{}` and "
                        "CollectionAcquisitionRequest `{}`",
                        acquisitionReq.expectedUUID->toString(),
                        _expectedUUID->toString()),
            !acquisitionReq.expectedUUID.has_value() || !_expectedUUID.has_value() ||
                acquisitionReq.expectedUUID == _expectedUUID);

    if (!acquisitionReq.expectedUUID) {
        acquisitionReq.expectedUUID = _expectedUUID;
    }

    auto [collAcq, _] = acquireCollectionWithBucketsLookup(opCtx, std::move(acquisitionReq), mode);

    timeseries::CollectionPreConditions::checkAcquisitionAgainstPreConditions(
        opCtx, *this, collAcq);

    return collAcq;
}
}  // namespace mongo::timeseries
