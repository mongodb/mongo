// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_knobs/query_knob_configuration.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knob_descriptors_optimization.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_lifespan.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/version_context.h"
#include "mongo/util/str.h"

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

auto decoration = QueryLifespan::declareOpCtxDecoration<boost::optional<QueryKnobConfiguration>>();

// Source labels emitted for each knob in 'serializeForExplain'.
constexpr std::string_view kSetParameterSource = "setParameter"sv;
constexpr std::string_view kQuerySettingsSource = "querySettings"sv;

/**
 * Returns a new snapshot initialized from the current global knob values, with any supported
 * per-query QuerySettings overrides applied on top.
 */
QueryKnobSnapshot makeQueryKnobSnapshot(const query_settings::QuerySettings& querySettings) {
    auto snapshot = QueryKnobSnapshotCache::instance().getThreadLocalSnapshot();
    const bool hasOverrides = querySettings.getQueryKnobs() || querySettings.getQueryFramework();
    if (!hasOverrides) {
        return snapshot;
    }
    QueryKnobSnapshotBuilder builder(std::move(snapshot));
    if (auto&& knobs = querySettings.getQueryKnobs()) {
        for (auto&& [id, value] : knobs->entries()) {
            builder.set(id, value, KnobSource::kQuerySettings);
        }
    }
    // TODO SERVER-129207 Migrate query framework control from QuerySettings to QueryKnobs and
    // remove this special case.
    if (auto&& framework = querySettings.getQueryFramework()) {
        builder.set(query_knobs::kQueryFrameworkControl.id,
                    QueryKnobValue(static_cast<int>(*framework)),
                    KnobSource::kQuerySettings);
    }
    return std::move(builder).build();
}
}  // namespace

QueryKnobConfiguration::QueryKnobConfiguration(const query_settings::QuerySettings& querySettings)
    : _snapshot(makeQueryKnobSnapshot(querySettings)),
      _overrideResult(query_settings::KnobOverrideResult::kApplied) {}

QueryKnobConfiguration::QueryKnobConfiguration()
    : _snapshot(QueryKnobSnapshotCache::instance().getThreadLocalSnapshot()) {}

const QueryKnobConfiguration& QueryKnobConfiguration::get(OperationContext* opCtx) {
    auto&& instance = decoration(opCtx);
    // Initialize a new QueryKnobConfiguration if needed.
    if (!instance) {
        instance = QueryKnobConfiguration();
    }

    // Early exit if the settings have already been installed.
    if (instance->_overrideResult == query_settings::KnobOverrideResult::kApplied) {
        return *instance;
    }

    // Try to install the query settings if available, or leave it for the next get() call.
    instance->_overrideResult =
        query_settings::tryOverrideQueryKnobValues(opCtx, instance->_snapshot);
    return *instance;
}

void QueryKnobConfiguration::reset_forTest(OperationContext* opCtx) {
    decoration(opCtx).reset();
}

bool QueryKnobConfiguration::_isKnobReadAllowed(QueryKnobId id) const {
    // Any read is allowed once the overrides are applied, or before the query starts (eligibility
    // not yet known). Only the pending window, where settings may still override a knob, restricts
    // reads to non-PQS-settable knobs.
    if (_overrideResult != query_settings::KnobOverrideResult::kPending) {
        return true;
    }
    // TODO SERVER-129207 The query framework control knob is not marked 'pqs_settable', yet
    // query settings may override it via 'queryFramework'. Treat it as overridable until it is
    // migrated to QueryKnobs.
    if (id == query_knobs::kQueryFrameworkControl.id) {
        return false;
    }
    return !QueryKnobRegistry::instance().entry(id).pqsSettable;
}

std::string QueryKnobConfiguration::_makeForbiddenKnobReadMsg(QueryKnobId id) const {
    const auto& entry = QueryKnobRegistry::instance().entry(id);
    return str::stream() << "PQS-settable query knob '" << entry.wireName
                         << "' read while query settings are pending resolution";
}

BSONObj QueryKnobConfiguration::serializeForExplain() const {
    BSONObjBuilder bob;
    auto append = [&](const auto& entry, std::string_view source) {
        BSONObjBuilder sub(bob.subobjStart(entry.wireName));
        entry.toBSON(sub, "value"sv, _snapshot.getValue(entry.id));
        sub.append("source"sv, source);
    };
    for (const auto& entry : QueryKnobRegistry::instance().entries()) {
        switch (_snapshot.getSource(entry.id)) {
            case KnobSource::kSetParameter:
                append(entry, kSetParameterSource);
                break;
            case KnobSource::kQuerySettings:
                append(entry, kQuerySettingsSource);
                break;
            case KnobSource::kDefault:
                // Don't include default-valued knobs in the explain output to reduce noise.
                break;
        }
    }
    return bob.obj();
}

void QueryKnobConfiguration::addToSlowLog(OperationContext* opCtx,
                                          logv2::DynamicAttributes& attrs) const {
    if (!feature_flags::gFeatureFlagPqsQueryKnobs.isEnabledUseLatestFCVWhenUninitialized(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return;
    }
    if (auto serializedKnobs = serializeForExplain(); !serializedKnobs.isEmpty()) {
        attrs.add("queryKnobs", serializedKnobs);
    }
}

// The remaining typed knob accessors are generated from the knob tables by the AccessorMixin base
// classes (see query_knob.h and query_knob_descriptors_{optimization,execution}.h). Only the
// accessors below compute over a knob value rather than returning it directly.

bool QueryKnobConfiguration::isForceClassicEngineEnabled() const {
    return get(query_knobs::kQueryFrameworkControl) ==
        QueryFrameworkControlEnum::kForceClassicEngine;
}

bool QueryKnobConfiguration::canPushDownFullyCompatibleStages() const {
    switch (get(query_knobs::kQueryFrameworkControl)) {
        case QueryFrameworkControlEnum::kForceClassicEngine:
        case QueryFrameworkControlEnum::kTrySbeRestricted:
            return false;
        case QueryFrameworkControlEnum::kTrySbeEngine:
            return true;
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
