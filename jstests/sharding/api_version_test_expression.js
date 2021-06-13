/**
 * Tests for ensuring api parameters propegate when pipelines are sent to shards.
 * @tags: [
 * requires_fcv_47,
 * requires_sharding,
 * uses_api_parameters,
 * ]
 */
(function() {
"use strict";

const dbName = jsTestName();
const collName = "test";
const collForeignName = collName + "_foreign";

/* Setup two shard with equal numbers of documents. */
const st = new ShardingTest({shards: 2});

const mongos = st.s0;
const db = mongos.getDB(dbName);
const admin = mongos.getDB("admin");
const config = mongos.getDB("config");
const shards = config.shards.find().toArray();
const namespace = dbName + "." + collName;
const namespaceForeign = dbName + "." + collForeignName;

/* Shard the collection. */
assert.commandWorked(admin.runCommand({enableSharding: dbName}));
assert.commandWorked(admin.runCommand({movePrimary: dbName, to: shards[0]._id}));
assert.commandWorked(admin.runCommand({shardCollection: namespace, key: {a: 1}}));

const coll = mongos.getCollection(namespace);
const collForeign = mongos.getCollection(namespaceForeign);
const num = 10;
const middle = num / 2;
for (let i = 0; i < num; i++) {
    assert.commandWorked(coll.insert({a: i}));
    assert.commandWorked(collForeign.insert({a: i}));
}

st.shardColl(collName, {a: 1}, {a: middle}, {a: middle + 1}, dbName);

// Assert error thrown when the command specifies apiStrict:true and an inner pipeline contains an
// unstable expression.
const unstableInnerPipeline = [{$project: {v: {$_testApiVersion: {unstable: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIStrictError);

// Assert command worked when the command specifies apiStrict:false and an inner pipeline contains
// an unstable expression.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: unstableInnerPipeline}}],
    cursor: {},
    apiStrict: false,
    apiVersion: "1"
}));

// Assert error thrown when the command specifies apiDeprecationErrors:true and an inner pipeline
// contains a deprecated expression.
const deprecatedInnerPipeline = [{$project: {v: {$_testApiVersion: {deprecated: true}}}}];
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);
assert.commandFailedWithCode(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: true,
    apiVersion: "1"
}),
                             ErrorCodes.APIDeprecationError);

// Assert command worked when the command specifies apiDeprecationErrors:false and an inner pipeline
// contains a deprecated expression.
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$lookup: {from: collForeignName, as: "output", pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));
assert.commandWorked(db.runCommand({
    aggregate: collName,
    pipeline: [{$unionWith: {coll: collForeignName, pipeline: deprecatedInnerPipeline}}],
    cursor: {},
    apiDeprecationErrors: false,
    apiVersion: "1"
}));

// Create a view with {apiStrict: true}.
db.view.drop();
assert.commandWorked(db.runCommand(
    {create: "view", viewOn: collName, pipeline: [], apiStrict: true, apiVersion: "1"}));
// find() on views should work normally if 'apiStrict' is true.
assert.commandWorked(db.runCommand({find: "view", apiStrict: true, apiVersion: "1"}));

st.stop();
})();
