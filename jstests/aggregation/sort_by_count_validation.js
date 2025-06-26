/**
 * Validates that $sortByCount correctly validates its inputs during parsing.
 *
 *  * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */

const coll = db.getSiblingDB(jsTestName()).coll;
coll.drop();

// Insert some data first to avoid an EOF plan as the collection is empty.
assert.commandWorked(coll.insertMany([{a: 1}, {a: 2}, {a: 1}, {a: 3}, {a: 2}]));

assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {}}], cursor: {}}),
    [40147, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': "foo"}}], cursor: {}}),
    [40147, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'$': "foo"}}], cursor: {}}),
    [ErrorCodes.InvalidPipelineOperator, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'$a': "foo"}}], cursor: {}}),
    [ErrorCodes.InvalidPipelineOperator, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'a': "foo"}}], cursor: {}}),
    [40147, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': {}}}], cursor: {}}),
    [40147, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': ''}}], cursor: {}}),
    [40147, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': {'': {}}}}], cursor: {}}),
    [40147, 9423101]);

assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: ''}], cursor: {}}),
    [40148, 9423101]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: '$'}], cursor: {}}),
    [40148, 9423101]);
