/**
 * Tests that PQS query knobs are correctly reflected in explain output. Knobs at their default
 * setParameter values are omitted from explain; knobs at non-default setParameter values appear
 * with source "setParameter"; knobs applied via QuerySettings always appear (even at default
 * values) with source "querySettings".
 *
 * @tags: [
 *   # TODO SERVER-98659 Investigate why this test is failing on
 *   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
 *   does_not_support_stepdowns,
 *   # Query settings commands can not be run on the shards directly.
 *   directly_against_shardsvrs_incompatible,
 *   # TODO(SERVER-113800): Enable setClusterParameters with replicaset started with --shardsvr
 *   transitioning_replicaset_incompatible,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Maps PQS wire name → server parameter name.
const kWireNameToServerParam = {
    planRanker: "internalQueryPlanRanker",
    cbrCEMode: "internalQueryCBRCEMode",
    automaticCEPlanRankingStrategy: "automaticCEPlanRankingStrategy",
    samplingConfidenceInterval: "samplingConfidenceInterval",
    samplingMarginOfError: "samplingMarginOfError",
    samplingCEMethod: "internalQuerySamplingCEMethod",
    noTableScan: "notablescan",
};

const kPqsKnobsDefaults = {
    planRanker: "mixed",
    cbrCEMode: "samplingCE",
    automaticCEPlanRankingStrategy: "CBRForNoMultiplanningResults",
    samplingConfidenceInterval: "95",
    samplingMarginOfError: 5.0,
    samplingCEMethod: "chunk",
    noTableScan: false,
};

const kPqsKnobsNonDefault = {
    planRanker: "costBased",
    cbrCEMode: "heuristicCE",
    automaticCEPlanRankingStrategy: "CBRCostBasedRankerChoice",
    samplingConfidenceInterval: "90",
    samplingMarginOfError: 2.5,
    samplingCEMethod: "random",
};

function toServerParams(knobs) {
    let params = {};
    for (const [key, value] of Object.entries(knobs)) {
        params[kWireNameToServerParam[key]] = value;
    }
    return params;
}

function getKnobsFromExplain(representativeQuery) {
    const explainCmd = getExplainCommand(qsutils.withoutDollarDB(representativeQuery));
    const explain = assert.commandWorked(db.runCommand(explainCmd));
    return {knobs: explain.queryKnobs ?? null, explain};
}

/**
 * Asserts knob presence and values in explain output.
 * - expectedSource === null: each wireName in expectedKnobs must be absent.
 * - otherwise: each wireName must be present with the given source and value.
 */
function assertKnobs(actualKnobs, expectedKnobs, expectedSource, explain) {
    for (const [wireName, expectedValue] of Object.entries(expectedKnobs)) {
        if (!expectedSource) {
            assert(
                !actualKnobs || !actualKnobs[wireName],
                `Expected ${wireName} to be absent from explain, actualKnobs=${tojson(actualKnobs)}, explain=${tojson(explain)}`,
            );
            continue;
        }
        const knob = actualKnobs && actualKnobs[wireName];
        assert(
            knob,
            `Expected ${wireName} to appear in explain, actualKnobs=${tojson(actualKnobs)}, explain=${tojson(explain)}`,
        );
        assert.eq(
            knob.source,
            expectedSource,
            `Expected source of ${wireName} to be '${expectedSource}', knob=${tojson(knob)}, explain=${tojson(explain)}`,
        );
        assert.eq(
            knob.value,
            expectedValue,
            `Expected ${wireName} to have value ${expectedValue}, knob=${tojson(knob)}, explain=${tojson(explain)}`,
        );
    }
}

// queryFramework set via QuerySettings appears as queryFrameworkControl knob with source querySettings.
function testQueryFrameworkViaQuerySettings(
    representativeQuery,
    queryFramework,
    expectedKnobValue,
) {
    const {knobs, explain} = qsutils.withQuerySettings(representativeQuery, {queryFramework}, () =>
        getKnobsFromExplain(representativeQuery),
    );
    assertKnobs(knobs, {queryFrameworkControl: expectedKnobValue}, "querySettings", explain);
}

describe("Query knobs and explain", function () {
    const kTestCases = {
        "find": qsutils.makeFindQueryInstance({filter: {field1: 1}}),
        "aggregate": qsutils.makeAggregateQueryInstance({pipeline: [{$match: {field1: 1}}]}),
        "distinct": qsutils.makeDistinctQueryInstance({key: "field1", query: {field1: 1}}),
    };
    for (const [queryType, representativeQuery] of Object.entries(kTestCases)) {
        it(`setParameter at defaults: knobs absent from explain for ${queryType}`, function () {
            runWithParamsAllNonConfigNodes(db, toServerParams(kPqsKnobsDefaults), () => {
                const {knobs, explain} = getKnobsFromExplain(representativeQuery);
                assertKnobs(knobs, kPqsKnobsDefaults, null, explain);
            });
        });
        it(`setParameter at non-defaults: knobs present in explain for ${queryType}`, function () {
            const params = toServerParams(kPqsKnobsNonDefault);
            const {knobs, explain} = runWithParamsAllNonConfigNodes(db, params, () =>
                getKnobsFromExplain(representativeQuery),
            );
            assertKnobs(knobs, kPqsKnobsNonDefault, "setParameter", explain);
        });
        it(`PQS knobs always present in explain for ${queryType}`, function () {
            const settings = {queryKnobs: kPqsKnobsDefaults};
            const {knobs, explain} = qsutils.withQuerySettings(representativeQuery, settings, () =>
                getKnobsFromExplain(representativeQuery),
            );
            assertKnobs(knobs, kPqsKnobsDefaults, "querySettings", explain);
        });
        it(`PQS knobs override setParameter in explain for ${queryType}`, function () {
            const settings = {queryKnobs: kPqsKnobsNonDefault};
            const params = toServerParams(kPqsKnobsNonDefault);
            const {knobs, explain} = runWithParamsAllNonConfigNodes(db, params, () =>
                qsutils.withQuerySettings(representativeQuery, settings, () =>
                    getKnobsFromExplain(representativeQuery),
                ),
            );
            assertKnobs(knobs, kPqsKnobsNonDefault, "querySettings", explain);
        });
        it(`queryFramework "classic" via QuerySettings appears as knob in explain for ${queryType}`, function () {
            testQueryFrameworkViaQuerySettings(
                representativeQuery,
                "classic",
                "forceClassicEngine",
            );
        });
        it(`queryFramework "sbe" via QuerySettings appears as knob in explain for ${queryType}`, function () {
            testQueryFrameworkViaQuerySettings(representativeQuery, "sbe", "trySbeEngine");
        });
    }
});
