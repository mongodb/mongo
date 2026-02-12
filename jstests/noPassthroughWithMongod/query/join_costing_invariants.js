// Test basic join costing invariants that are expected to hold regardless of the
// actual coefficients that are used or any future calibration work.
// @tags: [
//   requires_sbe
// ]

import {describe, it} from "jstests/libs/mochalite.js";

import {checkJoinOptimizationStatus} from "jstests/libs/query/sbe_util.js";
import {checkPauseAfterPopulate} from "jstests/libs/pause_after_populate.js";

const joinOptimizationStatus = checkJoinOptimizationStatus(typeof db === "undefined" ? null : db);
if (!joinOptimizationStatus) {
    jsTest.log.info("Join costing test is skipped as Join Optimization is not enabled.");
    quit();
}

function populate() {
    const collSize = 1000;

    const documents = [];
    for (let i = 0; i < collSize; i++) {
        documents.push({
            i_idx: i,
            i_noidx: i,
            c_idx: 1,
            d_idx: i % 10,
        });
    }

    db.many_rows.insertMany(documents);
    db.many_rows.createIndex({i_idx: 1});
    db.many_rows.createIndex({c_idx: 1});
    db.many_rows.createIndex({d_idx: 1});

    // An empty collection
    db.no_rows.createIndex({i_idx: 1});

    // Collection with a single row
    db.one_row.insert({i_idx: 1});
    db.one_row.createIndex({i_idx: 1});
}

function getValueByPath(obj, path) {
    /**
     * Given a dotted path provided as string, return the corresponding key in the object.
     */

    const parts = path.split(".");

    let current = obj;
    for (let part of parts) {
        // Check for array index (e.g., keyName[0])
        const match = part.match(/^([^\[]+)(\[(\d+)\])?$/);
        if (!match) return undefined;

        const key = match[1];
        const index = match[3] !== undefined ? parseInt(match[3], 10) : undefined;

        current = current?.[key];
        if (index !== undefined) {
            current = current?.[index];
        }

        if (current === undefined) return undefined;
    }
    return current;
}

function getCosts(command1, command2, path) {
    /**
     * Extract the costEstimate from two commands for the given dotted path.
     */

    command1["cursor"] = {};
    command2["cursor"] = {};

    const explain1 = db.runCommand({
        explain: command1,
    });

    const explain2 = db.runCommand({
        explain: command2,
    });

    const winningPlan1 = explain1.queryPlanner.winningPlan;
    const winningPlan2 = explain2.queryPlanner.winningPlan;

    const cost1 = getValueByPath(winningPlan1, path + ".costEstimate").toFixed(3);
    const cost2 = getValueByPath(winningPlan2, path + ".costEstimate").toFixed(3);

    return [cost1, cost2];
}

function assertCostGt(command1, command2, path) {
    const [cost1, cost2] = getCosts(command1, command2, path);
    print(`Command #1: ${JSON.stringify(command1)}`);
    print(`Command #2: ${JSON.stringify(command2)}`);
    print(`Path: ${path}`);
    print(`Costs: ${cost1} vs ${cost2}`);
    assert.gt(
        cost2,
        cost1,
        `Expected cost at '${path}' of command #2 (${cost2}) to be greater than cost of command #1 (${cost1}).`,
    );
}

function costEq(command1, command2, path) {
    const [cost1, cost2] = getCosts(command1, command2, path);
    assert.eq(
        cost1,
        cost2,
        `Expected cost at '${path}' of command #1 (${cost1}) to be equal to cost of command #2 (${cost2}).`,
    );
}

function costsAlmostZero(command1, command2, path) {
    const [cost1, cost2] = getCosts(command1, command2, path);
    assert(
        cost1 < 0.1 && cost2 < 0.1,
        `Expected both costs at path '${path}' to be less than 0.1 (${cost1} and ${cost2}).`,
    );
}

populate();
checkPauseAfterPopulate();

describe("Costing of individual inputs to a join", () => {
    it("Empty inputs should have near-zero costs", () => {
        costsAlmostZero(
            {
                aggregate: "no_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "no_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });

    it("Inputs with no matching rows should have near-zero costs (IXSCAN)", () => {
        costsAlmostZero(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_idx: -1}},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {
                        "$lookup": {
                            "from": "many_rows",
                            "localField": "a",
                            "foreignField": "a",
                            pipeline: [{$match: {i_idx: -1}}],
                            "as": "a",
                        },
                    },
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });

    it("Larger base table should have higher cost", () => {
        assertCostGt(
            {
                aggregate: "no_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan.inputStages[0]",
        );
        assertCostGt(
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });

    it("More matching base-table documents should have higher cost (COLLSCAN).", () => {
        assertCostGt(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_noidx: {$gt: 500}}},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_noidx: {$gt: 250}}},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });

    it("More matching base-collection documents should have higher cost (IXSCAN + FETCH).", () => {
        assertCostGt(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_idx: {$lt: 50}}},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_idx: {$lt: 250}}},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });

    it("Larger $lookup collection should have higher cost.", () => {
        assertCostGt(
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "one_row", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan.inputStages[1]",
        );
    });

    // TODO SERVER-118886 Join optimization: cost estimate for FETCH is incorrect
    it.skip("COLLSCAN should have a higher cost than a narrow FETCH + IXSCAN.", () => {
        assertCostGt(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {"i_idx": 1}},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {"i_noidx": 1}},
                ],
            },
            "queryPlan.inputStages[0]",
        );
    });
});

describe("Costing entire joins", () => {
    it("Joins over an empty collection should have an almost-zero cost.", () => {
        costsAlmostZero(
            {
                aggregate: "no_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "no_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );
    });

    it("Joins with empty input should have an almost-zero cost (IXSCAN).", () => {
        costsAlmostZero(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                    {"$match": {i_idx: -1}},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {
                        "$lookup": {
                            "from": "many_rows",
                            "localField": "a",
                            "foreignField": "a",
                            pipeline: [{$match: {i_idx: -1}}],
                            "as": "a",
                        },
                    },
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );
    });

    it("Join should have higher cost on larger inputs", () => {
        assertCostGt(
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "one_row", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "one_row", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );

        assertCostGt(
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "one_row", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "a", "foreignField": "a", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );
    });

    it("Join with a more selective join condition should have a lower cost", () => {
        assertCostGt(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "d_idx", "foreignField": "d_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "c_idx", "foreignField": "c_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );

        assertCostGt(
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "i_idx", "foreignField": "d_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "d_idx", "foreignField": "d_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );
    });

    it("Symmetrical joins should have identical costs", () => {
        costEq(
            {
                aggregate: "one_row",
                pipeline: [
                    {"$lookup": {"from": "many_rows", "localField": "i_idx", "foreignField": "i_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            {
                aggregate: "many_rows",
                pipeline: [
                    {"$lookup": {"from": "one_row", "localField": "i_idx", "foreignField": "i_idx", "as": "a"}},
                    {"$unwind": "$a"},
                ],
            },
            "queryPlan",
        );
    });
});
