/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/feature_flag.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/version/releases.h"

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
FCVGatedFeatureFlag::FCVGatedFeatureFlag(bool enabled,
                                         StringData versionString,
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
bool FCVGatedFeatureFlag::isEnabled(const VersionContext& vCtx,
                                    const ServerGlobalParams::FCVSnapshot fcv) const {
    const auto currentFcv = vCtx.getOperationFCV(VersionContext::Passkey()).value_or(fcv);

    return isEnabledOnVersion(currentFcv.getVersion());
}

bool FCVGatedFeatureFlag::isEnabledUseLastLTSFCVWhenUninitialized(
    const VersionContext& vCtx, const ServerGlobalParams::FCVSnapshot fcv) const {
    const auto currentFcv = vCtx.getOperationFCV(VersionContext::Passkey()).value_or(fcv);
    // (Generic FCV reference): This reference is needed for the feature flag check API.
    const auto applicableFcv = currentFcv.isVersionInitialized()
        ? currentFcv
        : ServerGlobalParams::FCVSnapshot(multiversion::GenericFCV::kLastLTS);

    return isEnabledOnVersion(applicableFcv.getVersion());
}

bool FCVGatedFeatureFlag::isEnabledUseLatestFCVWhenUninitialized(
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
bool FCVGatedFeatureFlag::isEnabledAndIgnoreFCVUnsafe() const {
    return _enabled;
}

bool FCVGatedFeatureFlag::isEnabledOnVersion(
    multiversion::FeatureCompatibilityVersion targetFCV) const {
    if (!_enabled) {
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

bool FCVGatedFeatureFlag::isDisabledOnTargetFCVButEnabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled) {
        return false;
    }

    return originalFCV >= _version && targetFCV < _version;
}

bool FCVGatedFeatureFlag::isEnabledOnTargetFCVButDisabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled) {
        return false;
    }

    return targetFCV >= _version && originalFCV < _version;
}

void FCVGatedFeatureFlag::appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const {
    flagBuilder.append("value", _enabled);
    if (_enabled) {
        flagBuilder.append(
            "version",
            FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(_version));
    }
    flagBuilder.append("fcv_gated", true);

    auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
    if (fcvSnapshot.isVersionInitialized()) {
        // TODO (SERVER-102076): Use VersionContext from opCtx instead of
        // kVersionContextIgnored_UNSAFE.
        flagBuilder.append("currentlyEnabled",
                           isEnabled(kVersionContextIgnored_UNSAFE, fcvSnapshot));
    }
}

void FCVGatedFeatureFlag::setForServerParameter(bool enabled) {
    _enabled = enabled;
}

bool LegacyContextUnawareFCVGatedFeatureFlag::isEnabled(ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabled(kVersionContextIgnored_UNSAFE, fcv);
}

bool LegacyContextUnawareFCVGatedFeatureFlag::isEnabledUseLastLTSFCVWhenUninitialized(
    ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabledUseLastLTSFCVWhenUninitialized(kVersionContextIgnored_UNSAFE, fcv);
}

bool LegacyContextUnawareFCVGatedFeatureFlag::isEnabledUseLatestFCVWhenUninitialized(
    ServerGlobalParams::FCVSnapshot fcv) const {
    return isEnabledUseLatestFCVWhenUninitialized(kVersionContextIgnored_UNSAFE, fcv);
}

namespace {
std::vector<const IncrementalRolloutFeatureFlag*>& getMutableAllIncrementalRolloutFeatureFlags() {
    static StaticImmortal<std::vector<const IncrementalRolloutFeatureFlag*>> flags;
    return *flags;
}
}  // namespace

const std::vector<const IncrementalRolloutFeatureFlag*>& IncrementalRolloutFeatureFlag::getAll() {
    return getMutableAllIncrementalRolloutFeatureFlags();
}

bool IncrementalRolloutFeatureFlag::checkEnabled() {
    auto checkResult = _value.load();
    (checkResult ? _numTrueChecks : _numFalseChecks).addAndFetch(1);
    return checkResult;
}

void IncrementalRolloutFeatureFlag::appendFlagStats(BSONArrayBuilder& flagStats) const {
    BSONObjBuilder{flagStats.subobjStart()}
        .append("name", _flagName)
        .append("value", _value.loadRelaxed())
        .append("falseChecks", static_cast<long long>(_numFalseChecks.loadRelaxed()))
        .append("trueChecks", static_cast<long long>(_numTrueChecks.loadRelaxed()))
        .append("numToggles", static_cast<long long>(_numToggles.loadRelaxed()));
}

void IncrementalRolloutFeatureFlag::appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const {
    bool enabled = _value.loadRelaxed();
    flagBuilder.append("value", enabled);
    if (enabled) {
        // (Generic FCV reference): Feature flag support.
        flagBuilder.append("version",
                           FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
                               multiversion::GenericFCV::kLatest));
    }
    flagBuilder.append("fcv_gated", false);

    if (serverGlobalParams.featureCompatibility.acquireFCVSnapshot().isVersionInitialized()) {
        flagBuilder.append("currentlyEnabled", enabled);
    }
}

void IncrementalRolloutFeatureFlag::appendFlagDetails(BSONObjBuilder& detailsBuilder) const {
    std::string phaseName = [&]() {
        switch (_phase) {
            case RolloutPhase::inDevelopment:
                return "inDevelopment";
            case RolloutPhase::rollout:
                return "rollout";
            case RolloutPhase::released:
                return "released";
        }
        MONGO_UNREACHABLE_TASSERT(101023);
    }();
    detailsBuilder.append("incrementalFeatureRolloutPhase", phaseName);
}

bool IncrementalRolloutFeatureFlag::checkWithContext(const VersionContext& vCtx,
                                                     IncrementalFeatureRolloutContext& ifrContext,
                                                     ServerGlobalParams::FCVSnapshot fcv) {
    return ifrContext.getSavedFlagValue(*this);
}

void IncrementalRolloutFeatureFlag::setForServerParameter(bool value) {
    auto previousValue = _value.swap(value);

    if (previousValue != value) {
        _numToggles.addAndFetch(1);
    }
}

void IncrementalRolloutFeatureFlag::registerFlag(IncrementalRolloutFeatureFlag* flag) {
    getMutableAllIncrementalRolloutFeatureFlags().push_back(flag);
}

bool IncrementalFeatureRolloutContext::getSavedFlagValue(IncrementalRolloutFeatureFlag& flag) {
    if (auto flagIt = _savedFlagValues.find(&flag); flagIt != _savedFlagValues.end()) {
        return flagIt->second;
    } else {
        bool value = flag.checkEnabled();
        _savedFlagValues.emplace(&flag, value);
        return value;
    }
}

void IncrementalFeatureRolloutContext::appendSavedFlagValues(BSONArrayBuilder& builder) const {
    for (auto&& [flag, savedValue] : _savedFlagValues) {
        BSONObjBuilder flagBuilder(builder.subobjStart());
        flagBuilder.append("name", flag->getName());
        flagBuilder.appendBool("value", savedValue);
    }
}
}  // namespace mongo
