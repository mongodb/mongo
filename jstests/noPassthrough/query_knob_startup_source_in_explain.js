/**
 * Regression test for SERVER-130164: query knobs set via --setParameter at startup must appear in
 * explain with source "setParameter". The bug was that the change notifier could be wired up after
 * startup options were processed, so the source was never recorded.
 *
 * Covers CBR knobs of each relevant scalar type (int, double, bool) and verifies that
 * $listQueryKnobs reports correct IDL default values regardless of startup overrides.
 *
 * @tags: [
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

// Non-default values so the knobs appear in explain (knobs at default are omitted).
const kStartupParams = {
    internalQueryNumWorksPerPlanForMPEstimation: 200, // default 384
    samplingMarginOfError: 3.0, // default 5.0
    internalQuerySamplingByStrides: true, // default false
};

// Wire name → expected value in explain.
const kExpected = {
    numWorksPerPlanForMPEstimation: 200,
    samplingMarginOfError: 3.0,
    samplingByStrides: true,
};

let conn;
let coll;

before(function () {
    conn = MongoRunner.runMongod({setParameter: kStartupParams});
    assert.neq(null, conn, "mongod failed to start");
    coll = conn.getDB("test").getCollection(jsTestName());
    coll.drop();
    assert.commandWorked(coll.insertOne({x: 1}));
});

after(function () {
    MongoRunner.stopMongod(conn);
});

describe("Query knobs set at startup via --setParameter", function () {
    const kKnobsUnderTest = [
        "planRankerMode",
        "samplingMarginOfError",
        "samplingCEMethod",
        "numWorksPerPlanForMPEstimation",
        "samplingByStrides",
    ];

    function getListQueryKnobs() {
        const knobs = {};
        const pipeline = [{$listQueryKnobs: {}}, {$match: {wireName: {$in: kKnobsUnderTest}}}];
        for (const doc of conn.getDB("admin").aggregate(pipeline).toArray()) {
            knobs[doc.wireName] = doc;
        }
        return knobs;
    }

    function getExplainKnobs() {
        const explain = assert.commandWorked(
            conn.getDB("test").runCommand({explain: {find: coll.getName(), filter: {x: 1}}}),
        );
        return explain.queryKnobs ?? {};
    }

    it("should register with correct default values", function () {
        const knobs = getListQueryKnobs();

        assert.eq("automaticCE", knobs["planRankerMode"].default, "planRankerMode default", {
            knob: knobs["planRankerMode"],
        });
        assert.eq(5.0, knobs["samplingMarginOfError"].default, "samplingMarginOfError default", {
            knob: knobs["samplingMarginOfError"],
        });
        assert.eq("chunk", knobs["samplingCEMethod"].default, "samplingCEMethod default", {
            knob: knobs["samplingCEMethod"],
        });
        assert.eq(
            384,
            knobs["numWorksPerPlanForMPEstimation"].default,
            "numWorksPerPlanForMPEstimation default",
            {knob: knobs["numWorksPerPlanForMPEstimation"]},
        );
        assert.eq(false, knobs["samplingByStrides"].default, "samplingByStrides default", {
            knob: knobs["samplingByStrides"],
        });
    });

    it("appear in explain with source 'setParameter'", function () {
        const knobs = getExplainKnobs();
        for (const [wireName, expectedValue] of Object.entries(kExpected)) {
            const knob = knobs[wireName];
            assert(knob, `Expected ${wireName} in explain queryKnobs`, {knobs});
            assert.eq("setParameter", knob.source, `Wrong source for ${wireName}`, {knob});
            assert.eq(expectedValue, knob.value, `Wrong value for ${wireName}`, {knob});
        }
    });

    it("knobs not overridden at startup are absent from explain", function () {
        const knobs = getExplainKnobs();
        assert(!knobs["planRankerMode"], "planRankerMode should be absent (not overridden)", {
            knobs,
        });
    });
});

// noTableScan is unusual: the --notablescan startup option writes storageGlobalParams.noTableScan
// directly, bypassing the ServerParameter setter (and thus the change notifier). It must still show
// up in explain with source "setParameter".
describe("noTableScan set at startup via the --notablescan option", function () {
    let ntsConn;
    let ntsColl;

    before(function () {
        ntsConn = MongoRunner.runMongod({notablescan: ""});
        assert.neq(null, ntsConn, "mongod failed to start with --notablescan");
        ntsColl = ntsConn.getDB("test").getCollection(jsTestName());
        ntsColl.drop();
        assert.commandWorked(ntsColl.insertOne({x: 1}));
        // An index is required so explain can produce a plan without a (disallowed) collection scan.
        assert.commandWorked(ntsColl.createIndex({x: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(ntsConn);
    });

    it("appears in explain with source 'setParameter'", function () {
        const explain = assert.commandWorked(
            ntsConn.getDB("test").runCommand({explain: {find: ntsColl.getName(), filter: {x: 1}}}),
        );
        const knob = (explain.queryKnobs ?? {})["noTableScan"];
        assert(knob, "Expected noTableScan in explain queryKnobs", {
            queryKnobs: explain.queryKnobs,
        });
        assert.eq("setParameter", knob.source, "Wrong source for noTableScan", {knob});
        assert.eq(true, knob.value, "Wrong value for noTableScan", {knob});
    });
});
