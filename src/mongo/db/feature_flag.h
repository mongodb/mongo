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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/version_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/version/releases.h"

#include <variant>
#include <vector>

#include <absl/container/flat_hash_map.h>

namespace mongo {
class IncrementalFeatureRolloutContext;

class FeatureFlag {
public:
    virtual ~FeatureFlag() = default;

    /**
     * Indicates whether runtime changes to the flag via the 'setParameter' command should be
     * allowed.
     */
    virtual bool allowRuntimeToggle() const = 0;

    /**
     * Populate a BSON object with the flag's value and any other properties.
     */
    virtual void appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const = 0;

    /**
     * Add any feature properties to the "details" object that are limited to verbose descriptions
     * of the flag.
     */
    virtual void appendFlagDetails(BSONObjBuilder& detailsBuilder) const {};

    /**
     * Checks if the flag is enabled, consulting the VersionContext and
     * IncrementalFeatureRolloutContext as appropriate.
     */
    virtual bool checkWithContext(const VersionContext& vCtx,
                                  IncrementalFeatureRolloutContext& ifrContext,
                                  ServerGlobalParams::FCVSnapshot fcv) = 0;

    /**
     * Returns true if there is a possibility of the flag becoming enabled within the current
     * process lifetime.
     */
    virtual bool canBeEnabled() const = 0;

    /**
     * Returns the boolean value representing the flag's user-configured value, which does not
     * necessarily indicate whether the feature is enabled.
     *
     * Intended for use by 'FeatureFlagServerParameter' to implement client configuration of the
     * flag. Do not use this interface to query the flag.
     */
    virtual bool getForServerParameter() const = 0;

    /**
     * Configure the flag according to a user-supplied bool value.
     *
     * Intended for use by 'FeatureFlagServerParameter' to implement client configuration of the
     * flag. Do not use this interface to update the flag.
     */
    virtual void setForServerParameter(bool enabled) = 0;

    /**
     * Returns true if the flag is an Incremental Feature Rollout (IFR) flag.
     */
    virtual bool isForIncrementalFeatureRollout() const {
        return false;
    }

    /**
     * Add the flag to any process-global registry that it belongs to.
     *
     * Intended for use by 'FeatureFlagServerParameter' during process intitialization.
     */
    virtual void registerFlag() {}
};

/**
 * The superclass for any feature flag that can optionally be used as a condition for
 * enabling/disabling a server parameter.
 */
class ParameterGatingFeatureFlag : public FeatureFlag {
public:
    /**
     * Returns true if a server parameter that is conditionalized on this flag should be enabled.
     */
    bool isServerParameterEnabled(multiversion::FeatureCompatibilityVersion fcv) {
        return isEnabledOnVersion(fcv);
    }

    /**
     * Returns true if the flag is set to true and enabled on the target FCV version.
     *
     * This function is used in the 'setFeatureCompatibilityVersion' command where the in-memory FCV
     * is in flux.
     */
    virtual bool isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const = 0;
};

/**
 * BinaryCompatibleFeatureFlag is a simple boolean feature flag whose value is only set at startup.
 * Its value does not change at runtime, nor during FCV upgrade/downgrade.
 */
class BinaryCompatibleFeatureFlag : public ParameterGatingFeatureFlag {
public:
    explicit BinaryCompatibleFeatureFlag(bool enabled) : _enabled(enabled) {}

    // Non-copyable, non-movable
    BinaryCompatibleFeatureFlag(const BinaryCompatibleFeatureFlag&) = delete;
    BinaryCompatibleFeatureFlag& operator=(const BinaryCompatibleFeatureFlag&) = delete;

    bool isEnabled() const {
        return _enabled;
    }

    bool allowRuntimeToggle() const override {
        return false;
    }

    void appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const override;

    bool checkWithContext(const VersionContext& vCtx,
                          IncrementalFeatureRolloutContext& ifrContext,
                          ServerGlobalParams::FCVSnapshot fcv) override {
        return _enabled;
    }

    bool canBeEnabled() const override {
        return _enabled;
    }

    bool getForServerParameter() const override {
        return _enabled;
    }

    void setForServerParameter(bool enabled) override {
        _enabled = enabled;
    }

    bool isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const override {
        return _enabled;
    }

private:
    bool _enabled;
};

/**
 * FCVGatedFeatureFlag is a boolean feature flag which is be enabled or disabled depending on FCV.
 * The feature flag is enabled if the current FCV is greater than or equal than the specified
 * threshold version and it is defined as enabled by default. It is not implicitly convertible to
 * bool to force all call sites to make a decision about what check to use.
 */
class FCVGatedFeatureFlag : public ParameterGatingFeatureFlag {
public:
    FCVGatedFeatureFlag(bool enabled,
                        StringData versionString,
                        bool enableOnTransitionalFCV = false);

    // Non-copyable, non-movable
    FCVGatedFeatureFlag(const FCVGatedFeatureFlag&) = delete;
    FCVGatedFeatureFlag& operator=(const FCVGatedFeatureFlag&) = delete;

    /**
     * Returns true if the flag is set to true and enabled for this FCV version.
     * If the functionality of this function changes, make sure that the
     * isEnabled/isPresentAndEnabled functions in feature_flag_util.js also incorporate the change.
     */
    bool isEnabled(const VersionContext& vCtx, ServerGlobalParams::FCVSnapshot fcv) const;

    /**
     * Returns true if the flag is set to true and enabled for this FCV version. If the FCV version
     * is unset, instead checks against the default last LTS FCV version.
     */
    bool isEnabledUseLastLTSFCVWhenUninitialized(const VersionContext& vCtx,
                                                 ServerGlobalParams::FCVSnapshot fcv) const;


    /**
     * Returns true if the flag is set to true and enabled for this FCV version. If the FCV version
     * is unset, instead checks against the latest FCV version.
     */
    bool isEnabledUseLatestFCVWhenUninitialized(const VersionContext& vCtx,
                                                ServerGlobalParams::FCVSnapshot fcv) const;

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

    bool isEnabledOnVersion(multiversion::FeatureCompatibilityVersion targetFCV) const override;

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

    bool allowRuntimeToggle() const override {
        return false;
    }

    void appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const override;

    bool checkWithContext(const VersionContext& vCtx,
                          IncrementalFeatureRolloutContext& ifrContext,
                          ServerGlobalParams::FCVSnapshot fcv) override {
        return isEnabled(vCtx, fcv);
    }

    bool canBeEnabled() const override {
        return _enabled;
    }

    bool getForServerParameter() const override {
        return _enabled;
    }

    void setForServerParameter(bool enabled) override;

private:
    bool _enabled;
    bool _enableOnTransitionalFCV;
    multiversion::FeatureCompatibilityVersion _version;
};

/**
 * Like FCVGatedFeatureFlag, but contains overloads that allow checking if an FCV-gated feature flag
 * is enabled, ignoring the context's operation FCV.
 *
 * This is a transitional solution to allow old FCV-gated feature flag checks to work until they
 * are adapted to the operation FCV aware API.
 */
class LegacyContextUnawareFCVGatedFeatureFlag : public FCVGatedFeatureFlag {
public:
    using FCVGatedFeatureFlag::FCVGatedFeatureFlag;

    // Provide methods compatible with the old API, which didn't take a VersionContext parameter
    bool isEnabled(ServerGlobalParams::FCVSnapshot fcv) const;
    bool isEnabledUseLastLTSFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;
    bool isEnabledUseLatestFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;

    // Avoid shadowing the original overloads with the compatibility stubs above
    using FCVGatedFeatureFlag::isEnabled;
    using FCVGatedFeatureFlag::isEnabledUseLastLTSFCVWhenUninitialized;
    using FCVGatedFeatureFlag::isEnabledUseLatestFCVWhenUninitialized;
};

/**
 * Describes where in the release cycle a feature is.
 */
enum class RolloutPhase {
    // The feature is not ready for release and is disabled by default.
    inDevelopment,

    // The feature is ready to be released via the incremental rollout process but is still disabled
    // by default.
    rollout,

    // The feature is enabled by default.
    released,
};

class IncrementalRolloutFeatureFlag : public FeatureFlag {
public:
    static const std::vector<const IncrementalRolloutFeatureFlag*>& getAll();

    IncrementalRolloutFeatureFlag(StringData flagName, RolloutPhase phase, bool value)
        : _flagName(std::string{flagName}), _phase(phase), _value(value) {}

    /**
     * Returns true if the feature is currently enabled, false otherwise. Also increments the
     * process-global counter tracking the total number of checks of this flag.
     *
     * NB: Unlike other feature flags, incremental rollout flags _can_ change their value at
     * runtime. Callers should take care not to assume that repeated calls to this check will return
     * the same value. Consider using an 'IncrementalFeatureRolloutContext' when querying the flag
     * value.
     */
    bool checkEnabled();

    /**
     * Add a document to the 'flagStats' array of the form:
     * {
     *   "name": <string>,
     *   "value": <bool>,
     *   "falseChecks": <number>,
     *   "trueChecks": <number>,
     *   "numToggles": <number>,
     * }
     */
    void appendFlagStats(BSONArrayBuilder& flagStats) const;

    const std::string& getName() const {
        return _flagName;
    }

    bool allowRuntimeToggle() const override {
        return true;
    }

    void appendFlagValueAndMetadata(BSONObjBuilder& flagBuilder) const override;

    void appendFlagDetails(BSONObjBuilder& detailsBuilder) const override;

    bool checkWithContext(const VersionContext& vCtx,
                          IncrementalFeatureRolloutContext& ifrContext,
                          ServerGlobalParams::FCVSnapshot fcv) override;

    bool canBeEnabled() const override {
        return true;
    }

    bool getForServerParameter() const override {
        return _value.loadRelaxed();
    }

    /**
     * Set a new value for the flag and, if the new value is different from the previous value,
     * increment the process-wide counter for how many times the flag was toggled.
     */
    void setForServerParameter(bool value) override;

    bool isForIncrementalFeatureRollout() const override {
        return true;
    }

    void registerFlag() override {
        registerFlag(this);
    }

private:
    // Adds flag to the global list of flags returned by the 'getAll()' method. Only safe to call as
    // part of process initialization.
    static void registerFlag(IncrementalRolloutFeatureFlag* flag);

    std::string _flagName;
    RolloutPhase _phase;
    Atomic<bool> _value;

    Atomic<int64_t> _numFalseChecks;
    Atomic<int64_t> _numTrueChecks;
    Atomic<int64_t> _numToggles;
};

/**
 * Records a set of 'IncrementalRolloutFeatureFlag's that were queried during an operation along
 * with their values. An operation with behavior controlled by 'IncrementalRolloutFeatureFlag's
 * should create an 'IncrementalRolloutFeatureContext' and uses its 'getSavedFlagValue()' method
 * instead of directly querying the flag for two reasons:
 *   1. The saved record of which flags were consulted and which features were enabled is useful
 *      diagnostically.
 *   2. Repeated checks of a flag via the same instance of 'IncrementalFeatureRolloutContext' will
 *      all return the same value, helping avoid errors caused by changes to a feature flag that are
 *      concurrent with an ongoing operation.
 */
class IncrementalFeatureRolloutContext {
public:
    /**
     * Returns the saved value of a feature flag when there is one or queries the flag via
     * 'checkEnabled()' and saves its value when there is not.
     */
    bool getSavedFlagValue(IncrementalRolloutFeatureFlag& flag);

    /**
     * Writes a diagnostic record of queried flags and their values. Each element of the array is a
     * document with 'name' and 'value' fields.
     */
    void appendSavedFlagValues(BSONArrayBuilder& builder) const;

private:
    absl::flat_hash_map<const IncrementalRolloutFeatureFlag*, bool> _savedFlagValues;
};
}  // namespace mongo
