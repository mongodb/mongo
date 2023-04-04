/**
 * Verify that $natural can only take 1 and -1 as input.
 * Cannot run against an unsharded collection due to findAndModify.
 * @tags: [
 *   assumes_unsharded_collection,
 *   requires_fastcount,
 *   requires_fcv_70,
 * ]
 */
(function() {
const collName = jsTestName();
const coll = db[collName];
coll.drop();

const badNaturals = [
    0,
    NaN,
    "reverse",
    {forward: 1},
    3,
    -100,
    "1",
    "-1",
    1.1,
    NumberDecimal("1.1"),
    -1.1,
    NumberDecimal("-1.4999")
];

const goodNaturals = [
    Number(1),
    NumberInt(1),
    NumberLong(1),
    NumberDecimal("1"),
    Number(-1),
    NumberInt(-1),
    NumberLong(-1),
    NumberDecimal("-1")
];

const findParams = ["sort", "hint"];
const pipelines = [[], [{$group: {_id: 0}}]];

const runFind = (param, natural) =>
    coll.runCommand({find: collName, filter: {}, [param]: {$natural: natural}});

const runAgg = (pipeline, natural) =>
    coll.runCommand({aggregate: collName, pipeline, cursor: {}, hint: {$natural: natural}});

const runCount = (natural) => coll.runCommand({count: collName, hint: {$natural: natural}});

const runFindAndModify = (param, natural) => coll.runCommand(
    {findAndModify: collName, [param]: {$natural: natural}, update: {updated: true}});

const runUpdate = (natural) => coll.runCommand(
    {update: collName, updates: [{q: {}, u: {updated: true}, hint: {$natural: natural}}]});

const runDelete = (natural) =>
    coll.runCommand({delete: collName, deletes: [{q: {}, limit: 1, hint: {$natural: natural}}]});

for (const natural of goodNaturals) {
    for (const param of findParams) {
        assert.commandWorked(runFind(param, natural), `${param}: {$natural: ${natural}}`);
        assert.commandWorked(runFindAndModify(param, natural), `count {$natural: ${natural}}`);
    }

    for (const pipeline of pipelines) {
        assert.commandWorked(runAgg(pipeline, natural), `agg {$natural: ${natural}}`);
    }

    assert.commandWorked(runCount(natural), `count {$natural: ${natural}}`);
    assert.commandWorked(runUpdate(natural), `update {$natural: ${natural}}`);
    assert.commandWorked(runDelete(natural), `delete {$natural: ${natural}}`);
}
for (const natural of badNaturals) {
    for (const param of findParams) {
        assert.commandFailedWithCode(runFind(param, natural),
                                     ErrorCodes.BadValue,
                                     `find {${param}: {$natural: ${natural}}}`);
        assert.commandFailedWithCode(runFindAndModify(param, natural),
                                     ErrorCodes.BadValue,
                                     `findAndModify {$natural: ${natural}}`);
    }

    for (const pipeline of pipelines) {
        assert.commandFailedWithCode(
            runAgg(pipeline, natural), ErrorCodes.BadValue, `agg {$natural: ${natural}}`);
    }

    assert.commandFailedWithCode(
        runCount(natural), ErrorCodes.BadValue, `count {$natural: ${natural}}`);
    assert.commandFailedWithCode(
        runUpdate(natural), ErrorCodes.BadValue, `update {$natural: ${natural}}`);
    assert.commandFailedWithCode(
        runDelete(natural), ErrorCodes.BadValue, `delete {$natural: ${natural}}`);
}
}());
