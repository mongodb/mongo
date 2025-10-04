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

#include "mongo/db/feature_compatibility_version_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#include <fmt/format.h>
#include <fmt/ostream.h>

namespace mongo {

namespace {

using GenericFCV = multiversion::GenericFCV;
using FCV = multiversion::FeatureCompatibilityVersion;

template <typename T, std::size_t N>
class UniqueArray {
public:
    using const_iterator = typename std::array<T, N>::const_iterator;

    constexpr const_iterator begin() const {
        return array.cbegin();
    }

    constexpr const_iterator end() const {
        return array.cbegin() + size;
    }

    constexpr bool contains(const T& value) const {
        return std::find(this->begin(), this->end(), value) != this->end();
    }

    constexpr void insertIfUnique(const T& value) {
        if (!this->contains(value)) {
            array[size++] = value;
        }
    }

private:
    std::array<T, N> array{};
    std::size_t size = 0;
};

template <typename T, std::size_t N>
constexpr UniqueArray<T, N> makeValidVersions(const std::array<T, N>& arr) {
    UniqueArray<T, N> result;
    for (const auto& item : arr) {
        // When kLastContinuous == kLastLTS some values can be aliases,
        // and others like kUpgradingFromLastLTSToLastContinuous become invalid.
        if (item != FCV::kInvalid) {
            result.insertIfUnique(item);
        }
    }
    return result;
}

constexpr UniqueArray validOfcvVersions =
    makeValidVersions(std::array{GenericFCV::kLatest,
                                 GenericFCV::kLastContinuous,
                                 GenericFCV::kLastLTS,
                                 GenericFCV::kUpgradingFromLastLTSToLatest,
                                 GenericFCV::kDowngradingFromLatestToLastLTS,
                                 GenericFCV::kUpgradingFromLastContinuousToLatest,
                                 GenericFCV::kDowngradingFromLatestToLastContinuous,
                                 GenericFCV::kUpgradingFromLastLTSToLastContinuous});

constexpr UniqueArray validFcvVersions = makeValidVersions(
    std::array{GenericFCV::kLatest, GenericFCV::kLastContinuous, GenericFCV::kLastLTS});

/*
 * Helper used to parse the `versionString` against the `validVersions`
 */
template <std::size_t N>
StatusWith<FCV> parseVersion(const UniqueArray<FCV, N>& validVersions, StringData versionString) {
    try {
        const auto version = multiversion::parseVersion(versionString);
        if (validVersions.contains(version)) {
            return version;
        }
    } catch (const ExceptionFor<ErrorCodes::BadValue>&) {
    }

    // Create a comma-separated list of valid versions
    std::ostringstream validVersionsStream;
    StringData sep;
    for (const auto& ver : validVersions) {
        validVersionsStream << sep << "'" << toString(ver) << "'";
        sep = ", ";
    }

    return Status{ErrorCodes::BadValue,
                  str::stream() << "Invalid feature compatibility version value '" << versionString
                                << "'. Expected one of the following versions: '"
                                << validVersionsStream.str()};
}

}  // namespace

FCV FeatureCompatibilityVersionParser::parseVersionForOfcvString(StringData versionString) {
    return uassertStatusOK(parseVersion(validOfcvVersions, versionString));
}

FCV FeatureCompatibilityVersionParser::parseVersionForFcvString(StringData versionString) {
    const auto version = parseVersion(validFcvVersions, versionString);
    if (version.isOK()) {
        return version.getValue();
    }
    uasserted(4926900,
              str::stream() << version.getStatus().reason() << ". See "
                            << feature_compatibility_version_documentation::compatibilityLink()
                            << ".");
}

FCV FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(StringData versionString) {
    return multiversion::parseVersionForFeatureFlags(versionString);
}

StringData FeatureCompatibilityVersionParser::serializeVersionForOfcvString(FCV version) {
    invariant(validOfcvVersions.contains(version),
              str::stream() << "Invalid feature compatibility version value: "
                            << multiversion::toString(version));
    return multiversion::toString(version);
}

StringData FeatureCompatibilityVersionParser::serializeVersionForFcvString(FCV version) {
    invariant(validFcvVersions.contains(version),
              str::stream() << "Invalid feature compatibility version value: "
                            << multiversion::toString(version));
    return multiversion::toString(version);
}

StringData FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(FCV version) {
    if (multiversion::isStandardFCV(version)) {
        return multiversion::toString(version);
    }
    uasserted(ErrorCodes::BadValue,
              fmt::format("Invalid FCV version {} for feature flag.", fmt::underlying(version)));
}

Status FeatureCompatibilityVersionParser::validatePreviousVersionField(FCV version) {
    if (version == GenericFCV::kLatest) {
        return Status::OK();
    }
    return Status(ErrorCodes::Error(4926901),
                  "when present, 'previousVersion' field must be the latest binary version");
}

StatusWith<FCV> FeatureCompatibilityVersionParser::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    try {
        auto fcvDoc = FeatureCompatibilityVersionDocument::parse(featureCompatibilityVersionDoc);
        auto version = fcvDoc.getVersion();
        auto targetVersion = fcvDoc.getTargetVersion();
        auto previousVersion = fcvDoc.getPreviousVersion();

        // Downgrading FCV.
        if ((version == GenericFCV::kLastLTS || version == GenericFCV::kLastContinuous) &&
            version == targetVersion) {
            // Downgrading FCV must have a "previousVersion" field.
            if (!previousVersion) {
                return Status(
                    ErrorCodes::Error(4926902),
                    str::stream()
                        << "Missing field "
                        << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                        << " in downgrading states for " << multiversion::kParameterName
                        << " document in "
                        << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg()
                        << ": " << featureCompatibilityVersionDoc << ". See "
                        << feature_compatibility_version_documentation::compatibilityLink() << ".");
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
            return Status(
                ErrorCodes::Error(4926903),
                str::stream()
                    << "Unexpected field "
                    << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                    << " in non-downgrading states for " << multiversion::kParameterName
                    << " document in "
                    << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg() << ": "
                    << featureCompatibilityVersionDoc << ". See "
                    << feature_compatibility_version_documentation::compatibilityLink() << ".");
        }

        // Upgrading FCV.
        if (targetVersion) {
            // For upgrading FCV, "targetVersion" must be kLatest or kLastContinuous and "version"
            // must be kLastContinuous or kLastLTS.
            if (targetVersion == GenericFCV::kLastLTS || version == GenericFCV::kLatest) {
                return Status(
                    ErrorCodes::Error(4926904),
                    str::stream()
                        << "Invalid " << multiversion::kParameterName << " document in "
                        << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg()
                        << ": " << featureCompatibilityVersionDoc << ". See "
                        << feature_compatibility_version_documentation::compatibilityLink() << ".");
            }

            if (version == GenericFCV::kLastLTS) {
                return targetVersion == GenericFCV::kLastContinuous
                    ? GenericFCV::kUpgradingFromLastLTSToLastContinuous
                    : GenericFCV::kUpgradingFromLastLTSToLatest;
            } else {
                uassert(5070601,
                        str::stream()
                            << "Invalid " << multiversion::kParameterName << " document in "
                            << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg()
                            << ": " << featureCompatibilityVersionDoc << ". See "
                            << feature_compatibility_version_documentation::compatibilityLink()
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
                          << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg()
                          << ": " << featureCompatibilityVersionDoc << ". See "
                          << feature_compatibility_version_documentation::compatibilityLink()
                          << ".");
        return status;
    }
    MONGO_UNREACHABLE
}

}  // namespace mongo
