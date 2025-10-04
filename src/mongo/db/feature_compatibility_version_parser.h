/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/util/version/releases.h"

namespace mongo {
/**
 * Helpers to parse featureCompatibilityVersion document BSON objects into
 * multiversion::FeatureCompatibilityVersion enums.
 *
 * Also, act as helpers to serialize and deserialize "fcv_string" and "ofcv_string" idl types.
 */
struct FeatureCompatibilityVersionParser {
    using GenericFCV = multiversion::GenericFCV;
    using FCV = multiversion::FeatureCompatibilityVersion;

    /**
     * Deserializer for "ofcv_string" idl type. Throws an ErrorCodes::BadValue exception when
     * the FCV value provided is not equal to one of {LastLTS, LastContinuous, Latest,
     * UpgradingFromLastLTSToLates, DowngradingFromLatestToLastLTS,
     * UpgradingFromLastContinuousToLatest, DowngradingFromLatestToLastContinuous,
     * UpgradingFromLastLTSToLastContinuous}
     */
    static FCV parseVersionForOfcvString(StringData versionString);

    /**
     * Deserializer for "fcv_string" idl type. Throws an exception with the error code 4926900,
     * when the FCV value provided is not equal to one of {LastLTS, LastContinuous, Latest}
     */
    static FCV parseVersionForFcvString(StringData versionString);

    // Used to parse FCV values for feature flags. It is acceptable to have feature flag versions
    // that are not one of { lastLTS, lastContinuous, latest } while the server code is
    // transitioning to the next LTS release. This is to avoid having the upgrade of FCV constants
    // be blocked on old code removal.
    static FCV parseVersionForFeatureFlags(StringData versionString);

    /**
     * Serializer for "ofcv_string" idl type. Asserts through an invariant that
     * the FCV value provided is equal to one of {LastLTS, LastContinuous, Latest,
     * Upgrading*, Downgrading*}
     */
    static StringData serializeVersionForOfcvString(FCV version);

    /**
     * Serializer for "fcv_string" idl type. Asserts through an invariant that
     * the FCV value provided is equal to one of {LastLTS, LastContinuous, Latest}
     */
    static StringData serializeVersionForFcvString(FCV version);

    static StringData serializeVersionForFeatureFlags(FCV version);

    static Status validatePreviousVersionField(FCV version);

    /**
     * Parses the featureCompatibilityVersion document from the server configuration collection
     * (admin.system.version), and returns the state represented by the combination of the
     * targetVersion and version.
     */
    static StatusWith<FCV> parse(const BSONObj& featureCompatibilityVersionDoc);
};

}  // namespace mongo
