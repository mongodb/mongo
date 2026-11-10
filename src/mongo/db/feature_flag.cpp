// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/feature_flag.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/db/version_context_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/version/releases.h"

#include <string_view>

namespace mongo {

void BinaryCompatibleFeatureFlag::appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const {
    flagBuilder.append("value", _enabled);
    if (_enabled) {
        // (Generic FCV reference): Feature flag support.
        flagBuilder.append("version",
                           FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
                               multiversion::GenericFCV::kLatest));
    }
    flagBuilder.append("fcv_gated", false);

    if (serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isVersionInitialized()) {
        flagBuilder.append("currentlyEnabled", _enabled);
    }
}

// (Generic FCV reference): Feature flag support.
FCVGatedFeatureFlagBase::FCVGatedFeatureFlagBase(bool enabled,
                                                 std::string_view versionString,
                                                 bool enableOnTransitionalFCV)
    : _enabled(enabled),
      _enableOnTransitionalFCV(enableOnTransitionalFCV),
      _version(multiversion::GenericFCV::kLatest) {

    // Verify the feature flag invariants. IDL binder verifies these hold but we add these checks to
    // prevent incorrect direct instantiation.
    //
    // If default is true, then version should be present.
    // If default is false, then no version is allowed.
    if (kDebugBuild) {
        if (enabled) {
            dassert(!versionString.empty());
        } else {
            dassert(versionString.empty());
        }
    }

    if (!versionString.empty()) {
        _version = FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(versionString);
    }
}

// If the functionality of this function changes, make sure that the isEnabled/isPresentAndEnabled
// functions in feature_flag_util.js also incorporate the change.
bool FCVGatedFeatureFlagBase::isEnabled(const VersionContext& vCtx,
                                        const ServerGlobalParams::FCVSnapshot fcv) const {
    const auto currentFcv = vCtx.getOperationFCV(VersionContext::Passkey()).value_or(fcv);

    tassert(11590501,
            "Trying to check feature flag on undefined FCV",
            currentFcv.isVersionInitialized());
    return isEnabledOnVersion(currentFcv.getVersion());
}

bool FCVGatedFeatureFlagBase::isEnabledUseLastLTSFCVWhenUninitialized(
    const VersionContext& vCtx, const ServerGlobalParams::FCVSnapshot fcv) const {
    const auto currentFcv = vCtx.getOperationFCV(VersionContext::Passkey()).value_or(fcv);
    // (Generic FCV reference): This reference is needed for the feature flag check API.
    const auto applicableFcv = currentFcv.isVersionInitialized()
        ? currentFcv
        : ServerGlobalParams::FCVSnapshot(multiversion::GenericFCV::kLastLTS);

    return isEnabledOnVersion(applicableFcv.getVersion());
}

bool FCVGatedFeatureFlagBase::isEnabledUseLatestFCVWhenUninitialized(
    const VersionContext& vCtx, const ServerGlobalParams::FCVSnapshot fcv) const {
    const auto currentFcv = vCtx.getOperationFCV(VersionContext::Passkey()).value_or(fcv);
    // (Generic FCV reference): This reference is needed for the feature flag check API.
    const auto applicableFcv = currentFcv.isVersionInitialized()
        ? currentFcv
        : ServerGlobalParams::FCVSnapshot(multiversion::GenericFCV::kLatest);

    return isEnabledOnVersion(applicableFcv.getVersion());
}

// isEnabledAndIgnoreFCVUnsafe should NOT be used in general, as it checks if the feature flag is
// turned on, regardless of which FCV we are on. It can result in unsafe scenarios
// where we enable a feature on an FCV where it is not supported or where the feature has not been
// fully implemented yet. In order to use isEnabledAndIgnoreFCVUnsafe, you **must** add a comment
// above that line starting with "(Ignore FCV check):" describing why we can safely ignore checking
// the FCV here.
// isEnabled() is prefered over this function since it will prevent upgrade/downgrade issues,
// or use isEnabledUseLatestFCVWhenUninitialized if your feature flag could be run while FCV
// is uninitialized during initial sync.
// Note that if the feature flag does not have any upgrade/downgrade concerns, then fcv_gated
// should be set to false and BinaryCompatibleFeatureFlag should be used instead of this function.
bool FCVGatedFeatureFlagBase::isEnabledAndIgnoreFCVUnsafe() const {
    return _enabled.load();
}

bool FCVGatedFeatureFlagBase::isEnabledOnVersion(
    multiversion::FeatureCompatibilityVersion targetFCV) const {
    if (!_enabled.load()) {
        return false;
    }

    if (targetFCV >= _version) {
        return true;
    }

    if (_enableOnTransitionalFCV &&
        ServerGlobalParams::FCVSnapshot::isUpgradingOrDowngrading(targetFCV)) {
        const auto transitionInfo = multiversion::getTransitionFCVInfo(targetFCV);
        // During upgrade, enable the feature flag, as if we were already on the target FCV
        // During downgrade, keep the feature flag enabled as if we were still on the source FCV
        const auto transitionTarget =
            transitionInfo.to > transitionInfo.from ? transitionInfo.to : transitionInfo.from;
        return transitionTarget >= _version;
    }

    return false;
}

bool FCVGatedFeatureFlagBase::isDisabledOnTargetFCVButEnabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled.load()) {
        return false;
    }

    return originalFCV >= _version && targetFCV < _version;
}

bool FCVGatedFeatureFlagBase::isEnabledOnTargetFCVButDisabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled.load()) {
        return false;
    }

    return targetFCV >= _version && originalFCV < _version;
}

void FCVGatedFeatureFlagBase::appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const {
    flagBuilder.append("value", _enabled.load());
    if (_enabled.load()) {
        flagBuilder.append(
            "version",
            FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(_version));
    }
    flagBuilder.append("fcv_gated", true);

    auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (fcvSnapshot.isVersionInitialized()) {
        // TODO (SERVER-114119): Use VersionContext from opCtx instead of
        // kVersionContextIgnored_UNSAFE.
        flagBuilder.append("currentlyEnabled",
                           isEnabled(kVersionContextIgnored_UNSAFE, fcvSnapshot));
    }
}

void FCVGatedFeatureFlagBase::setForServerParameter(bool enabled) {
    _enabled.store(enabled);
}

bool OperationFCVOnlyFCVGatedFeatureFlag::isEnabled(const VersionContext& vCtx) const {
    auto globalFcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    assertCheckingAgainstOFCV(vCtx, globalFcv);
    return FCVGatedFeatureFlagBase::isEnabled(vCtx, globalFcv);
}

bool OperationFCVOnlyFCVGatedFeatureFlag::isEnabledUseLastLTSFCVWhenUninitialized(
    const VersionContext& vCtx) const {
    auto globalFcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    assertCheckingAgainstOFCV(vCtx, globalFcv);
    return FCVGatedFeatureFlagBase::isEnabledUseLastLTSFCVWhenUninitialized(vCtx, globalFcv);
}

bool OperationFCVOnlyFCVGatedFeatureFlag::isEnabledUseLatestFCVWhenUninitialized(
    const VersionContext& vCtx) const {
    auto globalFcv = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    assertCheckingAgainstOFCV(vCtx, globalFcv);
    return FCVGatedFeatureFlagBase::isEnabledUseLatestFCVWhenUninitialized(vCtx, globalFcv);
}

void OperationFCVOnlyFCVGatedFeatureFlag::assertCheckingAgainstOFCV(
    const VersionContext& vCtx, ServerGlobalParams::FCVSnapshot globalFcv) const {
    if (vCtx.hasOperationFCV()) {
        return;
    }

    // This operation could be part of a ShardingCoordinator started on FCV 8.0, which do not
    // have an OFCV. In this case, tolerate checking it against the global server FCV.
    if (!feature_flags::gStrictlyEnforceOperationFCVOnlyFCVGatedFeatureFlags
             .isEnabledUseLatestFCVWhenUninitialized(vCtx, globalFcv)) {
        // Ensure that this only happens in a sharded cluster, as we expect.
        tassert(11144901,
                "Expected relaxed OFCV-only feature flag checks to only happen on sharded clusters",
                !serverGlobalParams.clusterRole.has(ClusterRole::None));
        return;
    }

    tasserted(11144900, "Expected the feature flag to have been checked with an OFCV");
}

bool LegacyFCVSnapshotOnlyFCVGatedFeatureFlag::isEnabled(
    ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabled(kVersionContextIgnored_UNSAFE, fcv);
}

bool LegacyFCVSnapshotOnlyFCVGatedFeatureFlag::isEnabledUseLastLTSFCVWhenUninitialized(
    ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabledUseLastLTSFCVWhenUninitialized(kVersionContextIgnored_UNSAFE, fcv);
}

bool LegacyFCVSnapshotOnlyFCVGatedFeatureFlag::isEnabledUseLatestFCVWhenUninitialized(
    ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabledUseLatestFCVWhenUninitialized(kVersionContextIgnored_UNSAFE, fcv);
}

}  // namespace mongo
