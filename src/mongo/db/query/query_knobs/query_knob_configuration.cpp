/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_knobs/query_knob_configuration.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knob_descriptors_optimization.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_registry.h"
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"

namespace mongo {
using namespace std::literals::string_view_literals;

namespace {

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
    : _snapshot(makeQueryKnobSnapshot(querySettings)) {}

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
