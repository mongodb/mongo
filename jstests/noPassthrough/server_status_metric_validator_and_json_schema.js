/**
 * Tests for serverStatus metrics.commands.<name>.validator stats.
 * @tags: [requires_sharding]
 *
 */
(function() {
"use strict";

function runCommandAndCheckValidatorCount({cmdToRun, cmdName, countDict, error, notFirst}) {
    let metricsBeforeCommandInvoked = {failed: 0, jsonSchema: 0, total: 0};
    if (notFirst) {
        metricsBeforeCommandInvoked = db.serverStatus().metrics.commands[cmdName].validator;
    }
    if (error) {
        cmdToRun();
    } else {
        assert.commandWorked(cmdToRun());
    }
    const metricsAfterCommandInvoked = db.serverStatus().metrics.commands[cmdName].validator;
    assert.eq(metricsAfterCommandInvoked.total - metricsBeforeCommandInvoked.total,
              countDict.total,
              metricsAfterCommandInvoked);
    assert.eq(metricsAfterCommandInvoked.failed - metricsBeforeCommandInvoked.failed,
              countDict.failed,
              metricsAfterCommandInvoked);
    assert.eq(metricsAfterCommandInvoked.jsonSchema - metricsBeforeCommandInvoked.jsonSchema,
              countDict.jsonSchema,
              metricsAfterCommandInvoked);
}

function runTests(db, collName, collCount) {
    //   Test that validator count is 0 if no validator specified.
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.createCollection(collName),
        cmdName: "create",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(0)},
        error: false,
        notFirst: false
    });
    collCount++;
    assert.commandWorked(db[collName].createIndex({a: 1}, {expireAfterSeconds: 1800}));
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand(
            {collMod: collName, index: {keyPattern: {a: 1}, expireAfterSeconds: 3600}}),
        cmdName: "collMod",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(0)},
        error: false,
        notFirst: false
    });

    // Test that validator total and failed count increments when a $jsonSchema validation error is
    // raised.
    let schema = {
        '$jsonSchema': {
            'bsonType': "object",
            'properties': {
                'a': "",
                'b': {
                    'bsonType': "number",
                },
                'c': {
                    'bsonType': "number",
                },
            }
        }
    };
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand({"create": collName + collCount, validator: schema}),
        cmdName: "create",
        countDict: {failed: NumberLong(1), jsonSchema: NumberLong(1), total: NumberLong(1)},
        error: true,
        notFirst: true
    });
    collCount++;

    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand({collMod: collName, validator: schema}),
        cmdName: "collMod",
        countDict: {failed: NumberLong(1), jsonSchema: NumberLong(1), total: NumberLong(1)},
        error: true,
        notFirst: true
    });

    // Test that validator total count increments, but not failed count for valid $jsonSchema.
    schema = {
        '$jsonSchema': {
            'bsonType': "object",
            'properties': {
                'a': {
                    'bsonType': "number",
                },
                'b': {
                    'bsonType': "number",
                },
                'c': {
                    'bsonType': "number",
                },
            }
        }
    };

    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.createCollection(collName + collCount, {validator: schema}),
        cmdName: "create",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(1), total: NumberLong(1)},
        error: false,
        notFirst: true
    });
    collCount++;
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand({collMod: collName, validator: schema}),
        cmdName: "collMod",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(1), total: NumberLong(1)},
        error: false,
        notFirst: true
    });

    // Test that only the validator 'total' count gets incremented with match expression validator.
    // Neither the 'find' nor 'jsonSchema' fields should be incremented.
    schema = {$expr: {$eq: ["$a", {$multiply: ["$v", {$sum: [1, "$c"]}]}]}};
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.createCollection(collName + collCount, {validator: schema}),
        cmdName: "create",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(1)},
        error: false,
        notFirst: true
    });
    collCount++;

    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand({collMod: collName, validator: schema}),
        cmdName: "collMod",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(1)},
        error: false,
        notFirst: true
    });

    // Test that validator count does not increment with empty validator object.
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.createCollection(collName + collCount, {validator: {}}),
        cmdName: "create",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(0)},
        error: false,
        notFirst: true
    });
    collCount++;
    runCommandAndCheckValidatorCount({
        cmdToRun: () => db.runCommand({collMod: collName, validator: {}}),
        cmdName: "collMod",
        countDict: {failed: NumberLong(0), jsonSchema: NumberLong(0), total: NumberLong(0)},
        error: false,
        notFirst: true
    });
}

// Standalone
const conn = MongoRunner.runMongod({});
assert.neq(conn, null, "mongod failed to start");
let db = conn.getDB(jsTestName());
let collCount = 0;
const collName = jsTestName();
runTests(db, collName, collCount);

MongoRunner.stopMongod(conn);

//  Sharded cluster
const st = new ShardingTest({shards: 2});
assert.commandWorked(st.s.adminCommand({enableSharding: jsTestName()}));
st.ensurePrimaryShard(jsTestName(), st.shard0.shardName);
db = st.rs0.getPrimary().getDB(jsTestName());
collCount = 0;
runTests(db, collName, collCount);

st.stop();
}());
