// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/feature_compatibility_version_parser.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/feature_compatibility_version_documentation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/version_context_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/version/releases.h"

#include <string_view>
#include <tuple>

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

constexpr UniqueArray validOfcvVersions = makeValidVersions(
    std::array{GenericFCV::kLatest,
               GenericFCV::kLastContinuous,
               GenericFCV::kLastLTS,
               GenericFCV::kUpgradingFromLastLTSToLatest,
               GenericFCV::kDowngradingFromLatestToLastLTS,
               GenericFCV::kUpgradingFromLastContinuousToLatest,
               GenericFCV::kDowngradingFromLatestToLastContinuous,
               GenericFCV::kUpgradingFromLastLTSToLastContinuous,
               multiversion::FeatureCompatibilityVersion::kUnsetDefaultLastLTSBehavior});

constexpr UniqueArray validFcvVersions = makeValidVersions(
    std::array{GenericFCV::kLatest, GenericFCV::kLastContinuous, GenericFCV::kLastLTS});

/*
 * Helper used to parse the `versionString` against the `validVersions`
 */
template <std::size_t N>
StatusWith<FCV> parseVersion(const UniqueArray<FCV, N>& validVersions,
                             std::string_view versionString) {
    try {
        const auto version = multiversion::parseVersion(versionString);
        if (validVersions.contains(version)) {
            return version;
        }
    } catch (const ExceptionFor<ErrorCodes::BadValue>&) {
    }

    // Create a comma-separated list of valid versions
    std::ostringstream validVersionsStream;
    std::string_view sep;
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

FCV FeatureCompatibilityVersionParser::parseVersionForOfcvString(std::string_view versionString) {
    return uassertStatusOK(parseVersion(validOfcvVersions, versionString));
}

FCV FeatureCompatibilityVersionParser::parseVersionForFcvString(std::string_view versionString) {
    const auto version = parseVersion(validFcvVersions, versionString);
    if (version.isOK()) {
        return version.getValue();
    }
    uasserted(4926900,
              str::stream() << version.getStatus().reason() << ". See "
                            << feature_compatibility_version_documentation::compatibilityLink()
                            << ".");
}

FCV FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(std::string_view versionString) {
    return multiversion::parseVersionForFeatureFlags(versionString);
}

std::string_view FeatureCompatibilityVersionParser::serializeVersionForOfcvString(FCV version) {
    invariant(validOfcvVersions.contains(version),
              str::stream() << "Invalid feature compatibility version value: "
                            << multiversion::toString(version));
    return multiversion::toString(version);
}

std::string_view FeatureCompatibilityVersionParser::serializeVersionForFcvString(FCV version) {
    invariant(validFcvVersions.contains(version),
              str::stream() << "Invalid feature compatibility version value: "
                            << multiversion::toString(version));
    return multiversion::toString(version);
}

std::string_view FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(FCV version) {
    if (multiversion::isStandardFCV(version)) {
        return multiversion::toString(version);
    }
    uasserted(ErrorCodes::BadValue,
              fmt::format("Invalid FCV version {} for feature flag.", fmt::underlying(version)));
}

Status FeatureCompatibilityVersionParser::validatePreviousVersionField(FCV version) {
    if (version == GenericFCV::kLatest || version == GenericFCV::kLastLTS ||
        version == GenericFCV::kLastContinuous) {
        return Status::OK();
    }
    return Status(ErrorCodes::Error(11948401),
                  "when present, 'previousVersion' field must be a standard FCV version");
}

StatusWith<FCV> FeatureCompatibilityVersionParser::parse(
    const BSONObj& featureCompatibilityVersionDoc) {
    try {
        auto fcvDoc = FeatureCompatibilityVersionDocument::parse(featureCompatibilityVersionDoc);
        auto version = fcvDoc.getVersion();
        auto targetVersion = fcvDoc.getTargetVersion();
        auto previousVersion = fcvDoc.getPreviousVersion();
        auto phase = fcvDoc.getPhase();

        // The 'phase' field is only ever written under Symmetric FCV (see
        // feature_compatibility_version.cpp's updateFeatureCompatibilityVersionDocument). Seeing it
        // in the doc while the flag is disabled means the on-disk state was written by a different
        // binary and this binary cannot safely interpret it — fail rather than silently mis-project
        // the in-memory FCV.
        if (phase.has_value() && !gFeatureFlagSymmetricFCV.isEnabled()) {
            return Status(ErrorCodes::Error(11948500),
                          str::stream()
                              << "FCV document has 'phase' field but Symmetric FCV is disabled: "
                              << featureCompatibilityVersionDoc);
        }

        // Under Symmetric FCV, kEnableTargetFeatures and kCommitAddedFeatures are the phases where
        // target features are already active. However, only project the in-memory FCV to the
        // target after confirming the document has a valid transitional shape; otherwise fall
        // through to the existing validation below so malformed documents still fail to parse.
        if (phase.has_value() && *phase >= SetFCVPhaseEnum::kEnableTargetFeatures) {

            if (!targetVersion.has_value()) {
                return Status(ErrorCodes::Error(11948501),
                              str::stream() << "Missing 'targetVersion' field in FCV document with "
                                            << "'phase' >= kEnableTargetFeatures: "
                                            << featureCompatibilityVersionDoc);
            }
            if (!previousVersion.has_value()) {
                return Status(ErrorCodes::Error(11948502),
                              str::stream()
                                  << "Missing 'previousVersion' field in FCV document with "
                                  << "'phase' >= kEnableTargetFeatures: "
                                  << featureCompatibilityVersionDoc);
            }

            auto upgradingOrDowngradingShapes = {
                // version, targetVersion, previousVersion (upgrading)
                std::tuple{GenericFCV::kLatest, GenericFCV::kLatest, GenericFCV::kLastLTS},
                std::tuple{GenericFCV::kLatest, GenericFCV::kLatest, GenericFCV::kLastContinuous},
                std::tuple{
                    GenericFCV::kLastContinuous, GenericFCV::kLastContinuous, GenericFCV::kLastLTS},
                // (downgrading)
                std::tuple{GenericFCV::kLastLTS, GenericFCV::kLastLTS, GenericFCV::kLatest},
                std::tuple{
                    GenericFCV::kLastContinuous, GenericFCV::kLastContinuous, GenericFCV::kLatest}};

            const bool isUpgradingOrDowngradingShape =
                std::any_of(upgradingOrDowngradingShapes.begin(),
                            upgradingOrDowngradingShapes.end(),
                            [&](const auto& t) {
                                return t == std::tuple{version, *targetVersion, *previousVersion};
                            });
            if (!isUpgradingOrDowngradingShape) {
                return Status(ErrorCodes::Error(11948503),
                              str::stream() << "Invalid transitional shape for FCV document with "
                                            << "'phase' >= kEnableTargetFeatures: "
                                            << featureCompatibilityVersionDoc);
            }
            return *targetVersion;
        }

        // Downgrading FCV.
        if ((version == GenericFCV::kLastLTS || version == GenericFCV::kLastContinuous) &&
            version == targetVersion) {
            // Downgrading FCV must have a "previousVersion" field equal to kLatest.
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
            if (previousVersion != GenericFCV::kLatest) {
                return Status(
                    ErrorCodes::Error(4926901),
                    str::stream()
                        << "When present in downgrading states, '"
                        << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                        << "' field must be the latest binary version in "
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

        // Steady-state FCV must not have a "previousVersion" field.
        if (previousVersion) {
            return Status(
                ErrorCodes::Error(4926903),
                str::stream()
                    << "Unexpected field "
                    << FeatureCompatibilityVersionDocument::kPreviousVersionFieldName
                    << " in non-transitioning states for " << multiversion::kParameterName
                    << " document in "
                    << NamespaceString::kServerConfigurationNamespace.toStringForErrorMsg() << ": "
                    << featureCompatibilityVersionDoc << ". See "
                    << feature_compatibility_version_documentation::compatibilityLink() << ".");
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
