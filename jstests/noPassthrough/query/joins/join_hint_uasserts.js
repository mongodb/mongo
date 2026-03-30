/**
 * Tests that join hinting uasserts for invalid cases.
 *
 * @tags: [
 *   requires_fcv_83,
 *   requires_sbe,
 * ]
 */
const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

const a = db[jsTestName() + "_a"];
const b = db[jsTestName() + "_b"];
const c = db[jsTestName() + "_c"];

// Insert dummy data into 3 collections.
let docs = [];
for (let i = 0; i < 20; i++) {
    docs.push({_id: i, x: i, y: i % 5});
}
for (const coll of [a, b, c]) {
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndexes([{x: 1}, {y: 1}]));
}

const pipeline = [
    {$lookup: {from: a.getName(), as: "foo", localField: "x", foreignField: "y"}},
    {$unwind: "$foo"},
    {$lookup: {from: b.getName(), as: "bar", localField: "foo.y", foreignField: "x"}},
    {$unwind: "$bar"},
];

function runWithHint(hint) {
    return db.runCommand({
        aggregate: c.getName(),
        pipeline: [{$_internalJoinHint: hint}].concat(pipeline),
        cursor: {},
    });
}

// Enable join optimization for most tests.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalEnableJoinOptimization: true,
        internalJoinReorderMode: "bottomUp",
    }),
);

// Invalid join method.
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {method: "BAD"}}],
    }),
    12016300,
);
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {method: 123}}],
    }),
    12016301,
);

// Invalid node id.
assert.commandFailedWithCode(
    runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {node: "bad"}}]}),
    12016301,
);
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {node: NumberInt(-1)}}],
    }),
    12016305,
);

// Invalid enumeration mode string.
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "INVALID"}]}), 12016302);

// Invalid plan shape string.
assert.commandFailedWithCode(
    runWithHint({planShape: "badShape", perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST"}]}),
    12016303,
);

// Inavlid JoinHint.
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {unknownField: "value"}}],
    }),
    12016306,
);
assert.commandFailedWithCode(
    runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", hint: {}}]}),
    12016307,
);

// Invalid level.
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(-1), mode: "CHEAPEST"}]}), 12016308);

// Invalid perSubsetLevelMode.
assert.commandFailedWithCode(
    runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST", badField: "value"}]}),
    12016309,
);
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(0)}]}), 12016310);
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{mode: "CHEAPEST"}]}), 12016310);
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: "notAnArray"}), 12016317);

// Invalid enumeration mode sequence (must start with level 0, no dupes).
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(1), mode: "CHEAPEST"}]}), 12016311);
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [
            {level: NumberInt(0), mode: "CHEAPEST"},
            {level: NumberInt(1), mode: "CHEAPEST"},
        ],
    }),
    12016311,
);
assert.commandFailedWithCode(runWithHint({}), 12016312);

// Fail when enableHJOrderPruning is not a boolean.
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST"}],
        enableHJOrderPruning: "yes",
    }),
    12016313,
);

// Fail for invalid hint field.
assert.commandFailedWithCode(
    runWithHint({
        perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST"}],
        unknownStrategyField: 123,
    }),
    12016314,
);

// $_internalJoinHint with random reorder mode should fail.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalJoinReorderMode: "random",
    }),
);
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST"}]}), 12016318);

// $_internalJoinHint without join optimization enabled should fail.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalEnableJoinOptimization: false,
    }),
);
assert.commandFailedWithCode(runWithHint({perSubsetLevelMode: [{level: NumberInt(0), mode: "CHEAPEST"}]}), 12016316);

MongoRunner.stopMongod(conn);
