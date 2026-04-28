/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/query/query_knob.h"
#include "mongo/db/server_parameter.h"
#include "mongo/util/string_map.h"
#include "mongo/util/version/releases.h"

#include <cstddef>
#include <vector>

#include <boost/optional.hpp>

namespace mongo {

namespace detail {
class QueryKnobRegistryBuilder;
}  // namespace detail

/**
 * Process-wide registry of query knobs. Built once at startup and immutable
 * thereafter. Indexed by a dense id for the hot read path, and by wire name
 * for the rare setQuerySettings write path.
 */
class QueryKnobRegistry {
public:
    struct Entry {
        QueryKnobBase& knob;
        ServerParameter& param;
        // Non-owning, points into `param`'s annotation BSON (static lifetime).
        StringData wireName;
        bool pqsSettable;
        // Present for PQS knobs only.
        boost::optional<multiversion::FeatureCompatibilityVersion> minFcv;
    };

    QueryKnobRegistry() = default;

    static const QueryKnobRegistry& instance();

    /**
     * Installs `reg` as the process instance and writes a dense index back into each entry's
     * QueryKnobBase descriptor. Call once at startup; invariants on a second call.
     */
    static void init(QueryKnobRegistry reg);

    /**
     * PQS-settable knobs only; boost::none for non-PQS or unknown name.
     */
    boost::optional<size_t> getKnobIdForName(StringData wireName) const;

    const Entry& entry(size_t id) const;

    size_t knobCount() const;
    size_t knobsExposedToQuerySettingsCount() const;

private:
    friend class detail::QueryKnobRegistryBuilder;

    explicit QueryKnobRegistry(std::vector<Entry> entries);

    static QueryKnobRegistry& _mutableInstance();

    void _writeBackDescriptorIndexes();

    std::vector<Entry> _entries;
    StringMap<size_t> _wireNameIndex;
    size_t _knobsExposedToQuerySettingsCount = 0;
    bool _initialized = false;
};

namespace detail {

/**
 * Builds a QueryKnobRegistry from QueryKnobDescriptorSet + ServerParameter
 * annotations. Exposed so tests can drive it directly; production code uses
 * QueryKnobRegistry::instance().
 */
class QueryKnobRegistryBuilder {
public:
    /**
     * Appends an entry. PQS entries are also indexed by wireName.
     */
    void addFromServerParameter(QueryKnobRegistry::Entry entry);

    /**
     * Parses the `query_knob` annotation on `param` and dispatches to the Entry overload.
     */
    void addFromServerParameter(QueryKnobBase& knob, ServerParameter* param);

    /**
     * Parses `queryKnobAnnotation` (the `query_knob` sub-object) directly and dispatches to the
     * Entry overload. Useful in tests that want to inject knob metadata without mutating the
     * ServerParameter.
     */
    void addFromServerParameter(QueryKnobBase& knob,
                                ServerParameter& param,
                                const BSONObj& queryKnobAnnotation);

    /**
     * Invariants if a ServerParameter has a query_knob annotation with no matching QueryKnob<T>.
     */
    void detectOrphanAnnotations(const ServerParameterSet& params) const;

    [[nodiscard]] QueryKnobRegistry build() &&;

private:
    std::vector<QueryKnobRegistry::Entry> _entries;
};

}  // namespace detail
}  // namespace mongo
