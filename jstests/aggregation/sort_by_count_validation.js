/**
 * Validates that $sortByCount correctly validates its inputs during parsing.
 */

const coll = db.getSiblingDB(jsTestName()).coll;
coll.drop();

assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {}}], cursor: {}}), [40147]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': "foo"}}], cursor: {}}),
    [40147]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'$': "foo"}}], cursor: {}}),
    [ErrorCodes.InvalidPipelineOperator]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'$a': "foo"}}], cursor: {}}),
    [ErrorCodes.InvalidPipelineOperator]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'a': "foo"}}], cursor: {}}),
    [40147]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': {}}}], cursor: {}}), [40147]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': ''}}], cursor: {}}), [40147]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: {'': {'': {}}}}], cursor: {}}),
    [40147]);

assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: ''}], cursor: {}}), [40148]);
assert.commandFailedWithCode(
    db.runCommand({aggregate: "coll", pipeline: [{$sortByCount: '$'}], cursor: {}}), [40148]);
