/**
 * Tests that the V3 explain verbosity modes are rejected for rooted $or queries.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

describe("V3 explain verbosity on rooted $or queries", function () {
    let originalPlanOrChildrenIndependently;

    before(function () {
        // Guarantee subplanning is enabled regardless of the environment.
        originalPlanOrChildrenIndependently = assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryPlanOrChildrenIndependently: true}),
        ).was;

        const coll = db[collName];
        coll.drop();
        assert.commandWorked(
            coll.insert([
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: 2, b: 3},
            ]),
        );
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        // Restore the knob to its original value.
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryPlanOrChildrenIndependently: originalPlanOrChildrenIndependently,
            }),
        );
        db[collName].drop();
    });

    // Error codes thrown by the subplanning branches when a V3 verbosity is requested. There are two
    // sites (the standard find path and the deferred-engine-choice path); either may fire depending on
    // the active query execution engine.
    const kV3OnRootedOrErrorCodes = [13145000, 13145001];

    // V3 modes: must be rejected.
    const v3Verbosities = ["planSummary", "plannerChoice", "plannerStats", "execStats"];

    const rootedOr = {$or: [{a: 2}, {b: 3}]};

    const commands = [
        {name: "find", command: {find: collName, filter: rootedOr}},
        {
            name: "agg-lowered",
            command: {aggregate: collName, pipeline: [{$match: rootedOr}], cursor: {}},
        },
        {
            name: "agg-cursor-stage",
            command: {
                aggregate: collName,
                pipeline: [
                    {$match: rootedOr},
                    // Prevent pipeline from being optimized away.
                    {$_internalInhibitOptimization: {}},
                    {$group: {_id: "$a", c: {$sum: 1}}},
                ],
                cursor: {},
            },
        },
    ];

    for (const {name, command} of commands) {
        for (const verbosity of v3Verbosities) {
            it(`rejects V3 verbosity '${verbosity}' for ${name}`, function () {
                assert.commandFailedWithCode(
                    db.runCommand({explain: command, verbosity}),
                    kV3OnRootedOrErrorCodes,
                );
            });
        }
    }

    // The legacy verbosities are unaffected by the restriction on either path.
    for (const {name, command} of commands) {
        for (const verbosity of ["queryPlanner", "executionStats", "allPlansExecution"]) {
            it(`allows legacy verbosity '${verbosity}' for ${name}`, function () {
                assert.commandWorked(db.runCommand({explain: command, verbosity}));
            });
        }
    }
});
