/**
 * A property-based test verifying that setQuerySettings and setParameter agree on query knob
 * validation. For each randomly generated set of valid knob values, the values must be accepted by
 * setParameter on all non-config nodes as well as by setQuerySettings, and the query settings
 * values must appear in explain output with source "querySettings".
 *
 * @tags: [
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   # Incompatible with setParameter.
 *   does_not_support_stepdowns,
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr.
 *   transitioning_replicaset_incompatible,
 *   # Some query knobs may not exist on older versions.
 *   multiversion_incompatible,
 *   # Uses runWithParamsAllNonConfigNodes which requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # Knobs applied to the config DB time out internal config queries.
 *   config_shard_incompatible,
 *   # The config fuzzer re-sets internalQueryStatsSampleRate at runtime, which races the exact
 *   # knob-value assertions in explain output.
 *   does_not_support_config_fuzzer,
 *   requires_fcv_90,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {getCurrentFCV} from "jstests/libs/feature_compatibility_version.js";
import {buildQueryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {kPbtInterruptAfterTimeLimitMs} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const numRuns = 50;
const seed = 4;

// How long to wait for query-settings knob overrides to be observable in explain output. Normal
// convergence is on the order of milliseconds.
const kKnobPropagationTimeoutMs = 30 * 1000;

const coll = assertDropAndRecreateCollection(db, jsTestName());
assert.commandWorked(coll.insert({x: 1}));
const qsutils = new QuerySettingsUtils(db, coll.getName());
const representativeQuery = qsutils.makeFindQueryInstance({filter: {x: 1}});

const excludeKnobs = [
    // Disallows collection scans, which makes explaining the representative query fail with
    // NoQueryExecutionPlans rather than exercising the validation parity property.
    "notablescan",
    // Can reject an unbounded COLLSCAN on the representative query's collection, which makes
    // explaining it fail with NoQueryExecutionPlans rather than exercising the validation parity
    // property.
    "maxEstimatedScanBytes",
    // "histogramCE" makes explain fail with HistogramCEFailure since no histogram exists on the
    // filtered path, which is a server-side guardrail rather than a validation divergence.
    "internalQueryCBRCEMode",
    // Memory limit caps: small values make the representative query fail with ExceededMemoryLimit,
    // a server-side guardrail rather than a validation divergence.
    "internalQueryMaxPushBytes",
    "internalQueryMaxAddToSetBytes",
    "internalQueryMaxConcatArraysBytes",
    "internalQueryMaxSetUnionBytes",
    "internalQueryTopNAccumulatorBytes",
    "internalQueryMaxPercentileAccumulatorBytes",
    "internalQueryMaxMemoryUsageBytesPerOperation",
    // TODO(SERVER-131322): remove this exclusion once the resharding invariant (BF-44347) is fixed.
    "internalQueryPlannerUseMultiplannerForSingleSolutions",
];

// Only pqsSettable knobs can be accepted by setQuerySettings; for all others the property holds
// vacuously.
const knobSchema = db
    .getSiblingDB("admin")
    .aggregate([
        {$listQueryKnobs: {}},
        {$match: {pqsSettable: true, name: {$nin: excludeKnobs}}},
        {$sort: {name: 1}},
    ])
    .toArray();
assert.gt(knobSchema.length, 0, "expected at least one pqsSettable query knob");

const currentFcv = getCurrentFCV(db);
const eligibleKnobs = knobSchema.filter(
    (knob) => !knob.minFcv || MongoRunner.compareBinVersions(currentFcv, knob.minFcv) >= 0,
);
assert.gt(eligibleKnobs.length, 0, "expected at least one FCV-eligible pqsSettable query knob");

// The knob model is keyed by server parameter name, while setQuerySettings and explain address
// knobs by their wire name.
const wireNameByName = Object.fromEntries(eligibleKnobs.map((knob) => [knob.name, knob.wireName]));
const queryKnobsModel = buildQueryKnobsModel(eligibleKnobs);

function valuesEqual(a, b) {
    return bsonWoCompare({v: a}, {v: b}) === 0;
}

function assertKnobValidationParity(knobToVal) {
    if (Object.keys(knobToVal).length === 0) {
        // The model generated no knobs; the property holds vacuously.
        return true;
    }
    const queryKnobs = Object.fromEntries(
        Object.entries(knobToVal).map(([name, value]) => [wireNameByName[name], value]),
    );
    jsTest.log.info("Running knob validation parity case: " + tojsononeline(knobToVal));
    function assertKnobsInExplain(expectedSource, {userFacing = false} = {}) {
        jsTest.log.info(
            `Explaining with expected source '${expectedSource}', userFacing=${userFacing}, ` +
                `queryKnobs: ${tojsononeline(queryKnobs)}`,
        );
        const queryCmd = qsutils.withoutDollarDB(representativeQuery);
        if (userFacing) {
            queryCmd.querySettings = {queryKnobs};
        }
        const explainCmd = getExplainCommand(queryCmd);
        const explain = assert.commandWorked(db.runCommand(explainCmd));
        for (const [wireName, expectedValue] of Object.entries(queryKnobs)) {
            const knob = explain.queryKnobs?.[wireName];
            assert(knob, `expected knob '${wireName}' to appear in explain`, {wireName, explain});
            assert.eq(knob.source, expectedSource, `unexpected knob source for '${wireName}'`, {
                wireName,
                knob,
            });
            assert(
                valuesEqual(knob.value, expectedValue),
                `unexpected knob value for '${wireName}': expected ` +
                    `${tojson(expectedValue)}, got ${tojson(knob.value)}`,
                {wireName, knob, expectedValue},
            );
        }
    }

    // Setting the knobs via setParameter must succeed, given that the very same values are
    // accepted by setQuerySettings below.
    jsTest.log.info(
        "Setting knobs via setParameter on all non-config nodes: " + tojsononeline(knobToVal),
    );
    runWithParamsAllNonConfigNodes(db, knobToVal, () => {
        // The model only generates non-default values, so with only setParameter applied each knob
        // must be reported with source "setParameter".
        assertKnobsInExplain("setParameter");
        // The same holds for user-facing query settings passed directly on the command: their knob
        // values must win over the setParameter ones and be reported with source "querySettings".
        assertKnobsInExplain("querySettings", {userFacing: true});
        // The query settings knob values must win over the setParameter ones and be reported with
        // source "querySettings".
        // TODO(SERVER-136720): remove the retry once the query settings install no longer skips
        // on an unchanged clusterParameterTime.
        jsTest.log.info("Setting knobs via setQuerySettings: " + tojsononeline(queryKnobs));
        qsutils.withQuerySettings(representativeQuery, {queryKnobs}, () => {
            let lastError = null;
            assert.soon(
                () => {
                    try {
                        assertKnobsInExplain("querySettings");
                        return true;
                    } catch (error) {
                        lastError = error;
                        return false;
                    }
                },
                () =>
                    `explain never reported source 'querySettings' for all knobs; last failure: ` +
                    `${lastError?.message ?? lastError}`,
                kKnobPropagationTimeoutMs,
                500 /* interval */,
                {runHangAnalyzer: false},
                {queryKnobs},
            );
        });
    });
    return true;
}

jsTest.log.info(
    "Running property `assertKnobValidationParity` from test file `" +
        jsTestName() +
        "`, seed = " +
        seed,
);
try {
    fc.assert(fc.property(queryKnobsModel, assertKnobValidationParity), {
        seed,
        numRuns,
        interruptAfterTimeLimit: kPbtInterruptAfterTimeLimitMs,
    });
} finally {
    assertDropCollection(db, coll.getName());
}
