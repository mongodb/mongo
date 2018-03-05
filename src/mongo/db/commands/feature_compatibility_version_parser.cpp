/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/commands/feature_compatibility_version_parser.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

constexpr StringData FeatureCompatibilityVersionParser::kVersion36;
constexpr StringData FeatureCompatibilityVersionParser::kVersion40;
constexpr StringData FeatureCompatibilityVersionParser::kVersionDowngradingTo36;
constexpr StringData FeatureCompatibilityVersionParser::kVersionUpgradingTo40;
constexpr StringData FeatureCompatibilityVersionParser::kVersionUnset;

constexpr StringData FeatureCompatibilityVersionParser::kParameterName;
constexpr StringData FeatureCompatibilityVersionParser::kVersionField;
constexpr StringData FeatureCompatibilityVersionParser::kTargetVersionField;

StatusWith<ServerGlobalParams::FeatureCompatibility::Version>
FeatureCompatibilityVersionParser::parse(const BSONObj& featureCompatibilityVersionDoc) {
    ServerGlobalParams::FeatureCompatibility::Version version =
        ServerGlobalParams::FeatureCompatibility::Version::kUnsetDefault36Behavior;
    std::string versionString;
    std::string targetVersionString;

    for (auto&& elem : featureCompatibilityVersionDoc) {
        auto fieldName = elem.fieldNameStringData();
        if (fieldName == "_id") {
            continue;
        } else if (fieldName == kVersionField || fieldName == kTargetVersionField) {
            if (elem.type() != BSONType::String) {
                return Status(
                    ErrorCodes::TypeMismatch,
                    str::stream() << fieldName << " must be of type String, but was of type "
                                  << typeName(elem.type())
                                  << ". Contents of "
                                  << kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": "
                                  << featureCompatibilityVersionDoc
                                  << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (elem.String() != kVersion40 && elem.String() != kVersion36) {
                return Status(
                    ErrorCodes::BadValue,
                    str::stream() << "Invalid value for " << fieldName << ", found "
                                  << elem.String()
                                  << ", expected '"
                                  << kVersion40
                                  << "' or '"
                                  << kVersion36
                                  << "'. Contents of "
                                  << kParameterName
                                  << " document in "
                                  << NamespaceString::kServerConfigurationNamespace.toString()
                                  << ": "
                                  << featureCompatibilityVersionDoc
                                  << ". See "
                                  << feature_compatibility_version_documentation::kCompatibilityLink
                                  << ".");
            }

            if (fieldName == kVersionField) {
                versionString = elem.String();
            } else if (fieldName == kTargetVersionField) {
                targetVersionString = elem.String();
            }
        } else {
            return Status(
                ErrorCodes::BadValue,
                str::stream() << "Unrecognized field '" << fieldName << "'. Contents of "
                              << kParameterName
                              << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString()
                              << ": "
                              << featureCompatibilityVersionDoc
                              << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        }
    }

    if (versionString == kVersion36) {
        if (targetVersionString == kVersion40) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kUpgradingTo40;
        } else if (targetVersionString == kVersion36) {
            version = ServerGlobalParams::FeatureCompatibility::Version::kDowngradingTo36;
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyDowngradedTo36;
        }
    } else if (versionString == kVersion40) {
        if (targetVersionString == kVersion40 || targetVersionString == kVersion36) {
            return Status(
                ErrorCodes::BadValue,
                str::stream() << "Invalid state for " << kParameterName << " document in "
                              << NamespaceString::kServerConfigurationNamespace.toString()
                              << ": "
                              << featureCompatibilityVersionDoc
                              << ". See "
                              << feature_compatibility_version_documentation::kCompatibilityLink
                              << ".");
        } else {
            version = ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40;
        }
    } else {
        return Status(
            ErrorCodes::BadValue,
            str::stream() << "Missing required field '" << kVersionField << "''. Contents of "
                          << kParameterName
                          << " document in "
                          << NamespaceString::kServerConfigurationNamespace.toString()
                          << ": "
                          << featureCompatibilityVersionDoc
                          << ". See "
                          << feature_compatibility_version_documentation::kCompatibilityLink
                          << ".");
    }

    return version;
}

}  // namespace mongo
