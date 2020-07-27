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

/**
 * Helpers to parse featureCompatibilityVersion document BSON objects into
 * ServerGlobalParams::FeatureCompatibility::Version enums and convert
 * ServerGlobalParams::FeatureCompatibility::Version enums into strings.
 */
class FeatureCompatibilityVersionParser {
public:
    static constexpr StringData kVersion44 = "4.4"_sd;
    static constexpr StringData kVersion47 = "4.7"_sd;
    static constexpr StringData kVersionDowngradingFrom47To44 = "downgrading from 4.7 to 4.4"_sd;
    static constexpr StringData kVersionUpgradingFrom44To47 = "upgrading from 4.4 to 4.7"_sd;
    static constexpr StringData kVersionUnset = "Unset"_sd;

    static constexpr StringData kParameterName = "featureCompatibilityVersion"_sd;

    static constexpr StringData kLastLTS = kVersion44;
    static constexpr StringData kLastContinuous = kVersion44;
    static constexpr StringData kLatest = kVersion47;

    static ServerGlobalParams::FeatureCompatibility::Version parseVersion(StringData versionString);

    static StringData serializeVersion(ServerGlobalParams::FeatureCompatibility::Version version);

    static Status validatePreviousVersionField(
        ServerGlobalParams::FeatureCompatibility::Version version);

    /**
     * Parses the featureCompatibilityVersion document from the server configuration collection
     * (admin.system.version), and returns the state represented by the combination of the
     * targetVersion and version.
     */
    static StatusWith<ServerGlobalParams::FeatureCompatibility::Version> parse(
        const BSONObj& featureCompatibilityVersionDoc);

    /**
     * Useful for message logging.
     */
    static StringData toString(ServerGlobalParams::FeatureCompatibility::Version version) {
        switch (version) {
            case ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault44Behavior:
                return kVersionUnset;
            case ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo44:
                return kVersion44;
            case ServerGlobalParams::FeatureCompatibility::Version::kUpgradingFrom44To47:
                return kVersionUpgradingFrom44To47;
            case ServerGlobalParams::FeatureCompatibility::Version::kDowngradingFrom47To44:
                return kVersionDowngradingFrom47To44;
            case ServerGlobalParams::FeatureCompatibility::Version::kVersion47:
                return kVersion47;
            default:
                MONGO_UNREACHABLE;
        }
    }
};

}  // namespace mongo
