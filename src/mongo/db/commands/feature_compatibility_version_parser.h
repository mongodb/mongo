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

#include "mongo/db/server_options.h"

namespace mongo {
using FeatureCompatibilityParams = ServerGlobalParams::FeatureCompatibility;

/**
 * Helpers to parse featureCompatibilityVersion document BSON objects into
 * ServerGlobalParams::FeatureCompatibility::Version enums and convert
 * ServerGlobalParams::FeatureCompatibility::Version enums into strings.
 */
class FeatureCompatibilityVersionParser {
public:
    static constexpr StringData kVersion44 = "4.4"_sd;  // Remove once old feature flags are deleted
    static constexpr StringData kVersion47 = "4.7"_sd;  // Remove once old feature flags are deleted
    static constexpr StringData kVersion48 = "4.8"_sd;  // Remove once old feature flags are deleted
    static constexpr StringData kVersion49 = "4.9"_sd;  // Remove once old feature flags are deleted
    static constexpr StringData kVersion50 = "5.0"_sd;
    static constexpr StringData kVersion51 = "5.1"_sd;
    static constexpr StringData kVersionDowngradingFrom51To50 = "downgrading from 5.1 to 5.0"_sd;
    static constexpr StringData kVersionUpgradingFrom50To51 = "upgrading from 5.0 to 5.1"_sd;
    static constexpr StringData kVersionUnset = "Unset"_sd;

    static constexpr StringData kParameterName = "featureCompatibilityVersion"_sd;

    static constexpr StringData kLastLTS = kVersion50;
    static constexpr StringData kLastContinuous = kVersion50;
    static constexpr StringData kLatest = kVersion51;
    static constexpr StringData kUpgradingFromLastLTSToLatest = kVersionUpgradingFrom50To51;
    static constexpr StringData kUpgradingFromLastContinuousToLatest = kVersionUpgradingFrom50To51;
    // kVersionUpgradingFromLastLTSToLastContinuous should assigned kVersionUnset when kLastLTS and
    // kLastContinuous are equal.
    static constexpr StringData kVersionUpgradingFromLastLTSToLastContinuous = kVersionUnset;
    static constexpr StringData kDowngradingFromLatestToLastLTS = kVersionDowngradingFrom51To50;
    static constexpr StringData kDowngradingFromLatestToLastContinuous =
        kVersionDowngradingFrom51To50;

    // Used to verify that FCV values in 'admin.system.version' are valid and equal to one of
    // { lastLTS, lastContinuous, latest }.
    static FeatureCompatibilityParams::Version parseVersion(StringData versionString);

    // Used to parse FCV values for feature flags. It is acceptable to have feature flag versions
    // that are not one of { lastLTS, lastContinuous, latest } while the server code is
    // transitioning to the next LTS release. This is to avoid having the upgrade of FCV constants
    // be blocked on old code removal.
    static FeatureCompatibilityParams::Version parseVersionForFeatureFlags(
        StringData versionString);

    static StringData serializeVersion(FeatureCompatibilityParams::Version version);

    static StringData serializeVersionForFeatureFlags(FeatureCompatibilityParams::Version version);

    static Status validatePreviousVersionField(FeatureCompatibilityParams::Version version);

    /**
     * Parses the featureCompatibilityVersion document from the server configuration collection
     * (admin.system.version), and returns the state represented by the combination of the
     * targetVersion and version.
     */
    static StatusWith<FeatureCompatibilityParams::Version> parse(
        const BSONObj& featureCompatibilityVersionDoc);

    /**
     * Useful for message logging.
     */
    static StringData toString(FeatureCompatibilityParams::Version version) {
        if (version == FeatureCompatibilityParams::Version::kUnsetDefault50Behavior) {
            return kVersionUnset;
        } else if (version == FeatureCompatibilityParams::kLastLTS) {
            return kLastLTS;
        } else if (version == FeatureCompatibilityParams::kDowngradingFromLatestToLastLTS) {
            return kDowngradingFromLatestToLastLTS;
        } else if (version == FeatureCompatibilityParams::kUpgradingFromLastLTSToLastContinuous) {
            // kUpgradingFromLastLTSToLastContinuous is only a valid FCV state when last-continuous
            // and last-lts are not equal. Otherwise, it is set to kInvalid.
            invariant(version != FeatureCompatibilityParams::Version::kInvalid);
            return kVersionUpgradingFromLastLTSToLastContinuous;
        } else if (version == FeatureCompatibilityParams::kUpgradingFromLastLTSToLatest) {
            return kUpgradingFromLastLTSToLatest;
        } else if (version == FeatureCompatibilityParams::kLastContinuous) {
            return kLastContinuous;
        } else if (version == FeatureCompatibilityParams::kDowngradingFromLatestToLastContinuous) {
            return kDowngradingFromLatestToLastContinuous;
        } else if (version == FeatureCompatibilityParams::kUpgradingFromLastContinuousToLatest) {
            return kUpgradingFromLastContinuousToLatest;
        } else if (version == FeatureCompatibilityParams::kLatest) {
            return kLatest;
        }
        MONGO_UNREACHABLE;
    }
};

}  // namespace mongo
