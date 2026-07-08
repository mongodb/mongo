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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/db/query/query_knob_descriptors_optimization.h"
#include "mongo/db/query/query_knobs/query_knob.h"
#include "mongo/db/query/query_knobs/query_knob_snapshot.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_settings/query_settings.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {
/**
 * A class for query-related knobs, it sets all the knob values on the first time a knob is accessed
 * and ensures the values are same though the whole lifetime of a query.
 */
class QueryKnobConfiguration
    : public query_knobs::AccessorMixinQueryOptimizationKnobs<QueryKnobConfiguration>,
      public query_knobs::AccessorMixinQueryExecutionKnobs<QueryKnobConfiguration> {
public:
    /**
     * NOTE: QueryKnobConfiguration construction requires 'querySettings', because settings may
     * override the query knob values.
     *
     * TODO SERVER-108400: Remove this constructor.
     */
    QueryKnobConfiguration(const query_settings::QuerySettings& querySettings);

    /**
     * Returns the QueryKnobConfiguration for 'opCtx'.
     */
    static const QueryKnobConfiguration& get(OperationContext* opCtx);

    /**
     * Clears the cached configuration for 'opCtx' so the next 'get()' rebuilds it from the current
     * global knob snapshot. Test-only: production keeps one configuration for an operation's whole
     * lifetime, but a test may need to observe a knob value toggled mid-operation.
     */
    static void reset_forTest(OperationContext* opCtx);

    /**
     * Read a knob value from the snapshot. T is deduced from the descriptor.
     */
    template <typename T>
    T get(const QueryKnob<T>& knob) const {
        dassert(_isKnobReadAllowed(knob.id));
        return _snapshot.get<T>(knob.id);
    }

    /**
     * Returns a BSON object containing every registered query knob with a non-default source,
     * keyed by wire name with nested "value" and "source" fields. Knobs overridden via
     * setParameter appear with source "setParameter"; knobs applied via QuerySettings appear with
     * source "querySettings" (even when their value equals the compiled-in default). Knobs at
     * KnobSource::kDefault are omitted. Returns an empty BSONObj when no knob has been overridden.
     */
    BSONObj serializeForExplain() const;

    /**
     * Adds the effective knob values to the slow query log 'attrs' under "queryKnobs", using the
     * same serialization as 'serializeForExplain()'. A no-op unless 'featureFlagPqsQueryKnobs' is
     * enabled and at least one knob has a non-default source.
     */
    void addToSlowLog(OperationContext* opCtx, logv2::DynamicAttributes& attrs) const;

    // Most typed knob accessors (getPlanRankerMode(), getMaxNodesInJoinGraph(),
    // getSbeDisableGroupPushdownForOp(), ...) are generated from the knob tables by the
    // AccessorMixin base classes. Only accessors that compute over a knob rather than returning it
    // directly are declared here.

    /**
     * Returns true if internal query framework control knob is set to 'forceClassicEngine', false
     * otherwise.
     */
    bool isForceClassicEngineEnabled() const;

    /**
     * Returns whether we can push down fully compatible stages to sbe. This is only true when the
     * query knob is 'trySbeEngine'.
     */
    bool canPushDownFullyCompatibleStages() const;

private:
    /**
     * Constructs a configuration from the current global knob values, with no per-query overrides
     * applied yet. Used when the configuration lives on the operation and overrides are applied
     * later via 'query_settings::tryOverrideQueryKnobValues()'.
     */
    QueryKnobConfiguration();

    /**
     * Returns true if 'id' may be read on this configuration. While query settings are pending
     * resolution, a PQS-settable knob must not be read, otherwise the caller would observe a value
     * that the settings are about to override. Reads before the query starts (eligibility not yet
     * known) and after the overrides are applied are unrestricted.
     */
    bool _isKnobReadAllowed(QueryKnobId id) const;

    QueryKnobSnapshot _snapshot;
    // Outcome of the last query-settings override attempt, refreshed on each 'get()' call until it
    // reaches 'kApplied'.
    query_settings::KnobOverrideResult _overrideResult =
        query_settings::KnobOverrideResult::kNotStarted;
};
}  // namespace mongo
