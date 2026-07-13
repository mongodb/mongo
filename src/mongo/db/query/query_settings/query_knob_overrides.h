// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
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
 * fromBSON() never throws: a knob that fails to parse or validate is simply omitted from
 * entries() and its failure is recorded in errors() instead, so a caller can decide whether to
 * reject (uassertNoErrors()) or degrade gracefully (log errors() and keep using entries()) based
 * on its own trust context. errors() is a parse-time diagnostic only; it is excluded from
 * equality comparisons and is never round-tripped through toBSON().
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
     * simplify() on the result before passing it to validateQuerySettings(). The result's errors()
     * is the concatenation of 'lhs' and 'rhs' errors().
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

    bool hasErrors() const {
        return !_errors.empty();
    }

    std::span<const Status> errors() const {
        return _errors;
    }

    /**
     * Throws the first recorded parse/validation error, if any. No-op if errors() is empty.
     */
    void uassertNoErrors() const;

    /**
     * Logs all recorded parse/validation errors() in a single log line, with extra context (e.g.
     * a query shape hash) merged in. No-op if errors() is empty.
     */
    void logErrors(const BSONObj& context) const;

    /**
     * Discards all recorded parse/validation errors(). Call once they've been reported (e.g. via
     * logErrors()) and no longer need to persist with a stored value.
     */
    void clearErrors() {
        _errors.clear();
    }

    friend auto operator<=>(const QuerySettingsKnobOverrides& lhs,
                            const QuerySettingsKnobOverrides& rhs) {
        return lhs._entries <=> rhs._entries;
    }
    friend bool operator==(const QuerySettingsKnobOverrides& lhs,
                           const QuerySettingsKnobOverrides& rhs) {
        return lhs._entries == rhs._entries;
    }

private:
    std::vector<Entry> _entries;
    std::vector<Status> _errors;
};

}  // namespace mongo::query_settings
