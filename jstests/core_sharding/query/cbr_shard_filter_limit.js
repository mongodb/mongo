/**
 * Regression test for SERVER-132567. A sharded query with a limit planned via CBR should not tassert.
 *
 * @tags: [
 *   uses_explain,
 *   # featureFlagCostBasedRanker was introduced in 9.0.
 *   requires_fcv_90,
 *   # setParameter calls to enable CBR will fail if a stepdown happens in between.
 *   does_not_support_stepdowns,
 *   # setParameter calls to enable CBR will hang on nodes having an initial sync and cause
 *   # timeouts.
 *   incompatible_with_initial_sync,
 *   # Explain calls will fail if a migration is going on.
 *   assumes_balancer_off,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getEngine,
    getPlanStage,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    ceEqual,
    getPlanRankerConfig,
    setPlanRankerConfigOnAllNonConfigNodes,
} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const kLimit = 3;

let coll;
let filter;
let prevPlanRankerConfig;

const mongodDb = FixtureHelpers.isMongos(db)
    ? FixtureHelpers.getPrimaries(db)[0].getDB(db.getName())
    : db;

// In the legacy (non-deferred) getExecutor path, CBR is disabled for queries that will be
// executed via SBE. When we are running in the legacy getExecutor path and SBE is fully enabled,
// then we will always build an SBE plan for the query so CBR will never run as expected in the
// test.
// TODO SERVER-119581: Once the feature flag controlling the deferred engine selection is
// deleted, this block should be able to be deleted.
const canRunCBR = !(
    checkSbeFullyEnabled(mongodDb) &&
    !FeatureFlagUtil.isEnabled(mongodDb, "GetExecutorDeferredEngineChoice")
);
if (!canRunCBR) {
    jsTest.log.info(
        `Skipping ${jsTestName()}: CBR is not run for SBE plans without deferred engine choice`,
    );
}

(canRunCBR ? describe : describe.skip)("a limit above a shard filter", function () {
    before(function () {
        prevPlanRankerConfig = getPlanRankerConfig(mongodDb);
        setPlanRankerConfigOnAllNonConfigNodes(db.getMongo(), {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "samplingCE",
        });

        coll = db[jsTestName()];
        coll.drop();

        assert.commandWorked(
            db.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
        );

        let docs = [];
        for (let i = 0; i < 200; i++) {
            docs.push({_id: i, f0: i, f1: i});
        }
        assert.commandWorked(coll.insertMany(docs));
        assert.commandWorked(coll.createIndexes([{f0: 1}, {f1: 1}]));

        // A non-selective bound on every indexed field, so each index yields a full-scan candidate
        // and the shard filter's cardinality estimate has to be clamped down to the limit.
        filter = {f0: {$gte: 0}, f1: {$gte: 0}};
    });

    after(function () {
        setPlanRankerConfigOnAllNonConfigNodes(db.getMongo(), prevPlanRankerConfig);
        coll.drop();
    });

    it("does not fail the query", function () {
        const res = coll.find(filter).limit(kLimit).toArray();
        assert.eq(kLimit, res.length, "expected the find to return 'limit' documents", {res});
    });

    it("is propagated into the shard filter's cardinality estimate", function () {
        const explain = coll.find(filter).limit(kLimit).explain();
        const shardFilterStage = getPlanStage(
            getWinningPlanFromExplain(explain),
            "SHARDING_FILTER",
        );
        assert.neq(null, shardFilterStage, "expected a SHARDING_FILTER stage", {explain});

        // TODO SERVER-92589: remove this exception for SBE. CBR still ranks the plans, so the
        // limit propagation test still runs, but the winning plan's explain output carries no
        // cost or cardinality estimates for it to be asserted on. This is required so that this
        // test passes under the trySbeEngine variant.
        if (getEngine(explain) !== "classic") {
            return;
        }

        // The presence of an estimate confirms the plan was costed by CBR rather than chosen by
        // multiplanning, and its value confirms the limit was propagated through the shard filter.
        const shardFilterCE = shardFilterStage.cardinalityEstimate;
        assert.neq(undefined, shardFilterCE, "expected the shard filter to be estimated by CBR", {
            shardFilterStage,
        });
        assert(
            ceEqual(shardFilterCE, kLimit),
            "expected the shard filter estimate to equal the limit",
            {shardFilterCE, limit: kLimit, shardFilterStage},
        );
    });
});
