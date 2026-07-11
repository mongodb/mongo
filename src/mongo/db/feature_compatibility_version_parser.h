// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <string_view>

namespace mongo {
/**
 * Helpers to parse featureCompatibilityVersion document BSON objects into
 * multiversion::FeatureCompatibilityVersion enums.
 *
 * Also, act as helpers to serialize and deserialize "fcv_string" and "ofcv_string" idl types.
 */
struct [[MONGO_MOD_PUBLIC]] FeatureCompatibilityVersionParser {
    using GenericFCV = multiversion::GenericFCV;
    using FCV = multiversion::FeatureCompatibilityVersion;

    /**
     * Deserializer for "ofcv_string" idl type. Throws an ErrorCodes::BadValue exception when
     * the FCV value provided is not equal to one of {LastLTS, LastContinuous, Latest,
     * UpgradingFromLastLTSToLates, DowngradingFromLatestToLastLTS,
     * UpgradingFromLastContinuousToLatest, DowngradingFromLatestToLastContinuous,
     * UpgradingFromLastLTSToLastContinuous}
     */
    static FCV parseVersionForOfcvString(std::string_view versionString);

    /**
     * Deserializer for "fcv_string" idl type. Throws an exception with the error code 4926900,
     * when the FCV value provided is not equal to one of {LastLTS, LastContinuous, Latest}
     */
    static FCV parseVersionForFcvString(std::string_view versionString);

    // Used to parse FCV values for feature flags. It is acceptable to have feature flag versions
    // that are not one of { lastLTS, lastContinuous, latest } while the server code is
    // transitioning to the next LTS release. This is to avoid having the upgrade of FCV constants
    // be blocked on old code removal.
    static FCV parseVersionForFeatureFlags(std::string_view versionString);

    /**
     * Serializer for "ofcv_string" idl type. Asserts through an invariant that
     * the FCV value provided is equal to one of {LastLTS, LastContinuous, Latest,
     * Upgrading*, Downgrading*}
     */
    static std::string_view serializeVersionForOfcvString(FCV version);

    /**
     * Serializer for "fcv_string" idl type. Asserts through an invariant that
     * the FCV value provided is equal to one of {LastLTS, LastContinuous, Latest}
     */
    static std::string_view serializeVersionForFcvString(FCV version);

    static std::string_view serializeVersionForFeatureFlags(FCV version);

    static Status validatePreviousVersionField(FCV version);

    /**
     * Parses the featureCompatibilityVersion document from the server configuration collection
     * (admin.system.version), and returns the state represented by the combination of the
     * targetVersion and version.
     */
    static StatusWith<FCV> parse(const BSONObj& featureCompatibilityVersionDoc);
};

}  // namespace mongo
