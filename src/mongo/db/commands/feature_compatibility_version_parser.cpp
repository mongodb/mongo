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
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

constexpr StringData FeatureCompatibilityVersionParser::kVersion42;
constexpr StringData FeatureCompatibilityVersionParser::kVersion44;
constexpr StringData FeatureCompatibilityVersionParser::kVersionDowngradingTo42;
constexpr StringData FeatureCompatibilityVersionParser::kVersionUpgradingTo44;
constexpr StringData FeatureCompatibilityVersionParser::kVersionUnset;

constexpr StringData FeatureCompatibilityVersionParser::kParameterName;
constexpr StringData FeatureCompatibilityVersionParser::kVersionField;
constexpr StringData FeatureCompatibilityVersionParser::kTargetVersionField;

StatusWith<ServerGlobalParams::FeatureCompatibility::Version>
FeatureCompatibilityVersionParser::parse(const BSONObj& featureCompatibilityVersionDoc) {
    ServerGlobalParams::FeatureCompatibility::Version version =
        ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault42Behavior;
    std::string versionString;
    std::string targetVersionString;

    for (auto&& elem : featureCompatibilityVersionDoc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == kVersionField || fieldName == kTargetVersionField) {
            if (elem.type() != BSONType::String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream()
                                  << fieldName << " must be of type String, but was of type "
                                  << typeName(elem.type()) << ". Contents of " << kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (elem.String() != kVersion44 && elem.String() != kVersion42) {
                return Status(ErrorCodes::BadValue,
                              str::stream()
                                  << "Invalid value for " << fieldName << ", found "
                                  << elem.String() << ", expected '" << kVersion44 << "' or '"
                                  << kVersion42 << "'. Contents of " << kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": " << featureCompatibilityVersionDoc << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (fieldName == kVersionField) {
                versionString = elem.String();
            } else if (fieldName == kTargetVersionField) {
                targetVersionString = elem.String();
            }
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "Unrecognized field '" << fieldName << "'. Contents of "
                              << kParameterName << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                              << featureCompatibilityVersionDoc << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        }
    }

    if (versionString == kVersion42) {
        if (targetVersionString == kVersion44) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo44;
        } else if (targetVersionString == kVersion42) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo42;
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo42;
        }
    } else if (versionString == kVersion44) {
        if (targetVersionString == kVersion44 || targetVersionString == kVersion42) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                              << "Invalid state for " << kParameterName << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                              << featureCompatibilityVersionDoc << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44;
        }
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream()
                          << "Missing required field '" << kVersionField << "''. Contents of "
                          << kParameterName << " document in "
                          << NamespaceString::kServerConfigurationNamespace.toString() << ": "
                          << featureCompatibilityVersionDoc << ". See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
    }

    return version;
}

}  // namespace mongo
