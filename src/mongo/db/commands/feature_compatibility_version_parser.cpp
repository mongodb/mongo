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

namespace mongo {

constexpr StringData FeatureCompatibilityVersionParser::kParameterName;

constexpr StringData FeatureCompatibilityVersionParser::kLastLTS;
constexpr StringData FeatureCompatibilityVersionParser::kLastContinuous;
constexpr StringData FeatureCompatibilityVersionParser::kLatest;

ServerGlobalParams::FeatureCompatibility::Version FeatureCompatibilityVersionParser::parseVersion(
    StringData versionString) {
    if (versionString == kLastLTS) {
        return ServerGlobalParams::FeatureCompatibility::kLastLTS;
    }
    if (versionString == kLastContinuous) {
        return ServerGlobalParams::FeatureCompatibility::kLastContinuous;
    }
    if (versionString == kLatest) {
        return ServerGlobalParams::FeatureCompatibility::kLatest;
    }
    uasserted(4926900,
              str::stream() << "Invalid value for " << kParameterName << "document in "
                            << NamespaceString::kServerConfigurationNamespace.toString()
                            << ", found " << versionString << ", expected '" << kLastLTS << "' or '"
                            << kLastContinuous << "' or '" << kLatest << ". See "
                            << feature_compatibility_version_documentation::kCompatibilityLink
                            << ".");
}

StringData FeatureCompatibilityVersionParser::serializeVersion(
    ServerGlobalParams::FeatureCompatibility::Version version) {
    if (version == ServerGlobalParams::FeatureCompatibility::kLastLTS) {
        return kLastLTS;
    }
    if (version == ServerGlobalParams::FeatureCompatibility::kLastContinuous) {
        return kLastContinuous;
    }
    if (version == ServerGlobalParams::FeatureCompatibility::kLatest) {
        return kLatest;
    }
    // It is a bug if we hit here.
    invariant(false, "Invalid version value for featureCompatibilityVersion documents");
    MONGO_UNREACHABLE
}

Status FeatureCompatibilityVersionParser::validatePreviousVersionField(
    ServerGlobalParams::FeatureCompatibility::Version version) {
    if (version == ServerGlobalParams::FeatureCompatibility::kLatest) {
        return Status::OK();
    }
    return Status(ErrorCodes::Error(4926901),
                  "when present, 'previousVersion' field must be the latest binary version");
}

StatusWith<ServerGlobalParams::FeatureCompatibility::Version>
FeatureCompatibilityVersionParser::parse(const BSONObj& featureCompatibilityVersionDoc) {
    try {
        auto fcvDoc = FeatureCompatibilityVersionDocument::parse(
            IDLParserErrorContext("FeatureCompatibilityVersionParser"),
            featureCompatibilityVersionDoc);
        auto version = fcvDoc.getVersion();
        auto targetVersion = fcvDoc.getTargetVersion();
        auto previousVersion = fcvDoc.getPreviousVersion();

        // Downgrading FCV.
        if ((version == ServerGlobalParams::FeatureCompatibility::kLastLTS ||
             version == ServerGlobalParams::FeatureCompatibility::kLastContinuous) &&
            version == targetVersion) {
            // Downgrading FCV must have a "previousVersion" field.
            if (!previousVersion) {
                return Status(ErrorCodes::Error(4926902),
                              str::stream()
                                  << "Missing field "
                                  << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                                  << " in downgrading states for " << kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }
            if (version == ServerGlobalParams::FeatureCompatibility::kLastLTS) {
                // Downgrading to last-lts.
                return ServerGlobalParams::FeatureCompatibility::kDowngradingFromLatestToLastLTS;
            } else {
                return ServerGlobalParams::FeatureCompatibility::
                    kDowngradingFromLatestToLastContinuous;
            }
        }

        // Non-downgrading FCV must not have a "previousVersion" field.
        if (previousVersion) {
            return Status(ErrorCodes::Error(4926903),
                          str::stream()
                              << "Unexpected field "
                              << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                              << " in non-downgrading states for " << kParameterName
                              << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                              << featureCompatibilityVersionDoc << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        }

        // Upgrading FCV.
        if (targetVersion) {
            // For upgrading FCV, "targetVersion" must be kLatest and "version" must be
            // kLastContinuous or kLastLTS.
            if (targetVersion != ServerGlobalParams::FeatureCompatibility::kLatest ||
                version == ServerGlobalParams::FeatureCompatibility::kLatest) {
                return Status(ErrorCodes::Error(4926904),
                              str::stream()
                                  << "Invalid " << kParameterName << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (version == ServerGlobalParams::FeatureCompatibility::kLastLTS) {
                return ServerGlobalParams::FeatureCompatibility::kUpgradingFromLastLTSToLatest;
            } else {
                invariant(version == ServerGlobalParams::FeatureCompatibility::kLastContinuous);
                return ServerGlobalParams::FeatureCompatibility::
                    kUpgradingFromLastContinuousToLatest;
            }
        }

        // No "targetVersion" or "previousVersion" field.
        return version;
    } catch (const DBException& e) {
        auto status = e.toStatus();
        status.addContext(str::stream()
                          << "Invalid " << kParameterName << " document in "
                          << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                          << featureCompatibilityVersionDoc << ". See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
        return status;
    }
    MONGO_UNREACHABLE
}

}  // namespace mongo
