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
#include "mongo/db/timeseries/collection_pre_conditions_util.h"
#include "mongo/db/timeseries/timeseries_options.h"

namespace mongo::timeseries {

template <typename T>
concept HasGetNs = requires(const T& t) { t.getNs(); };

template <typename T>
concept HasGetNamespace = requires(const T& t) { t.getNamespace(); };

template <typename T>
concept HasGetNsString = requires(const T& t) { t.getNsString(); };

template <typename T>
concept HasGetNamespaceOrUUID = requires(const T& t) { t.getNamespaceOrUUID(); };

template <typename T>
concept HasGetIsTimeseriesNamespace = requires(const T& t) { t.getIsTimeseriesNamespace(); };

template <typename T>
concept IsRequestableOnTimeseries =
    HasGetNs<T> || HasGetNamespace<T> || HasGetNsString<T> || HasGetNamespaceOrUUID<T>;


/**
 * Returns true if the given request was sent with `isTimeseriesNamespace` flag to true.
 *
 * This flag is specified by the sender when had already translated the namespace to the underlying
 * timeseries system.buckets collection.
 *
 * This is only used for legacy timeseries collection (view + buckets).
 *
 * TODO SERVER-101784 remove this function once 9.0 becomes last LTS. By then only viewless
 * timeseries collection will exist and this function will always return false.
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

/**
 * Extracts the namespace or UUID for the given request.
 */
template <typename T>
requires IsRequestableOnTimeseries<T>
mongo::NamespaceStringOrUUID getNamespaceOrUUID(const T& request) {
    if constexpr (HasGetNamespace<T>) {
        return request.getNamespace();
    } else if constexpr (HasGetNsString<T>) {
        return request.getNsString();
    } else if constexpr (HasGetNs<T>) {
        return request.getNs();
    } else {
        return request.getNamespaceOrUUID();
    }
}

/**
 * Returns a boolean indicating if this request should serve raw data without performing any logical
 * transformation.
 *
 * For timeseries collection this is the case when either the request was sent explicitily with
 * `rawData` parameter.
 *
 * For legacy timeseries collection (view + buckets) all operation targeting directly the
 * system.buckets collection are also considered raw.
 */
template <typename T>
requires IsRequestableOnTimeseries<T>
bool isRawDataRequest(OperationContext* opCtx, const T& request) {
    if (isRawDataOperation(opCtx)) {
        // Explicitily requested raw data
        return true;
    }

    auto nssOrUUID = getNamespaceOrUUID(request);
    auto wasNssAlreadyTranslated = getIsTimeseriesNamespaceFlag(request, nssOrUUID);

    if (wasNssAlreadyTranslated) {
        // The request originally was targeting the view timeseries namespace and the namespace have
        // been translated to system.buckets.
        // Since rawData was not passed we consider this a logical request.
        return false;
    }

    // At this point we know that:
    //  - rawData is not set
    //  - The namespace was not translated to system.buckets

    if (nssOrUUID.isUUID()) {
        // The request is targeting a specific collection UUID
        // In this case we always consider the request rawData
        //
        // TODO SERVER-102758 implement logical request through UUID targeting
        return true;
    } else if (nssOrUUID.nss().isTimeseriesBucketsCollection()) {
        // The request came directly on the system.buckets namespace.
        return true;
    }

    return false;
}

/**
 * Returns true if this request is targeting a timeseries collection.
 *
 * Throws if this is a time-series collection but the timeseries options are not valid.
 */
template <typename T>
requires IsRequestableOnTimeseries<T>
bool isTimeseriesRequest(OperationContext* opCtx, const T& request) {
    if (isRawDataRequest(opCtx, request)) {
        return false;
    }

    auto nssOrUUID = getNamespaceOrUUID(request);
    auto wasNssAlreadyTranslated = getIsTimeseriesNamespaceFlag(request, nssOrUUID);

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
requires IsRequestableOnTimeseries<T>
std::pair<bool, NamespaceString> isTimeseriesViewRequest(OperationContext* opCtx,
                                                         const T& request) {
    auto nssOrUUID = getNamespaceOrUUID(request);
    auto isTimeseriesNamespaceFlag = getIsTimeseriesNamespaceFlag(request, nssOrUUID);
    auto lookupTimeseriesInfo =
        lookupTimeseriesCollection(opCtx, nssOrUUID, isTimeseriesNamespaceFlag);

    auto isTsViewRequest = lookupTimeseriesInfo.isTimeseries &&
        (lookupTimeseriesInfo.wasNssTranslated || isTimeseriesNamespaceFlag);

    return {isTsViewRequest, std::move(lookupTimeseriesInfo.targetNss)};
}

/**
 * Returns a pair where the first element is the CollectionPreConditions object for this class, and
 * the second is a bool indicating whether the request being performed is a request to perform a
 * logical time-series operation.
 *
 * There are two broad ways of interacting with a time-series collection - directly on the
 * compressed buckets data, or logically on the measurements. The former are generally
 * referred to as rawData operations and the latter can be considered logical time-series
 * operations. Logical time-series operations have the concept of bucketing measurements
 * - i.e, inserts will try to place measurements with the same metaField into the same bucket,
 * updates will know to target a measurements within a bucket and update the rest of the bucket
 * accordingly, etc. RawData operations treat the bucket document as a normal document.
 */
template <typename T>
requires IsRequestableOnTimeseries<T>
std::pair<CollectionPreConditions, bool> getCollectionPreConditionsAndIsTimeseriesLogicalRequest(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const T& request,
    boost::optional<UUID> expectedUUID) {

    const bool isRawDataReq = isRawDataRequest(opCtx, request);
    auto preConditions =
        timeseries::CollectionPreConditions::getCollectionPreConditions(opCtx, nss, expectedUUID);

    const auto isTimeseriesLogicalRequest = preConditions.isTimeseriesCollection() && !isRawDataReq;
    preConditions.setIsTimeseriesLogicalRequest(isTimeseriesLogicalRequest);

    return {preConditions, isTimeseriesLogicalRequest};
}

}  // namespace mongo::timeseries
