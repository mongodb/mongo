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
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {buildQueryKnobsModel} from "jstests/libs/property_test_helpers/models/query_knob_models.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const numRuns = 50;
const seed = 4;

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

// The knob model is keyed by server parameter name, while setQuerySettings and explain address
// knobs by their wire name.
const wireNameByName = Object.fromEntries(knobSchema.map((knob) => [knob.name, knob.wireName]));
const queryKnobsModel = buildQueryKnobsModel(knobSchema);

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
            assert(knob, "expected knob to appear in explain", {wireName, explain});
            assert.eq(knob.source, expectedSource, "unexpected knob source", {wireName, knob});
            assert(valuesEqual(knob.value, expectedValue), "unexpected knob value", {
                wireName,
                knob,
                expectedValue,
            });
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
        jsTest.log.info("Setting knobs via setQuerySettings: " + tojsononeline(queryKnobs));
        qsutils.withQuerySettings(representativeQuery, {queryKnobs}, () =>
            assertKnobsInExplain("querySettings"),
        );
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
    fc.assert(fc.property(queryKnobsModel, assertKnobValidationParity), {seed, numRuns});
} finally {
    assertDropCollection(db, coll.getName());
}
