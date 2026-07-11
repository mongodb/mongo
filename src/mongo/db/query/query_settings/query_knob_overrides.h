// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/util/version/releases.h"

#include <span>
#include <string_view>
#include <vector>

namespace mongo::query_settings {

/**
 * Pre-parsed representation of per-query knob overrides stored in QuerySettings.
 *
 * Each entry is a (registry-index, value) pair produced by a single fromBSON() call.
 * An empty entries vector means no overrides are active.
 *
 * DeleteQueryKnobOverride is a write-path-only removal sentinel used to signal that a knob should
 * be removed during merge. It must not survive into stored settings; validateQuerySettings()
 * tasserts its absence after simplification.
 */
class QuerySettingsKnobOverrides {
public:
    struct Entry {
        QueryKnobId id;
        QueryKnobValue value;

        // Compare by id, then value.
        auto operator<=>(const Entry&) const = default;
    };

    static QuerySettingsKnobOverrides fromBSON(const BSONObj& obj);
    static QuerySettingsKnobOverrides fromBSON(const BSONElement& element) {
        return fromBSON(element.Obj());
    }

    /**
     * Applies 'rhs' as a per-knob patch onto 'lhs' (a sorted union where 'rhs' wins on equal ids).
     * 'lhs' is expected to be sentinel-free (it is always already-stored or already-simplified
     * settings); sentinels legitimately originate only in the 'rhs' wire patch. Callers must run
     * simplify() on the result before passing it to validateQuerySettings().
     */
    static QuerySettingsKnobOverrides merge(const QuerySettingsKnobOverrides& lhs,
                                            const QuerySettingsKnobOverrides& rhs);

    /**
     * Removes DeleteQueryKnobOverride removal sentinels from the entries. Run on merged settings
     * before calling validateQuerySettings() to ensure no sentinel survives into stored settings.
     */
    void simplify();

    /**
     * Removes all overrides for knobs whose 'minFcv' is greater than 'fcv', i.e. knobs not
     * supported on that FCV. Returns true if any entry was removed.
     */
    bool removeKnobsRequiringHigherFcv(multiversion::FeatureCompatibilityVersion fcv);

    BSONObj toBSON() const;
    void toBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
        builder->append(fieldName, toBSON());
    }

    bool empty() const {
        return _entries.empty();
    }

    std::span<const Entry> entries() const {
        return _entries;
    }

    auto operator<=>(const QuerySettingsKnobOverrides&) const = default;

private:
    std::vector<Entry> _entries;
};

}  // namespace mongo::query_settings
