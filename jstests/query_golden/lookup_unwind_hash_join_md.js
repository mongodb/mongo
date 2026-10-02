/**
 * Golden test for $lookup+$unwind hash join ResultInfo participation; results are
 * field-order-sensitive and plan UUIDs are normalized away.
 */

import {section, subSection, code, line, linebreak} from "jstests/libs/query/pretty_md.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

const localDocs = [
    {_id: 0, lkey: 1, a: 5, blob: "x"},
    {_id: 1, lkey: 2, a: 7},
    {_id: 2, lkey: [1, 3], a: 9, blob: "y"},
    {_id: 3, joined: "preexisting", lkey: 3},
    {_id: 4, a: 1},
    {_id: 5, lkey: null, blob: "z"},
];

const foreignDocs = [
    {_id: 10, fkey: 1, b: 10},
    {_id: 11, fkey: 1, b: 20},
    {_id: 12, fkey: 3, b: 30},
    {_id: 13, fkey: null, b: 40},
];

const unwinds = [
    {$unwind: "$joined"},
    {$unwind: {path: "$joined", preserveNullAndEmptyArrays: true}},
    {$unwind: {path: "$joined", includeArrayIndex: "idx"}},
];

// Stages appended after the $lookup+$unwind.
const suffixStages = [
    [{$addFields: {computed: {$add: ["$a", "$joined.b"]}}}],
    [{$project: {_id: 0, lkey: 1, joined: 1}}],
];

// 'allowDiskUse: true' selects the hash join strategy.
const aggOptions = [{allowDiskUse: true}];

const lookup = {$lookup: {from: "foreign", localField: "lkey", foreignField: "fkey", as: "joined"}};

const uuidRegex = /@"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"/g;

// Finds the strategy of the $lookup stage anywhere in the winning plan tree.
function findLookupStrategy(node) {
    if (node.stage === "EQ_LOOKUP" || node.stage === "EQ_LOOKUP_UNWIND") {
        return node.strategy;
    }
    const children = (node.inputStages || []).concat(node.inputStage ? [node.inputStage] : []);
    for (const child of children) {
        const strategy = findLookupStrategy(child);
        if (strategy) {
            return strategy;
        }
    }
    return null;
}

db.local.drop();
db.foreign.drop();
assert.commandWorked(db.local.insertMany(localDocs));
assert.commandWorked(db.foreign.insertMany(foreignDocs));

// Data for the dotted "as" path runs: "a" is an object with siblings, missing, a conflicting
// scalar, and an object with a preexisting "a.b".
const dottedLocalDocs = [
    {_id: 0, lkey: 1, a: {x: 10}},
    {_id: 1, lkey: 2},
    {_id: 2, lkey: [1, 3], a: 5},
    {_id: 3, lkey: 3},
    {_id: 4, a: {x: 1, b: "preexisting"}},
    {_id: 5, lkey: null, a: {}},
];

const dottedForeignDocs = [
    {_id: 10, fkey: 1, c: 100},
    {_id: 11, fkey: 1, c: 200},
    {_id: 12, fkey: 3, c: 300},
    {_id: 13, fkey: null, c: 400},
];

db.localDotted.drop();
db.foreignDotted.drop();
assert.commandWorked(db.localDotted.insertMany(dottedLocalDocs));
assert.commandWorked(db.foreignDotted.insertMany(dottedForeignDocs));

// Pin the strategy flags so the plans depend only on the engine mode, not the build's flags.
const strategyFlags = {
    featureFlagSbeEqLookupUnwindHashJoin: true,
    featureFlagSbeEqLookupUnwindNestedLoopJoin: false,
    featureFlagSbeEqLookupUnwindIndexedLoopJoin: false,
    featureFlagSbeEqLookupUnwindDynamicIndexedLoopJoin: false,
};
const priorFlagValues = {};
for (const [flag, value] of Object.entries(strategyFlags)) {
    priorFlagValues[flag] = assert.commandWorked(db.adminCommand({getParameter: 1, [flag]: 1}))[
        flag
    ];
    assert.commandWorked(db.adminCommand({setParameter: 1, [flag]: value}));
}

function runPipeline(collName, pipeline, options) {
    subSection("Pipeline");
    code(tojson(pipeline));
    subSection("Options");
    code(tojsononeline(options));

    const cmd = Object.assign({aggregate: collName, pipeline: pipeline, cursor: {}}, options);
    const res = db.runCommand(cmd);
    subSection("Results");
    if (res.ok !== 1) {
        line(`Error: ${res.code} ${res.codeName}: ${res.errmsg}`);
    } else {
        code(res.cursor.firstBatch.map((doc) => tojsononeline(doc)).join("\n") + "\n", "text");
    }

    subSection("Plan");
    const explain = db.getCollection(collName).explain().aggregate(pipeline, options);
    line(`Engine: ${explain.explainVersion === "2" ? "SBE" : "classic"}`);
    const winningPlan = getQueryPlanner(explain).winningPlan;
    const queryPlan = winningPlan.queryPlan || winningPlan;
    const strategy = findLookupStrategy(queryPlan);
    if (strategy) {
        line(`Strategy: ${strategy}`);
    }
    if (winningPlan.slotBasedPlan) {
        line(`Slots: ${winningPlan.slotBasedPlan.slots}`);
        code(winningPlan.slotBasedPlan.stages.replace(uuidRegex, '@""'), "text");
    }
    // SBE plans that are not fully pushed into the query system only show the $cursor subplan
    // above; print the remaining aggregation stages (e.g. a $lookup that absorbed a $match).
    if (explain.explainVersion === "2" && Array.isArray(explain.stages)) {
        const stagesAboveCursor = explain.stages.filter((stage) => !stage.$cursor);
        if (stagesAboveCursor.length > 0) {
            line("Stages above the cursor:");
            code(tojson(stagesAboveCursor).replace(uuidRegex, '@""'));
        }
    }
    linebreak();
}

try {
    unwinds.forEach((unwind) => {
        section(`Unwind: ${tojsononeline(unwind)}`);
        linebreak();

        suffixStages.forEach((suffix) => {
            aggOptions.forEach((options) => {
                const pipeline = [lookup, unwind].concat(suffix);
                runPipeline("local", pipeline, options);
            });
        });
    });

    // $lookup with a dotted "as" path and parent stages reading a prefix of the "as" path ("a"),
    // the full "as" path ("a.b"), and a path deeper than the "as" path ("a.b.c").
    const dottedLookup = {
        $lookup: {from: "foreignDotted", localField: "lkey", foreignField: "fkey", as: "a.b"},
    };

    const dottedUnwinds = [
        {$unwind: "$a.b"},
        {$unwind: {path: "$a.b", preserveNullAndEmptyArrays: true}},
    ];

    const dottedSuffixStages = [
        [],
        [{$match: {a: {$exists: true}}}],
        [{$match: {"a.b": {$exists: true}}}],
        [{$match: {"a.b.c": {$gt: 150}}}],
        [{$match: {a: {$exists: true}, "a.b.c": {$gt: 150}}}],
    ];

    dottedUnwinds.forEach((unwind) => {
        section(`Dotted as path, unwind: ${tojsononeline(unwind)}`);
        linebreak();

        dottedSuffixStages.forEach((suffix) => {
            aggOptions.forEach((options) => {
                const pipeline = [dottedLookup, unwind].concat(suffix);
                runPipeline("localDotted", pipeline, options);
            });
        });
    });
} finally {
    for (const [flag, value] of Object.entries(priorFlagValues)) {
        assert.commandWorked(db.adminCommand({setParameter: 1, [flag]: value}));
    }
}

jsTest.log.info("Done");
