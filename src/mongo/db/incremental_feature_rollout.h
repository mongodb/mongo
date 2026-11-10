// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/feature_flag_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/version/releases.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
class IFRSenderVersion;
class OperationContext;

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

    /**
     * Process-wide count of `fromWire()` constructions attempted. Increments once per inbound
     * request that carried an `ifrFlags` payload, regardless of how many flags were in it, before
     * any per-flag processing occurs — so it still increments even if a later flag in the same
     * payload causes construction to fail (e.g. a malformed field or an unknown-flag protocol
     * error). This keeps it in step with the per-flag wire-install counters, which are bumped
     * inline as each flag is processed and are therefore also retained on partial failure.
     */
    static int64_t getWireInstallsCount();

    /**
     * Process-wide count of protocol errors where one or more unknown IFR flags arrived from a
     * same-or-older sender. Non-zero indicates a misconfiguration.
     */
    static int64_t getUnknownWireFlagErrorsCount();

    /**
     * Process-wide count of incoming IFR flag values dropped because the local binary does not
     * know the flag name (sender is newer). A non-zero value indicates a mixed-version cluster is
     * sending IFR flags the local binary cannot interpret.
     */
    static int64_t getUnknownWireFlagsDroppedCount();

    /**
     * Process-wide count of active flags absent from an inbound wire payload (scenario 4) that were
     * conservatively resolved to false because the sender predates the flag's introduction.
     */
    static int64_t getAbsentFlagsConservativeFalseCount();

    /**
     * Process-wide count of active flags absent from an inbound wire payload (scenario 4) that were
     * resolved to this binary's local default because the sender is same-or-newer.
     */
    static int64_t getAbsentFlagsLocalDefaultCount();

    IncrementalRolloutFeatureFlag(
        std::string_view flagName,
        RolloutPhase phase,
        bool value,
        std::string_view serializeOnOutgoingRequestsVersion = std::string_view{});

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
     * Records that this flag was installed from an inbound wire payload with the given value.
     * Bumps the matching per-flag wire-install counter surfaced by 'appendFlagStats'.
     */
    void recordWireInstall(bool value);

    void appendFlagStats(BSONArrayBuilder& flagStats) const;

    /**
     * For each flag, add a document to the 'flagStats' array of the form:
     * {
     *   "name": <string>,
     *   "value": <bool>,
     *   "falseChecks": <number>,
     *   "trueChecks": <number>,
     *   "numToggles": <number>,
     *   "trueWireInstalls": <number>,
     *   "falseWireInstalls": <number>,
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

    // Per-flag counts of how many times this flag was installed from an inbound wire payload with a
    // true / false value (scenario 1 of the wire-resolution table).
    Atomic<int64_t> _numTrueWireInstalls;
    Atomic<int64_t> _numFalseWireInstalls;
};

/**
 * Records a set of 'IncrementalRolloutFeatureFlag's that were queried during an operation along
 * with their values. An operation with behavior controlled by 'IncrementalRolloutFeatureFlag's
 * should create an 'IncrementalFeatureRolloutContext' and use its 'getSavedFlagValue()' method
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
        std::span<const IFRFlagWireEntry> flags, std::unique_ptr<IFRSenderVersion> senderVersion);

    /**
     * Like above, but provides a default 'senderVersion' - the current version.
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> fromWireForTest(
        std::span<const IFRFlagWireEntry> flags);

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
        std::span<const IFRFlagWireEntry> flags);

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
     * Returns a fresh copy of the shard-server "no ifrFlags" template (every flag introduced since
     * the last LTS pinned to false). Requires 'initShardServerDefaultTemplate()' to have run.
     */
    static std::shared_ptr<IncrementalFeatureRolloutContext> cloneShardServerDefaultTemplate();

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

    /**
     * Names of flags that were present on the wire but unknown to this binary (and dropped because
     * the sender is newer). Populated only for wire-installed contexts; read by diagnostics.
     */
    const absl::flat_hash_set<std::string>& getUnknownWireFlags() const {
        return _unknownWireFlags;
    }

private:
    /**
     *  Constructor for the 'from wire' case. 'senderVersion' must not be null.
     */
    IncrementalFeatureRolloutContext(std::span<const IFRFlagWireEntry> flags,
                                     std::unique_ptr<IFRSenderVersion> senderVersion);
    explicit IncrementalFeatureRolloutContext(std::span<const IFRFlagWireEntry> flags);

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

    // Names of flags received on the wire that this binary does not recognize and dropped because
    // the sender is newer. Populated only for wire-installed contexts.
    absl::flat_hash_set<std::string> _unknownWireFlags;
};
}  // namespace mongo
