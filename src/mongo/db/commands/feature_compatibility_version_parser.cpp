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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version_parser.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version_document_gen.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/version/releases.h"

namespace mongo {

using GenericFCV = multiversion::GenericFCV;

multiversion::FeatureCompatibilityVersion FeatureCompatibilityVersionParser::parseVersion(
    StringData versionString) {
    if (versionString == multiversion::toString(GenericFCV::kLastLTS)) {
        return GenericFCV::kLastLTS;
    }
    if (versionString == multiversion::toString(GenericFCV::kLastContinuous)) {
        return GenericFCV::kLastContinuous;
    }
    if (versionString == multiversion::toString(GenericFCV::kLatest)) {
        return GenericFCV::kLatest;
    }

    uasserted(4926900,
              str::stream() << "Invalid feature compatibility version value, expected '"
                            << multiversion::toString(GenericFCV::kLastLTS) << "' or '"
                            << multiversion::toString(GenericFCV::kLastContinuous) << "' or '"
                            << multiversion::toString(GenericFCV::kLatest) << ". See "
                            << feature_compatibility_version_documentation::kCompatibilityLink
                            << ".");
}

multiversion::FeatureCompatibilityVersion
FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(StringData versionString) {
    return multiversion::parseVersionForFeatureFlags(versionString);
}

StringData FeatureCompatibilityVersionParser::serializeVersion(
    multiversion::FeatureCompatibilityVersion version) {
    invariant(version == GenericFCV::kLastLTS || version == GenericFCV::kLastContinuous ||
                  version == GenericFCV::kLatest,
              "Invalid feature compatibility version value");

    return multiversion::toString(version);
}

StringData FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
    multiversion::FeatureCompatibilityVersion version) {
    if (multiversion::isStandardFCV(version)) {
        return multiversion::toString(version);
    }

    uasserted(ErrorCodes::BadValue,
              fmt::format("Invalid FCV version {} for feature flag.", version));
}

Status FeatureCompatibilityVersionParser::validatePreviousVersionField(
    multiversion::FeatureCompatibilityVersion version) {
    if (version == GenericFCV::kLatest) {
        return Status::OK();
    }
    return Status(ErrorCodes::Error(4926901),
                  "when present, 'previousVersion' field must be the latest binary version");
}

StatusWith<multiversion::FeatureCompatibilityVersion> FeatureCompatibilityVersionParser::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    try {
        auto fcvDoc = FeatureCompatibilityVersionDocument::parse(
            IDLParserContext("FeatureCompatibilityVersionParser"), featureCompatibilityVersionDoc);
        auto version = fcvDoc.getVersion();
        auto targetVersion = fcvDoc.getTargetVersion();
        auto previousVersion = fcvDoc.getPreviousVersion();

        // Downgrading FCV.
        if ((version == GenericFCV::kLastLTS || version == GenericFCV::kLastContinuous) &&
            version == targetVersion) {
            // Downgrading FCV must have a "previousVersion" field.
            if (!previousVersion) {
                return Status(ErrorCodes::Error(4926902),
                              str::stream()
                                  << "Missing field "
                                  << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                                  << " in downgrading states for " << multiversion::kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }
            if (version == GenericFCV::kLastLTS) {
                // Downgrading to last-lts.
                return GenericFCV::kDowngradingFromLatestToLastLTS;
            } else {
                return GenericFCV::kDowngradingFromLatestToLastContinuous;
            }
        }

        // Non-downgrading FCV must not have a "previousVersion" field.
        if (previousVersion) {
            return Status(ErrorCodes::Error(4926903),
                          str::stream()
                              << "Unexpected field "
                              << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                              << " in non-downgrading states for " << multiversion::kParameterName
                              << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                              << featureCompatibilityVersionDoc << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        }

        // Upgrading FCV.
        if (targetVersion) {
            // For upgrading FCV, "targetVersion" must be kLatest or kLastContinuous and "version"
            // must be kLastContinuous or kLastLTS.
            if (targetVersion == GenericFCV::kLastLTS || version == GenericFCV::kLatest) {
                return Status(ErrorCodes::Error(4926904),
                              str::stream()
                                  << "Invalid " << multiversion::kParameterName << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (version == GenericFCV::kLastLTS) {
                return targetVersion == GenericFCV::kLastContinuous
                    ? GenericFCV::kUpgradingFromLastLTSToLastContinuous
                    : GenericFCV::kUpgradingFromLastLTSToLatest;
            } else {
                uassert(5070601,
                        str::stream()
                            << "Invalid " << multiversion::kParameterName << " document in "
                            << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                            << featureCompatibilityVersionDoc << ". See "
                            << feature_compatibility_version_documentation::kCompatibilityLink
                            << ".",
                        version == GenericFCV::kLastContinuous);
                return GenericFCV::kUpgradingFromLastContinuousToLatest;
            }
        }

        // No "targetVersion" or "previousVersion" field.
        return version;
    } catch (const DBException& e) {
        auto status = e.toStatus();
        status.addContext(str::stream()
                          << "Invalid " << multiversion::kParameterName << " document in "
                          << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                          << featureCompatibilityVersionDoc << ". See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
        return status;
    }
    MONGO_UNREACHABLE
}

}  // namespace mongo
