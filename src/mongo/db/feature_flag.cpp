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

#include "mongo/util/debug_util.h"
#include "mongo/util/version/releases.h"

namespace mongo {

// (Generic FCV reference): feature flag support
FeatureFlag::FeatureFlag(bool enabled, StringData versionString, bool shouldBeFCVGated)
    : _enabled(enabled),
      _version(multiversion::GenericFCV::kLatest),
      _shouldBeFCVGated(shouldBeFCVGated) {

    // Verify the feature flag invariants. IDL binder verifies these hold but we add these checks to
    // prevent incorrect direct instantiation.
    //
    // If default is true, then version should be present.
    // If default is false, then no version is allowed.
    if (kDebugBuild) {
        if (enabled && shouldBeFCVGated) {
            dassert(!versionString.empty());
        } else {
            dassert(versionString.empty());
        }
    }

    if (!versionString.empty()) {
        _version = FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(versionString);
    }
}

bool FeatureFlag::isEnabled(const ServerGlobalParams::FeatureCompatibility& fcv) const {
    // If the feature flag is not FCV gated, return whether it is enabled.
    if (!_shouldBeFCVGated) {
        return _enabled;
    }


    // If the FCV is not initialized yet, we check whether the feature flag is enabled on the last
    // LTS FCV, which is the lowest FCV we can have on this server. Because the version of a feature
    // flag is not supposed to change, we are sure that if the feature flag is enabled on the last
    // LTS FCV, it is enabled on all FCVs this server can have.
    if (!fcv.isVersionInitialized()) {
        // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
        return isEnabledOnVersion(multiversion::GenericFCV::kLastLTS);
    }

    if (!_enabled) {
        return false;
    }

    // If the feature flag is enabled, return whether the server's FCV is >= to the version the
    // feature flag was enabled on.
    return fcv.isGreaterThanOrEqualTo(_version);
}

// isEnabledAndIgnoreFCVUnsafe should NOT be used in general, as it checks if the feature flag is
// turned on, regardless of which FCV we are on. It can result in unsafe scenarios
// where we enable a feature on an FCV where it is not supported or where the feature has not been
// fully implemented yet. In order to use isEnabledAndIgnoreFCVUnsafe, you **must** add a comment
// above that line starting with "(Ignore FCV check):" describing why we can safely ignore checking
// the FCV here.
// Note that if the feature flag does not have any upgrade/downgrade concerns, then shouldBeFCVGated
// should be set to false and FeatureFlag::isEnabled() should be used instead of this function.
bool FeatureFlag::isEnabledAndIgnoreFCVUnsafe() const {
    return _enabled;
}

// isEnabledAndIgnoreFCVUnsafeAtStartup should only be used on startup, if we want to check if the
// feature flag if the feature flag is turned on, regardless of which FCV we are on.
// Note that if the feature flag does not have any upgrade/downgrade concerns, then shouldBeFCVGated
// should be set to false and FeatureFlag::isEnabled() should be used instead of this function.
bool FeatureFlag::isEnabledAndIgnoreFCVUnsafeAtStartup() const {
    return _enabled;
}

bool FeatureFlag::isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const {
    if (!_enabled) {
        return false;
    }

    return targetFCV >= _version;
}

bool FeatureFlag::isDisabledOnTargetFCVButEnabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled) {
        return false;
    }

    return originalFCV >= _version && targetFCV < _version;
}

bool FeatureFlag::isEnabledOnTargetFCVButDisabledOnOriginalFCV(
    multiversion::FeatureCompatibilityVersion targetFCV,
    multiversion::FeatureCompatibilityVersion originalFCV) const {
    if (!_enabled) {
        return false;
    }

    return targetFCV >= _version && originalFCV < _version;
}

multiversion::FeatureCompatibilityVersion FeatureFlag::getVersion() const {
    uassert(5111001, "Feature Flag is not enabled, cannot retrieve version", _enabled);

    return _version;
}

void FeatureFlag::set(bool enabled) {
    _enabled = enabled;
}

FeatureFlagServerParameter::FeatureFlagServerParameter(StringData name, FeatureFlag& storage)
    : ServerParameter(name, ServerParameterType::kStartupOnly), _storage(storage) {}

void FeatureFlagServerParameter::append(OperationContext* opCtx,
                                        BSONObjBuilder* b,
                                        StringData name,
                                        const boost::optional<TenantId>&) {
    bool enabled = _storage.isEnabledAndIgnoreFCVUnsafe();

    {
        auto sub = BSONObjBuilder(b->subobjStart(name));
        sub.append("value"_sd, enabled);

        if (enabled) {
            sub.append("version",
                       FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
                           _storage.getVersion()));
        }
    }
}

void FeatureFlagServerParameter::appendSupportingRoundtrip(OperationContext* opCtx,
                                                           BSONObjBuilder* b,
                                                           StringData name,
                                                           const boost::optional<TenantId>&) {
    bool enabled = _storage.isEnabledAndIgnoreFCVUnsafe();
    b->append(name, enabled);
}

Status FeatureFlagServerParameter::set(const BSONElement& newValueElement,
                                       const boost::optional<TenantId>&) {
    bool newValue;

    if (auto status = newValueElement.tryCoerce(&newValue); !status.isOK()) {
        return {status.code(),
                str::stream() << "Failed setting " << name() << ": " << status.reason()};
    }

    _storage.set(newValue);

    return Status::OK();
}

Status FeatureFlagServerParameter::setFromString(StringData str, const boost::optional<TenantId>&) {
    auto swNewValue = idl_server_parameter_detail::coerceFromString<bool>(str);
    if (!swNewValue.isOK()) {
        return swNewValue.getStatus();
    }

    _storage.set(swNewValue.getValue());

    return Status::OK();
}

}  // namespace mongo
