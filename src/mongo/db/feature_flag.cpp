// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/feature_flag.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/feature_compatibility_version_parser.h"
#include "mongo/db/ifr_unrecognized_flag_info.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/db/version_context_feature_flags_gen.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/ifr_sender_version.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/static_immortal.h"
#include "mongo/util/str.h"
#include "mongo/util/version.h"
#include "mongo/util/version/releases.h"

#include <charconv>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_set.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

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

namespace {
std::vector<IncrementalRolloutFeatureFlag*>& getMutableAllIncrementalRolloutFeatureFlags() {
    static StaticImmortal<std::vector<IncrementalRolloutFeatureFlag*>> flags;
    return *flags;
}

// Cache of IFR flags whose 'serialize_on_outgoing_requests' version is greater than kLastLTS — the
// list a receiver must conservatively disable when it gets a request from a sender that predates
// the flag entirely. The set is fixed once all flags have registered (registration happens during
// static initialization, before any MONGO_INITIALIZER runs), so we can populate this once and hand
// out a const reference on every request instead of rebuilding a vector per call.
std::vector<IncrementalRolloutFeatureFlag*>& getMutableFlagsIntroducedSinceLastLTS() {
    static StaticImmortal<std::vector<IncrementalRolloutFeatureFlag*>> flags;
    return *flags;
}

// Function-local static so 'VersionInfoInterface::instance()' is not touched until after
// 'main()' has installed a provider. A namespace-scope 'static const' initializer here runs
// during DSO '_init', before any 'MONGO_INITIALIZER' fires, and fatally asserts with fassert
// 40278 ("valid version info has not been configured").
const IFRSenderVersion& localSenderVersion() {
    static const IFRSenderVersion v = makeLocalIFRSenderVersion();
    return v;
}

// IFR flag introduction versions are currently declared at FCV (major.minor) granularity. This
// generates a 'major.minor.0.<int_min>' IFRSenderVersion to help with comparisons.
IFRSenderVersion toFullVersion(multiversion::FeatureCompatibilityVersion fcv) {
    IFRSenderVersion version;
    version.setMajor(multiversion::majorVersion(fcv));
    version.setMinor(multiversion::minorVersion(fcv));
    version.setPatch(0);
    version.setExtra(std::numeric_limits<int>::min());
    return version;
}

// Use a lazy-initialization here to avoid frontloading any work for traffic which doesn't consult
// any feature flags.
using DeferredIfrContext = Deferred<std::shared_ptr<IncrementalFeatureRolloutContext> (*)()>;
struct OpCtxIfrContext {
    DeferredIfrContext deferred{[] {
        return std::make_shared<IncrementalFeatureRolloutContext>();
    }};
};
const auto getIfrContextOnOpCtx = OperationContext::declareDecoration<OpCtxIfrContext>();

}  // namespace

IncrementalRolloutFeatureFlag* IncrementalRolloutFeatureFlag::findByName(
    std::string_view flagName) {
    for (auto* flag : getMutableAllIncrementalRolloutFeatureFlags()) {
        if (flag->getName() == flagName) {
            return flag;
        }
    }
    return nullptr;
}

IncrementalRolloutFeatureFlag::IncrementalRolloutFeatureFlag(
    std::string_view flagName,
    RolloutPhase phase,
    bool value,
    std::string_view serializeOnOutgoingRequestsVersion)
    : _flagName(std::string{flagName}), _phase(phase), _value(value) {
    if (!serializeOnOutgoingRequestsVersion.empty()) {
        _serializeOnOutgoingRequestsVersion =
            FeatureCompatibilityVersionParser::parseVersionForFeatureFlags(
                serializeOnOutgoingRequestsVersion);
    }
}


const std::vector<IncrementalRolloutFeatureFlag*>&
IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS() {
    // Populated once by the CacheIfrFlagsIntroducedSinceLastLTS MONGO_INITIALIZER below.
    return getMutableFlagsIntroducedSinceLastLTS();
}

// (Generic FCV reference): This is a receiver-side conservative default. We do not gate by the
// running FCV here — callers decide whether the defaulting should apply. Populated after all
// IFR flags have registered via static init but before any request processing runs.
MONGO_INITIALIZER(CacheIfrFlagsIntroducedSinceLastLTS)(InitializerContext*) {
    auto& sinceLastLTSCache = getMutableFlagsIntroducedSinceLastLTS();
    for (auto* flag : getMutableAllIncrementalRolloutFeatureFlags()) {
        if (flag->shouldSerializeOnOutgoingRequests()) {
            if (*flag->getSerializeOnOutgoingRequestsVersion() >
                multiversion::GenericFCV::kLastLTS) {
                sinceLastLTSCache.push_back(flag);
            }
        }
    }

    // Build the shard-server "no ifrFlags" default context now that the flag list above is
    // populated, so 'installForRequestWithoutIfrFlags()' can clone it instead of rebuilding it on
    // every request.
    IncrementalFeatureRolloutContext::initShardServerDefaultTemplate();
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

void IncrementalRolloutFeatureFlag::appendFlagsStats(BSONArrayBuilder& flagStats) {
    for (auto* flag : getMutableAllIncrementalRolloutFeatureFlags()) {
        flag->appendFlagStats(flagStats);
    }
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
            case RolloutPhase::release:
                return "release";
        }
        MONGO_UNREACHABLE_TASSERT(101023);
    }();
    detailsBuilder.append("incrementalFeatureRolloutPhase", phaseName);
    if (_serializeOnOutgoingRequestsVersion) {
        detailsBuilder.append("serializeOnOutgoingRequestsVersion",
                              FeatureCompatibilityVersionParser::serializeVersionForFeatureFlags(
                                  *_serializeOnOutgoingRequestsVersion));
    }
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

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::get(
    OperationContext* opCtx) {
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    // A nested DBDirectClient op (e.g. view resolution, or the auth user-cache lookup) can run on
    // the parent's opCtx before the parent installs its wire IFR context. The context installs
    // once, so materializing it here with local defaults would make the parent's wire install a
    // no-op and silently drop a value the router set -- e.g. a flag it disabled after a kickback.
    // Hand nested ops a detached context instead; once the parent installs, nested ops inherit it.
    if (!deferred.isInitialized() && opCtx->getClient()->isInDirectClient()) {
        return std::make_shared<IncrementalFeatureRolloutContext>();
    }
    // Forces materialization of whatever initializer is present (the default empty context, or one
    // an install path assigned).
    return *deferred;
}

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::tryGet(
    OperationContext* opCtx) {
    // Never forces; an unmaterialized context is treated as absent.
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    return deferred.isInitialized() ? *deferred : nullptr;
}

// static
bool IncrementalFeatureRolloutContext::isInstalled(OperationContext* opCtx) {
    return getIfrContextOnOpCtx(opCtx).deferred.isInitialized();
}

// static
void IncrementalFeatureRolloutContext::set(OperationContext* opCtx,
                                           std::shared_ptr<IncrementalFeatureRolloutContext> ctx) {
    auto& deferred = getIfrContextOnOpCtx(opCtx).deferred;
    // Install-once invariant: replacing a context that already cached its egress serialization
    // would silently drop that resolved state. Only a materialized context can have cached it.
    if (deferred.isInitialized()) {
        tassert(13002310,
                "Refusing to replace an IFRContext whose egress metadata has already been cached",
                !(*deferred)->hasCachedEgressMetadataForTest());
    }
    // Eager install: value is materialized immediately, so it is visible to tryGet()/isInstalled()
    // and serialized on egress.
    deferred = DeferredIfrContext(std::move(ctx));
}

// static
IncrementalFeatureRolloutContext::IncrementalFeatureRolloutContext() = default;
IncrementalFeatureRolloutContext::~IncrementalFeatureRolloutContext() = default;

std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::fromWire(
    std::span<const BSONObj> flags, std::unique_ptr<IFRSenderVersion> senderVersion) {
    return std::shared_ptr<IncrementalFeatureRolloutContext>(new IncrementalFeatureRolloutContext(
        flags,
        senderVersion ? std::move(senderVersion)
                      : std::make_unique<IFRSenderVersion>(toFullVersion(
                            multiversion::FeatureCompatibilityVersion::kVersion_8_3))));
}

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::fromWireForTest(
    std::span<const BSONObj> flags) {
    // (Generic FCV Reference): For Testing.
    return std::shared_ptr<IncrementalFeatureRolloutContext>(new IncrementalFeatureRolloutContext(
        flags, std::make_unique<IFRSenderVersion>(localSenderVersion())));
}

// static
std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::forTest(
    std::span<const BSONObj> flags) {
    return std::shared_ptr<IncrementalFeatureRolloutContext>(
        new IncrementalFeatureRolloutContext(flags));
}

std::shared_ptr<IncrementalFeatureRolloutContext> IncrementalFeatureRolloutContext::clone() const {
    auto copy = std::make_shared<IncrementalFeatureRolloutContext>();
    copy->_savedFlagValues = _savedFlagValues;
    if (_senderVersion) {
        copy->_senderVersion = std::make_unique<IFRSenderVersion>(*_senderVersion);
    }
    // Intentionally do not copy '_cachedEgressMetadata': a freshly cloned context must start
    // un-memoized so it can be installed on an opCtx (see the tassert in 'set()').
    return copy;
}

// static
IncrementalFeatureRolloutContext&
IncrementalFeatureRolloutContext::mutableShardServerDefaultTemplate() {
    // Default-constructed empty here; 'initShardServerDefaultTemplate()' fills it during startup.
    static IncrementalFeatureRolloutContext instance;
    return instance;
}

// static
void IncrementalFeatureRolloutContext::initShardServerDefaultTemplate() {
    auto& tmpl = mutableShardServerDefaultTemplate();
    const auto& flags = IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS();
    tmpl._savedFlagValues.clear();
    tmpl._savedFlagValues.reserve(flags.size());
    for (auto* flag : flags) {
        tmpl._savedFlagValues[flag] = false;
    }
}

// static
void IncrementalFeatureRolloutContext::installForRequestWithoutIfrFlags(OperationContext* opCtx) {

    // A shard server that receives no ifrFlags got the request from a pre-9.0 router, an old shard,
    // or a background thread — none of which coordinated a value — should conservatively disable
    // every release flag introduced at kLatest to keep shards from turning a feature on before
    // their siblings do. Any shard might be the first upgraded in the deployment, so we need to
    // wait for a signal that other shards are ready to turn on a new behavior across the whole
    // cluster. This will come when the routing layer is upgraded.
    //
    // This is deliberately FCV-independent, and in balance with a pure replica set deployment. In
    // a replica set deployment we expect requests from clients to come in soon after upgrading
    // (before setFCV command) to use the new defaults of the feature flags. So in this case the
    // lack of ifrFlags on the request indicates we should use whatever the current settings are on
    // this node.
    //
    // We discriminate between the two based on whether we have the shard server role.
    //
    // This isn't great, but it's a stopgap. In the v9.1+ future, a "pre-9.0 router" won't exist,
    // and we will expect all incoming requests to have their flags specified. Then we can remove
    // this condition and have no fear using the local settings if we don't hear otherwise.
    const auto clusterRole = serverGlobalParams.clusterRole;
    if (clusterRole.has(ClusterRole::ShardServer)) {
        // Defer cloning the all-flags-off template until a flag is actually consulted. If nothing
        // consults one, the context stays unmaterialized and no ifrFlags are forwarded downstream.
        getIfrContextOnOpCtx(opCtx).deferred =
            DeferredIfrContext([] { return mutableShardServerDefaultTemplate().clone(); });
    } else if (clusterRole.hasExclusively(ClusterRole::RouterServer)) {
        // Avoid deferring initialization on mongos - we want to eagerly initialize to make sure
        // everyone's on the same page.
        set(opCtx, std::make_shared<IncrementalFeatureRolloutContext>());
    }
}

// Constructor for IFRContext sent from wire.
IncrementalFeatureRolloutContext::IncrementalFeatureRolloutContext(
    std::span<const BSONObj> flags, std::unique_ptr<IFRSenderVersion> senderVersion)
    : _senderVersion(std::move(senderVersion)) {
    tassert(13013608,
            "Expected sender version to be resolved by this point",
            _senderVersion != nullptr);

    // Track which active flags arrived so the post-loop can fill in the absent ones (scenario
    // 4).
    absl::flat_hash_set<const IncrementalRolloutFeatureFlag*> receivedFlags;

    // Collect all scenario-3 (genuinely unrecognized) flags so we can report them together.
    // Keyed by name so a payload that repeats an unknown flag collapses to a single entry.
    UnrecognizedIFRFlagInfo::FlagMap unknownFlags;

    for (const auto& flagObj : flags) {
        const auto& nameElem = flagObj["name"];
        uassert(
            11565102, "Expected 'name' field to be a string", nameElem.type() == BSONType::string);

        const auto& valueElem = flagObj["value"];
        uassert(11565103,
                "Expected 'value' field to be a boolean",
                valueElem.type() == BSONType::boolean);

        const auto flagName = nameElem.valueStringData();
        auto* flag = IncrementalRolloutFeatureFlag::findByName(flagName);

        if (flag != nullptr) {
            // Scenario 1: recognized flag — store the sender's value. A flag appearing twice in one
            // payload is a malformed request from the sender; treat it as a protocol error.
            _savedFlagValues[flag] = valueElem.boolean();
            tassert(13024005,
                    str::stream() << "Sender specified IFR flag '" << flagName
                                  << "' more than once",
                    receivedFlags.insert(flag).second);
            // TODO SERVER-130479 Missing a possible scenario: flag was removed in an earlier
            // version.
        } else if (*_senderVersion <= localSenderVersion()) {
            // Scenario 3: flag is genuinely unknown and the sender is no newer than this binary
            // (compared at FCV granularity; patch-level precision is deferred with the
            // flag-introduction-granularity work). A same-series-or-older sender should never
            // produce a flag we don't recognize; treat it as a protocol error. Accumulate all
            // such flags so they can be reported together. As with scenario 1, a repeated flag is a
            // malformed request.
            tassert(13024006,
                    str::stream() << "Sender specified IFR flag '" << flagName
                                  << "' more than once",
                    unknownFlags.emplace(std::string(flagName), valueElem.boolean()).second);
        } else {
            // Sender is newer than this binary (or no version info was provided). A flag unknown to
            // us was added after our binary — silently drop with a rate-limited log so
            // rolling-upgrade traffic stays quiet while misconfigurations are still visible.
            static logv2::SeveritySuppressor suppressor(
                Seconds{10}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2));
            if (auto sev = suppressor(); shouldLog(MONGO_LOGV2_DEFAULT_COMPONENT, sev)) {
                LOGV2_DEBUG(13002301,
                            sev.toInt(),
                            "Dropped unrecognized IFR flag from wire",
                            "flagName"_attr = flagName);
            }
        }
    }

    // Scenario 3 (deferred tripwire): a same-or-older sender should never produce a flag we don't
    // recognize, so treat it as a programmer error. Report all such flags together, attaching the
    // structured UnrecognizedIFRFlagInfo so callers can inspect the full set without re-parsing.
    if (!unknownFlags.empty()) {
        tasserted(makeUnrecognizedIFRFlagStatus(std::move(unknownFlags), *_senderVersion));
    }

    // Scenario 4: eagerly resolve active flags that were absent from the payload.
    // A flag not sent by the sender is either new to our version (sender predates it) or was
    // removed/promoted in a newer version the sender is running.
    for (auto* flag : IncrementalRolloutFeatureFlag::getFlagsIntroducedSinceLastLTS()) {
        if (_savedFlagValues.contains(flag)) {
            continue;
        }
        const auto flagIntroVersion = flag->getSerializeOnOutgoingRequestsVersion();
        tassert(
            13013607,
            "Expected getFlagsIntroducedSinceLastLTS to return only flags with 'flagIntroVersion' "
            "specified",
            flagIntroVersion);
        if (*_senderVersion < toFullVersion(*flagIntroVersion)) {
            // Sender predates this flag's introduction — conservative false.
            _savedFlagValues[flag] = false;
        } else {
            // Sender is same/newer (or no version info) — use this binary's local default.
            _savedFlagValues[flag] = flag->checkEnabled();
        }
    }
}

IncrementalFeatureRolloutContext::IncrementalFeatureRolloutContext(std::span<const BSONObj> flags) {
    for (const auto& flagObj : flags) {
        const auto& nameElem = flagObj["name"];
        uassert(
            11565104, "Expected 'name' field to be a string", nameElem.type() == BSONType::string);

        const auto& valueElem = flagObj["value"];
        uassert(11565105,
                "Expected 'value' field to be a boolean",
                valueElem.type() == BSONType::boolean);

        auto* flag = IncrementalRolloutFeatureFlag::findByName(nameElem.valueStringData());
        tassert(11565106,
                str::stream() << "Unknown IFR flag name in test: " << nameElem.valueStringData(),
                flag != nullptr);
        _savedFlagValues[flag] = valueElem.boolean();
    }
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

void IncrementalFeatureRolloutContext::disableFlag(IncrementalRolloutFeatureFlag& flag) {
    _savedFlagValues.insert_or_assign(&flag, false);
    _cachedEgressMetadata.reset();
}

void IncrementalFeatureRolloutContext::appendToEgressMetadata(BSONObjBuilder* bob) {
    // Memoize on first call: the same IFRContext may be consulted more than once during dispatch
    // (e.g. per-shard restamping on retry). Since IFRContext is per-opCtx we can skip locking.
    if (!_cachedEgressMetadata) {
        BSONObjBuilder subBuilder;

        const auto& outgoingSenderVersion = _senderVersion ? *_senderVersion : localSenderVersion();

        if (outgoingSenderVersion >
            toFullVersion(multiversion::FeatureCompatibilityVersion::kVersion_8_3)) {
            // The sender version.
            BSONObjBuilder senderVersionBuilder(
                subBuilder.subobjStart(GenericArguments::kIfrSenderVersionFieldName));
            if (isInstalledFromWire()) {
                _senderVersion->serialize(&senderVersionBuilder);
            } else {
                localSenderVersion().serialize(&senderVersionBuilder);
            }
            senderVersionBuilder.doneFast();
        }

        {
            // The flags.
            BSONArrayBuilder arr(subBuilder.subarrayStart(GenericArguments::kIfrFlagsFieldName));
            for (auto* flag : getMutableAllIncrementalRolloutFeatureFlags()) {
                if (const auto versionIntroduced = flag->getSerializeOnOutgoingRequestsVersion()) {
                    if (toFullVersion(*versionIntroduced) <= outgoingSenderVersion) {
                        arr << (BSONObjBuilder{}
                                    .append("name", flag->getName())
                                    .append("value", getSavedFlagValue(*flag))
                                    .obj());
                    }
                }
            }
        }

        _cachedEgressMetadata = subBuilder.obj();
    }

    // Append cached fields to the caller's builder.
    for (const auto& elem : *_cachedEgressMetadata) {
        bob->append(elem);
    }
}

}  // namespace mongo
