// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <string_view>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
using namespace std::literals::string_view_literals;
class IFRSenderVersion;
class IncrementalFeatureRolloutContext;
class OperationContext;
class IFRSenderVersion;

class [[MONGO_MOD_OPEN]] FeatureFlag {
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
class [[MONGO_MOD_OPEN]] ParameterGatingFeatureFlag : public FeatureFlag {
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
class [[MONGO_MOD_OPEN]] BinaryCompatibleFeatureFlag : public ParameterGatingFeatureFlag {
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
 * Represents a boolean feature flag which is be enabled or disabled depending on FCV.
 * The feature flag is enabled if the current FCV is greater than or equal to the specified
 * threshold version and it is defined as enabled by default. It is not implicitly convertible to
 * bool to force all call sites to make a decision about what check to use.
 *
 * Due to the ongoing migration of FCV-gated feature flags from server FCV snapshots (`FCVSnapshot`)
 * to Operation FCV (`VersionContext`), this class is abstract, and its derived classes expose
 * different method signatures for feature flag checks.
 */
class [[MONGO_MOD_OPEN]] FCVGatedFeatureFlagBase : public ParameterGatingFeatureFlag {
public:
    FCVGatedFeatureFlagBase(bool enabled,
                            std::string_view versionString,
                            bool enableOnTransitionalFCV = false);

    // Non-copyable, non-movable
    FCVGatedFeatureFlagBase(const FCVGatedFeatureFlagBase&) = delete;
    FCVGatedFeatureFlagBase& operator=(const FCVGatedFeatureFlagBase&) = delete;

    /**
     * Returns true if this flag is enabled regardless of the current FCV version. When using this
     * function, you are allowing the feature flag to pass checking during transitional FCV states
     * and downgraded FCV, which means the code gated by this feature flag is allowed to run even if
     * the FCV requirement of this feature flag is not met.
     *
     * isEnabled() is preferred over this function since it will prevent upgrade/downgrade issues,
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

    bool canBeEnabled() const override {
        return _enabled.load();
    }

    bool getForServerParameter() const override {
        return _enabled.load();
    }

    void setForServerParameter(bool enabled) override;

protected:
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

private:
    Atomic<bool> _enabled;
    bool _enableOnTransitionalFCV;
    multiversion::FeatureCompatibilityVersion _version;
};

/**
 * A FCV-gated feature flag which can be checked against either an Operation FCV (`VersionContext`)
 * or a server FCV snapshot (`FCVSnapshot`).
 */
class [[MONGO_MOD_OPEN]] FCVGatedFeatureFlag : public FCVGatedFeatureFlagBase {
public:
    using FCVGatedFeatureFlagBase::FCVGatedFeatureFlagBase;

    // Make the protected isEnabled* methods from the base class public
    using FCVGatedFeatureFlagBase::isEnabled;
    using FCVGatedFeatureFlagBase::isEnabledUseLastLTSFCVWhenUninitialized;
    using FCVGatedFeatureFlagBase::isEnabledUseLatestFCVWhenUninitialized;

    bool checkWithContext(const VersionContext& vCtx,
                          IncrementalFeatureRolloutContext& ifrContext,
                          ServerGlobalParams::FCVSnapshot fcv) override {
        return isEnabled(vCtx, fcv);
    }
};

/**
 * A FCV-gated feature flag which can be checked only against an Operation FCV (`VersionContext`).
 */
class [[MONGO_MOD_OPEN]] OperationFCVOnlyFCVGatedFeatureFlag : public FCVGatedFeatureFlagBase {
public:
    using FCVGatedFeatureFlagBase::FCVGatedFeatureFlagBase;

    bool isEnabled(const VersionContext& vCtx) const;
    bool isEnabledUseLastLTSFCVWhenUninitialized(const VersionContext& vCtx) const;
    bool isEnabledUseLatestFCVWhenUninitialized(const VersionContext& vCtx) const;

    bool checkWithContext(const VersionContext& vCtx,
                          IncrementalFeatureRolloutContext& ifrContext,
                          ServerGlobalParams::FCVSnapshot fcv) override {
        return isEnabled(vCtx);
    }

private:
    void assertCheckingAgainstOFCV(const VersionContext& vCtx,
                                   ServerGlobalParams::FCVSnapshot globalFcv) const;
};

/**
 * A FCV-gated feature flag which can only be checked against a server FCV snapshot (`FCVSnapshot`),
 * ignoring the context's operation FCV.
 *
 * This is a transitional solution to allow old FCV-gated feature flag checks to work until they
 * are adapted to the operation FCV aware API.
 */
class [[MONGO_MOD_OPEN]] LegacyFCVSnapshotOnlyFCVGatedFeatureFlag : public FCVGatedFeatureFlag {
public:
    using FCVGatedFeatureFlag::FCVGatedFeatureFlag;

    // Provide methods compatible with the old API, which didn't take a VersionContext parameter
    bool isEnabled(ServerGlobalParams::FCVSnapshot fcv) const;
    bool isEnabledUseLastLTSFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;
    bool isEnabledUseLatestFCVWhenUninitialized(ServerGlobalParams::FCVSnapshot fcv) const;

    // Avoid shadowing the original overloads with the compatibility stubs above
    using FCVGatedFeatureFlagBase::isEnabled;
    using FCVGatedFeatureFlagBase::isEnabledUseLastLTSFCVWhenUninitialized;
    using FCVGatedFeatureFlagBase::isEnabledUseLatestFCVWhenUninitialized;
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
    release,
};

class [[MONGO_MOD_OPEN]] IncrementalRolloutFeatureFlag : public FeatureFlag {
public:
    static IncrementalRolloutFeatureFlag* findByName(std::string_view flagName);

    /**
     * Returns every registered IFR flag whose declared `serialize_on_outgoing_requests` version is
     * greater than `multiversion::GenericFCV::kLastLTS`, i.e. flags first introduced after our last
     * supported mixed-version release.
     */
    static const std::vector<IncrementalRolloutFeatureFlag*>& getFlagsIntroducedSinceLastLTS();

    IncrementalRolloutFeatureFlag(std::string_view flagName,
                                  RolloutPhase phase,
                                  bool value,
                                  std::string_view serializeOnOutgoingRequestsVersion = ""sv);

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

    void appendFlagStats(BSONArrayBuilder& flagStats) const;

    /**
     * For each flag, add a document to the 'flagStats' array of the form:
     * {
     *   "name": <string>,
     *   "value": <bool>,
     *   "falseChecks": <number>,
     *   "trueChecks": <number>,
     *   "numToggles": <number>,
     * }
     */
    static void appendFlagsStats(BSONArrayBuilder& flagStats);

    const std::string& getName() const {
        return _flagName;
    }

    /**
     * Returns true if this flag has declared an FCV version for outgoing request serialization.
     */
    bool shouldSerializeOnOutgoingRequests() const {
        return _serializeOnOutgoingRequestsVersion.has_value();
    }

    /**
     * Returns the FCV at which this flag began serializing on outgoing requests, or boost::none if
     * the flag does not participate in wire serialization.
     */
    boost::optional<multiversion::FeatureCompatibilityVersion>
    getSerializeOnOutgoingRequestsVersion() const {
        return _serializeOnOutgoingRequestsVersion;
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
    // Adds flag to the global list of flags. Only safe to call as part of process initialization.
    static void registerFlag(IncrementalRolloutFeatureFlag* flag);

    std::string _flagName;
    RolloutPhase _phase;
    Atomic<bool> _value;
    boost::optional<multiversion::FeatureCompatibilityVersion> _serializeOnOutgoingRequestsVersion;

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
    // Defined out-of-line so 'IFRSenderVersion' can be forward-declared in this header.
    IncrementalFeatureRolloutContext();
    ~IncrementalFeatureRolloutContext();

    /**
     * Builds an IFRContext from a request's `ifrFlags` payload. Marks the new context as wire-
     * installed so that downstream outgoing-request serializers can keep forwarding through
     * shard-to-shard hops while suppressing serialization on internal-origin traffic.
     *
     * `senderVersion` is the sender's full binary version (from the `ifrSenderVersion` wire
     * field). It may be null (as it will be for v8.3 senders)
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> fromWire(
        std::span<const BSONObj> flags, std::unique_ptr<IFRSenderVersion> senderVersion);

    /**
     * Like above, but provides a default 'senderVersion' - the current version.
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> fromWireForTest(
        std::span<const BSONObj> flags);

    /**
     * Builds an IFRContext for a request that arrived without any `ifrFlags` payload, and installs
     * it on 'opCtx' for future use. This happens when the sender is on a binary that predates the
     * IFR wire protocol (typical during a rolling upgrade: a last-lts router forwarding to a latest
     * shard), when an old shard forwards to a newer shard, or when the deployment is only a replica
     * set.
     *
     * The installed context is *not* marked as installed-from-wire.
     */
    static void installForRequestWithoutIfrFlags(OperationContext* opCtx);

    /**
     * `get()` returns the opCtx-decorated IFRContext, lazily constructing an empty one and
     * installing it on first call. `tryGet()` is the read-only version: returns the installed
     * context if there is one, otherwise nullptr. Use `tryGet()` from read paths that just want to
     * ask "is an IFRContext installed yet?" without materializing one.
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> get(OperationContext* opCtx);
    static std::shared_ptr<IncrementalFeatureRolloutContext> tryGet(OperationContext* opCtx);
    static bool isInstalled(OperationContext* opCtx);
    static void set(OperationContext* opCtx, std::shared_ptr<IncrementalFeatureRolloutContext> ctx);

    /**
     * Test-only factory: seeds an explicit set of flag values from a list of `{name, value}`
     * documents, skipping the wire-protocol resolution (unknown-flag handling, sender-version
     * scenarios, absent-flag defaulting) performed by `fromWire()`. The resulting context is
     * *not* marked installed-from-wire. Use this to simulate a router that sent a particular flag
     * value without exercising the full wire path.
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> forTest(
        std::span<const BSONObj> flags);

    /**
     * Returns a deep copy of this context: copies the saved flag values and (if present) the
     * sender version, but starts with no memoized egress metadata so the copy can be freshly
     * installed on an opCtx. Used to reproduce a precomputed template context per request without
     * rebuilding its flag map. See 'installForRequestWithoutIfrFlags()'.
     */
    std::shared_ptr<IncrementalFeatureRolloutContext> clone() const;

    /**
     * Builds the process-wide shard-server "no ifrFlags" template context. Invoked once at startup
     * from the CacheIfrFlagsIntroducedSinceLastLTS initializer, after the flag list it reads is
     * populated. Not for general use.
     */
    static void initShardServerDefaultTemplate();

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

    /**
     * Disables the value of an IFR flag for the lifetime and scope of this context specifically.
     * Primarily used for shard-router communication in the situation where we want to retry an
     * aggregate command, with the value of a previously-enabled IFR flag disabled.
     */
    void disableFlag(IncrementalRolloutFeatureFlag& flag);

    /**
     * Returns true if `IncrementalFeatureRolloutContext::fromWire()` produced this context.
     */
    bool isInstalledFromWire() const {
        return _senderVersion != nullptr;
    }

    /**
     * Returns the sender's full binary version captured when this context was built via
     * `fromWire()`. Returns nullptr for contexts that did not originate from a wire payload. If the
     * wire payload omitted `ifrSenderVersion`, `fromWire()` installs a sentinel (v8.3) version so
     * egress serialization can omit the field while still treating the context as wire-installed.
     */
    const IFRSenderVersion* getWireSenderVersion() const {
        return _senderVersion.get();
    }

    /**
     * Serializes this context onto the outgoing sharding-request metadata builder as the
     * `ifrSenderVersion` and `ifrFlags` wire fields, according to the forwarding rules:
     *   - If installed-from-wire: forward the original sender's version verbatim (if any) and
     *     re-serialize this binary's post-resolution flag values.
     *   - Otherwise (router originating a fresh non-wire request): emit our current local version.
     *
     * The produced sub-object is memoized on first invocation and reused for subsequent calls on
     * the same IFRContext instance, so restamping on a retry does not repeat the serialization
     * work.
     */
    void appendToEgressMetadata(BSONObjBuilder* bob);

    /**
     * Test-only: reports whether the egress metadata has been cached yet. Used by the invariant
     * on `set()` and by unit tests.
     */
    bool hasCachedEgressMetadataForTest() const {
        return _cachedEgressMetadata.has_value();
    }

private:
    /**
     *  Constructor for the 'from wire' case. 'senderVersion' must not be null.
     */
    IncrementalFeatureRolloutContext(std::span<const BSONObj> flags,
                                     std::unique_ptr<IFRSenderVersion> senderVersion);
    explicit IncrementalFeatureRolloutContext(std::span<const BSONObj> flags);

    // Process-wide template for the shard-server "arrived without ifrFlags" case: every release
    // flag introduced since the last LTS pinned to false. Populated once at startup by
    // 'initShardServerDefaultTemplate()' (invoked from the CacheIfrFlagsIntroducedSinceLastLTS
    // initializer, after the flag list it reads is built) and cloned per request rather than
    // rebuilt. Member function so it can reach the private map and constructor.
    static IncrementalFeatureRolloutContext& mutableShardServerDefaultTemplate();

    absl::flat_hash_map<const IncrementalRolloutFeatureFlag*, bool> _savedFlagValues;

    // Optional - set if this context was installed from the wire.
    // Held by unique_ptr rather than boost::optional so this header can forward-declare
    // 'IFRSenderVersion' rather than include 'generic_argument_gen.h' (which pulls a large
    // transitive header set that would create re-entrant #include cycles for callers upstream in
    // read_preference.h etc.). The destructor is defined out-of-line in the .cpp for the same
    // reason.
    std::unique_ptr<IFRSenderVersion> _senderVersion;

    // Memoized serialization of this context's egress-metadata sub-object. The IFRContext is
    // per-opCtx so there is no cross-context contention; simple lazy init suffices.
    boost::optional<BSONObj> _cachedEgressMetadata;
};
}  // namespace mongo
