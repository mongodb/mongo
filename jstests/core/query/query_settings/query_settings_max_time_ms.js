/**
 * Tests that the 'maxTimeMS' query setting is stored, retrieved, reflected in explain output, and
 * enforced correctly:
 *   - QS maxTimeMS causes MaxTimeMSExpired.
 *   - A loose QS maxTimeMS overrides a user-supplied tight maxTimeMS.
 *   - A tight QS maxTimeMS overrides a user-supplied loose maxTimeMS.
 *   - Queries of a different shape are not affected.
 *
 * Enforcement is tested deterministically, without relying on a real sleeping query or wall-clock
 * timing races: every enforcement query is issued as an aggregate with a trailing
 * '$_internalInhibitOptimization' stage, which prevents the query optimizer from collapsing the
 * pipeline away (see 'canOptimizeAwayPipeline()' in run_aggregate.cpp) and so guarantees a real
 * 'DocumentSourceCursor' is constructed. The 'hangBeforeDocumentSourceCursorLoadBatch' fail point
 * then blocks that cursor indefinitely (looping until turned off or interrupted), so the operation
 * only ever stops once its maxTimeMS deadline actually expires. We then read the operation's real
 * elapsed 'durationMillis' back from the server's slow query log to determine which of the two
 * competing maxTimeMS values actually governed. Each query is tagged with a unique 'comment',
 * which is also passed to the fail point (so only that specific query is blocked) and used to pick
 * out its slow query log line unambiguously. Both the fail point and the slow-query-log read are
 * applied to every discovered node (not just 'db'), so this works regardless of which node
 * actually executes the query — including under random mongos dispatch or secondary-read
 * redirection.
 *
 * @tags: [
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *   transitioning_replicaset_incompatible,
 *   does_not_support_stepdowns,
 *   # 'maxTimeMS' query settings are gated behind their own dedicated PQS feature flag.
 *   featureFlagPqsMaxTimeMS,
 *   requires_fcv_90,
 *   # TODO SERVER-130738: View passthrough transparently redirects find/aggregate/count/distinct
 *   # commands to a differently-named underlying namespace (e.g. implicit_identity_views.js), but
 *   # do NOT redirect setQuerySettings/removeQuerySettings the same way. Since query settings
 *   # resolve by query-shape hash (which includes the namespace), the settings installed here and
 *   # the redirected query end up with different shape hashes, so the settings silently never
 *   # apply and the enforcement queries below hang forever instead of hitting MaxTimeMSExpired.
 *   incompatible_with_views,
 *   # The enforcement cases arm the 'hangBeforeDocumentSourceCursorLoadBatch' fail point on every
 *   # node and open a fresh connection to each for arming/disarming and slow-query-log reads. Under
 *   # a concurrently running balancer, the test collection is continuously resharded, which
 *   # saturates the nodes and makes this setup/teardown (in particular the fail point disarm) hang,
 *   # timing out the suite.
 *   assumes_balancer_off,
 *   needs_query_settings_user_role,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());
qsutils.removeAllQuerySettings();
assert.commandWorked(coll.insert({a: 1}));

describe("maxTimeMS query setting", function () {
    // 'maxTimeMS' is a safeInt64 and round-trips as a NumberLong.
    const settings = {maxTimeMS: NumberLong(5000)};
    const kTestCases = {
        find: qsutils.makeFindQueryInstance({filter: {a: 1}}),
        aggregate: qsutils.makeAggregateQueryInstance({pipeline: [{$match: {a: 1}}]}),
        distinct: qsutils.makeDistinctQueryInstance({key: "a", query: {a: 1}}),
    };

    for (const [queryType, representativeQuery] of Object.entries(kTestCases)) {
        it(`is stored and retrieved for ${queryType}`, function () {
            qsutils.withQuerySettings(representativeQuery, settings, () => {
                qsutils.assertQueryShapeConfiguration([
                    qsutils.makeQueryShapeConfiguration(settings, representativeQuery),
                ]);
            });
        });

        it(`appears in explain output for ${queryType}`, function () {
            qsutils.withQuerySettings(representativeQuery, settings, () => {
                qsutils.assertExplainQuerySettings(representativeQuery, settings);
            });
        });
    }

    it("rejects maxTimeMS: 0 as the only setting, since it normalizes to empty", function () {
        // 'maxTimeMS: 0' is normalized to unset, so setting it alone leaves nothing to store,
        // just like {reject: false} alone.
        const query = qsutils.makeFindQueryInstance({filter: {a: 1}});
        assert.commandFailedWithCode(
            db.adminCommand({setQuerySettings: query, settings: {maxTimeMS: NumberLong(0)}}),
            7746604,
        );
    });
});

describe("maxTimeMS query setting enforcement", function () {
    // The trailing '$_internalInhibitOptimization' stage prevents the query optimizer from
    // collapsing this pipeline away (which would otherwise happen since it's just a single
    // leading $match), guaranteeing a real 'DocumentSourceCursor' — and thus a reachable
    // 'hangBeforeDocumentSourceCursorLoadBatch' fail point — even when transparently redirected
    // through a view or timeseries collection.
    function makeAggCmd(filter) {
        return {
            aggregate: coll.getName(),
            pipeline: [{$match: filter}, {$_internalInhibitOptimization: {}}],
            cursor: {},
        };
    }
    const aggCmd = {...makeAggCmd({a: 1}), $db: db.getName()};

    // The "Slow query" log line (read back by getLastDurationMillis() below) is only emitted for
    // operations slower than the default 'slowms' threshold (100ms), so both values here are kept
    // comfortably above it to avoid depending on server profiling settings.
    const kTightMaxTimeMS = 200;
    const kLooseMaxTimeMS = 2000;

    // Upper bound for asserting that the *tight* value governed: generous enough to absorb
    // scheduling/log-flush slop, but well below kLooseMaxTimeMS, so a duration under this bound
    // can only be explained by the tight deadline actually winning (as opposed to merely not
    // being as slow as the loose one).
    const kTightUpperBoundMS = kTightMaxTimeMS + 1000;

    // Blocks the query tagged with 'comment' indefinitely, until either 'fp.off()' is called or
    // the operation is interrupted (e.g. because its maxTimeMS deadline expired). Arming on every
    // shard and mongos means this works regardless of which node actually executes the query.
    function hangMatchingQuery(comment) {
        const data = {shouldCheckForInterrupt: true, comment, nss: coll.getFullName()};
        configureFailPointForAllShardsAndMongos({
            conn: db.getMongo(),
            failPointName: "hangBeforeDocumentSourceCursorLoadBatch",
            data,
            failPointMode: "alwaysOn",
        });
        return {
            off: () =>
                configureFailPointForAllShardsAndMongos({
                    conn: db.getMongo(),
                    failPointName: "hangBeforeDocumentSourceCursorLoadBatch",
                    data: {},
                    failPointMode: "off",
                }),
        };
    }

    // Returns the 'durationMillis' of the most recently logged slow query tagged with 'comment'
    // that timed out due to maxTimeMS expiry. Reads every discovered node's own log (rather than
    // just 'db''s) since the query may have actually executed on a different node than 'db' is
    // connected to (e.g. under random mongos dispatch or secondary-read redirection); each node
    // is queried through a fresh, direct connection so this doesn't depend on 'db' targeting any
    // particular node.
    function getLastDurationMillis(comment) {
        const slowQueryLogId = 51803; // ID for 'Slow query' log messages.
        const hosts = DiscoverTopology.findNonConfigNodes(db.getMongo());
        const messages = hosts
            .flatMap(
                (host) =>
                    checkLog.getFilteredLogMessages(new Mongo(host), slowQueryLogId, {}) || [],
            )
            .filter(
                (message) =>
                    message.attr.command?.comment === comment &&
                    message.attr.errName === "MaxTimeMSExpired",
            );
        assert.gt(messages.length, 0, "expected at least one slow query log line", {messages});
        return messages[messages.length - 1].attr.durationMillis;
    }

    it("QS maxTimeMS causes MaxTimeMSExpired", function () {
        const comment = UUID().toString();
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kTightMaxTimeMS}, () => {
            const fp = hangMatchingQuery(comment);
            try {
                assert.commandFailedWithCode(
                    db.runCommand({...makeAggCmd({a: 1}), comment}),
                    ErrorCodes.MaxTimeMSExpired,
                );
            } finally {
                fp.off();
            }
        });
    });

    it("QS maxTimeMS does not affect queries of a different shape", function () {
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kTightMaxTimeMS}, () => {
            // A query with a different filter (different shape) should not be timed out. No fail
            // point is used here: this query is never expected to block on anything.
            assert.commandWorked(db.runCommand(makeAggCmd({b: 1})));
        });
    });

    it("QS maxTimeMS loosens a user-supplied tight maxTimeMS", function () {
        const comment = UUID().toString();
        // QS sets a loose (winning) deadline; the user supplies a tight (losing) one.
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kLooseMaxTimeMS}, () => {
            const fp = hangMatchingQuery(comment);
            try {
                assert.commandFailedWithCode(
                    db.runCommand({
                        ...makeAggCmd({a: 1}),
                        maxTimeMS: kTightMaxTimeMS,
                        comment,
                    }),
                    ErrorCodes.MaxTimeMSExpired,
                );
                // Had the tight user value won, this would have run for ~kTightMaxTimeMS, not
                // ~kLooseMaxTimeMS.
                assert.gte(getLastDurationMillis(comment), kLooseMaxTimeMS);
            } finally {
                fp.off();
            }
        });
    });

    it("QS maxTimeMS tightens a user-supplied loose maxTimeMS", function () {
        const comment = UUID().toString();
        // QS sets a tight (winning) deadline; the user supplies a loose (losing) one.
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kTightMaxTimeMS}, () => {
            const fp = hangMatchingQuery(comment);
            try {
                assert.commandFailedWithCode(
                    db.runCommand({
                        ...makeAggCmd({a: 1}),
                        maxTimeMS: kLooseMaxTimeMS,
                        comment,
                    }),
                    ErrorCodes.MaxTimeMSExpired,
                );
                // Had the loose user value won, this would have run for ~kLooseMaxTimeMS, not
                // ~kTightMaxTimeMS. Asserting against kTightUpperBoundMS (rather than just
                // "< kLooseMaxTimeMS") proves the tight value actually governed, not merely that
                // the loose one didn't.
                assert.lt(getLastDurationMillis(comment), kTightUpperBoundMS);
            } finally {
                fp.off();
            }
        });
    });

    it("QS maxTimeMS loosens a user-supplied tight maxTimeMS via client-side querySettings", function () {
        if (!FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "AllowUserFacingQuerySettings")) {
            return;
        }
        const comment = UUID().toString();
        // Cluster PQS sets a loose (winning) deadline; the client-side querySettings field
        // supplies a tight (losing) one.
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kLooseMaxTimeMS}, () => {
            const fp = hangMatchingQuery(comment);
            try {
                assert.commandFailedWithCode(
                    db.runCommand({
                        ...makeAggCmd({a: 1}),
                        querySettings: {maxTimeMS: NumberLong(kTightMaxTimeMS)},
                        comment,
                    }),
                    ErrorCodes.MaxTimeMSExpired,
                );
                // Had the tight client-side value won, this would have run for
                // ~kTightMaxTimeMS, not ~kLooseMaxTimeMS.
                assert.gte(getLastDurationMillis(comment), kLooseMaxTimeMS);
            } finally {
                fp.off();
            }
        });
    });

    it("QS maxTimeMS tightens a user-supplied loose maxTimeMS via client-side querySettings", function () {
        if (!FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), "AllowUserFacingQuerySettings")) {
            return;
        }
        const comment = UUID().toString();
        // Cluster PQS sets a tight (winning) deadline; the client-side querySettings field
        // supplies a loose (losing) one.
        qsutils.withQuerySettings(aggCmd, {maxTimeMS: kTightMaxTimeMS}, () => {
            const fp = hangMatchingQuery(comment);
            try {
                assert.commandFailedWithCode(
                    db.runCommand({
                        ...makeAggCmd({a: 1}),
                        querySettings: {maxTimeMS: NumberLong(kLooseMaxTimeMS)},
                        comment,
                    }),
                    ErrorCodes.MaxTimeMSExpired,
                );
                // Had the loose client-side value won, this would have run for
                // ~kLooseMaxTimeMS, not ~kTightMaxTimeMS. Asserting against kTightUpperBoundMS
                // (rather than just "< kLooseMaxTimeMS") proves the tight value actually
                // governed, not merely that the loose one didn't.
                assert.lt(getLastDurationMillis(comment), kTightUpperBoundMS);
            } finally {
                fp.off();
            }
        });
    });
});
