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

#pragma once

#include <variant>

#include "mongo/base/string_data.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/server_options.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/version/releases.h"

namespace mongo {

/**
 * BinaryCompatibleFeatureFlag is a simple boolean feature flag whose value is only set at startup.
 * Its value does not change at runtime, nor during FCV upgrade/downgrade.
 */
class BinaryCompatibleFeatureFlag {
    friend class FeatureFlagServerParameter;  // For set(...)

public:
    explicit BinaryCompatibleFeatureFlag(bool enabled) : _enabled(enabled) {}

    // Non-copyable, non-movable
    BinaryCompatibleFeatureFlag(const BinaryCompatibleFeatureFlag&) = delete;
    BinaryCompatibleFeatureFlag& operator=(const BinaryCompatibleFeatureFlag&) = delete;

    bool isEnabled() const {
        return _enabled;
    }

    // TODO(SERVER-99874): Remove this stub
    bool isEnabled(ServerGlobalParams::FCVSnapshot fcv) const {
        return isEnabled();
    }
    bool isEnabledUseLastLTSFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const {
        return isEnabled();
    }
    bool isEnabledUseLatestFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const {
        return isEnabled();
    }
    bool isEnabledAndIgnoreFCVUnsafe() const {
        return isEnabled();
    }

private:
    void set(bool enabled) {
        _enabled = enabled;
    };

    bool _enabled;
};

/**
 * FCVGatedFeatureFlag is a boolean feature flag which is be enabled or disabled depending on FCV.
 * The feature flag is enabled if the current FCV is greater than or equal than the specified
 * threshold version and it is defined as enabled by default. It is not implicitly convertible to
 * bool to force all call sites to make a decision about what check to use.
 */
// TODO(SERVER-99351): Review the API exposed for FCV-gated feature flags
class FCVGatedFeatureFlag {
    friend class FeatureFlagServerParameter;  // For set(...)

public:
    FCVGatedFeatureFlag(bool enabled, StringData versionString);

    /**
     * Returns true if the flag is set to true and enabled for this FCV version.
     * If the functionality of this function changes, make sure that the
     * isEnabled/isPresentAndEnabled functions in feature_flag_util.js also incorporate the change.
     */
    bool isEnabled(ServerGlobalParams::FCVSnapshot fcv) const;

    /**
     * Returns true if the flag is set to true and enabled for this FCV version. If the FCV version
     * is unset, instead checks against the default last LTS FCV version.
     */
    bool isEnabledUseLastLTSFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;


    /**
     * Returns true if the flag is set to true and enabled for this FCV version. If the FCV version
     * is unset, instead checks against the latest FCV version.
     */
    bool isEnabledUseLatestFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;

    /**
     * Returns true if this flag is enabled regardless of the current FCV version. When using this
     * function, you are allowing the feature flag to pass checking during transitional FCV states
     * and downgraded FCV, which means the code gated by this feature flag is allowed to run even if
     * the FCV requirement of this feature flag is not met.
     *
     * isEnabled() is prefered over this function since it will prevent upgrade/downgrade issues,
     * or use isEnabledUseLatestFCVWhenUninitialized if your feature flag could be run while FCV is
     * uninitialized during initial sync.
     *
     * Note: A comment starting with (Ignore FCV check) is required for the use of this function.
     */
    bool isEnabledAndIgnoreFCVUnsafe() const;

    /**
     * Returns true if the flag is set to true and enabled on the target FCV version.
     *
     * This function is used in the 'setFeatureCompatibilityVersion' command where the in-memory FCV
     * is in flux.
     */
    bool isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const;

    /**
     * Returns true if the feature flag is disabled on targetFCV but enabled on originalFCV.
     */
    bool isDisabledOnTargetFCVButEnabledOnOriginalFCV(
        multiversion::FeatureCompatibilityVersion targetFCV,
        multiversion::FeatureCompatibilityVersion originalFCV) const;

    /**
     * Returns true if the feature flag is enabled on targetFCV but disabled on originalFCV.
     */
    bool isEnabledOnTargetFCVButDisabledOnOriginalFCV(
        multiversion::FeatureCompatibilityVersion targetFCV,
        multiversion::FeatureCompatibilityVersion originalFCV) const;

    /**
     * Return the version associated with this feature flag.
     *
     * Throws if feature is not enabled.
     */
    multiversion::FeatureCompatibilityVersion getVersion() const;

private:
    void set(bool enabled);

private:
    bool _enabled;
    multiversion::FeatureCompatibilityVersion _version;
};

/**
 * Expresses that a feature may depend on either a binary-compatible or FCV-gated feature flag.
 *
 * This class can wrap either a BinaryCompatibleFeatureFlag or an FCVGatedFeatureFlag, and provides
 * a single method to check if the feature is enabled.
 *
 * It can also represent a "null object" state, where the feature does not depend on a feature flag,
 * and thus it is unconditionally enabled.
 *
 * For FCV-gated feature flags, you must provide a callback defining how to check an FCV-gated flag,
 * to handle cases such as uninitialized FCV or checking multiple flags under the same FCV snapshot.
 *
 * If you don't need to support both binary-compatible and FCV-gated feature flags, you should
 * use BinaryCompatibleFeatureFlag or FCVGatedFeatureFlag directly.
 */
class CheckableFeatureFlagRef {
public:
    CheckableFeatureFlagRef() = default;
    explicit(false) CheckableFeatureFlagRef(BinaryCompatibleFeatureFlag& featureFlag)
        : _featureFlag(&featureFlag) {}
    explicit(false) CheckableFeatureFlagRef(FCVGatedFeatureFlag& featureFlag)
        : _featureFlag(&featureFlag) {}

    template <typename F>
    bool isEnabled(F&& isFcvGatedFlagEnabled) const {
        return visit(
            OverloadedVisitor{[](std::monostate) { return true; },
                              [](BinaryCompatibleFeatureFlag* impl) { return impl->isEnabled(); },
                              [&](FCVGatedFeatureFlag* impl) -> bool {
                                  return isFcvGatedFlagEnabled(*impl);
                              }},
            _featureFlag);
    }

private:
    std::variant<std::monostate, BinaryCompatibleFeatureFlag*, FCVGatedFeatureFlag*> _featureFlag;
};

/**
 * Expresses that a feature is not dependent on a feature flag, and
 * thus it is always enabled.
 */
inline constexpr CheckableFeatureFlagRef kDoesNotRequireFeatureFlag;

}  // namespace mongo
