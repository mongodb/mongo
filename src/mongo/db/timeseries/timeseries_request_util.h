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
#include "mongo/db/operation_context.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/timeseries/catalog_helper.h"

namespace mongo::timeseries {

template <typename T>
concept HasGetNamespace = requires(const T& t) {
    t.getNamespace();
};

template <typename T>
concept HasGetNsString = requires(const T& t) {
    t.getNsString();
};

template <typename T>
concept HasGetNamespaceOrUUID = requires(const T& t) {
    t.getNamespaceOrUUID();
};

template <typename T>
concept HasGetIsTimeseriesNamespace = requires(const T& t) {
    t.getIsTimeseriesNamespace();
};

template <typename T>
concept IsRequestableOnTimeseries =
    HasGetNamespace<T> || HasGetNsString<T> || HasGetNamespaceOrUUID<T>;

/**
 * Returns true if the timeseries namespace for this request has been already translated
 * from the view to bucket namespace.
 */
template <typename T>
bool getIsTimeseriesNamespaceFlag(const T& request, const NamespaceStringOrUUID& nssOrUUID) {
    if constexpr (HasGetIsTimeseriesNamespace<T>) {
        uassert(
            5916400,
            "'isTimeseriesNamespace' parameter can only be set when the request is sent on "
            "system.buckets namespace",
            !request.getIsTimeseriesNamespace() ||
                (nssOrUUID.isNamespaceString() && nssOrUUID.nss().isTimeseriesBucketsCollection()));

        return request.getIsTimeseriesNamespace();
    }
    return false;
}

template <typename T>
requires IsRequestableOnTimeseries<T> mongo::NamespaceStringOrUUID getNamespaceOrUUID(
    const T& request) {
    if constexpr (HasGetNamespace<T>) {
        return request.getNamespace();
    } else if constexpr (HasGetNsString<T>) {
        return request.getNsString();
    } else {
        return request.getNamespaceOrUUID();
    }
}

/**
 * Returns true if this request is targeting a timeseries collection.
 *
 * Throws if this is a time-series collection but the timeseries options are not valid.
 */
template <typename T>
requires IsRequestableOnTimeseries<T>
bool isTimeseriesRequest(OperationContext* opCtx, const T& request) {
    if (isRawDataOperation(opCtx)) {
        return false;
    }

    auto nssOrUUID = getNamespaceOrUUID(request);
    auto wasNssAlreadyTranslated = getIsTimeseriesNamespaceFlag(request, nssOrUUID);

    if (!wasNssAlreadyTranslated &&
        (nssOrUUID.isUUID() || nssOrUUID.nss().isTimeseriesBucketsCollection())) {
        return false;
    }

    return lookupTimeseriesCollection(opCtx, nssOrUUID, wasNssAlreadyTranslated).isTimeseries;
}

/**
 * Returns a pair of (whether 'request' is made on a legacy timeseries view and the timeseries
 * system bucket collection namespace if so).
 *
 * If the 'request' is not made on a timeseries view, the second element of the pair is same as
 * the namespace of the 'request'.
 *
 * Throws if this is a time-series view request but the buckets collection is not valid.
 *
 * TODO SERVER-101784 remove this function once 9.0 becomes last LTS. By then only viewless
 * timeseries collection will exist and this function will always return false.
 */
template <typename T>
requires IsRequestableOnTimeseries<T> std::pair<bool, NamespaceString> isTimeseriesViewRequest(
    OperationContext* opCtx, const T& request) {
    auto nssOrUUID = getNamespaceOrUUID(request);
    auto isTimeseriesNamespaceFlag = getIsTimeseriesNamespaceFlag(request, nssOrUUID);
    auto lookupTimeseriesInfo =
        lookupTimeseriesCollection(opCtx, nssOrUUID, isTimeseriesNamespaceFlag);

    auto isTsViewRequest = lookupTimeseriesInfo.isTimeseries &&
        (lookupTimeseriesInfo.wasNssTranslated || isTimeseriesNamespaceFlag);

    return {isTsViewRequest, std::move(lookupTimeseriesInfo.targetNss)};
}

}  // namespace mongo::timeseries
