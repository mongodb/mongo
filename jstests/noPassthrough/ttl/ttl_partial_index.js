// Test that the TTL monitor will correctly use TTL indexes that are also partial indexes.
// SERVER-17984.
import {TTLUtil} from "jstests/libs/ttl/ttl_util.js";

// Launch mongod with shorter TTL monitor sleep interval.
let runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
let coll = runner.getDB("test").ttl_partial_index;
coll.drop();

// Create TTL partial index.
assert.commandWorked(coll.createIndex({x: 1}, {expireAfterSeconds: 0, partialFilterExpression: {z: {$exists: true}}}));

let now = new Date();
assert.commandWorked(coll.insert({x: now, z: 2}));
assert.commandWorked(coll.insert({x: now}));

// Wait for the TTL monitor to run at least twice (in case we weren't finished setting up our
// collection when it ran the first time).
TTLUtil.waitForPass(coll.getDB());

assert.eq(
    0,
    coll
        .find({z: {$exists: true}})
        .hint({x: 1})
        .itcount(),
    "Wrong number of documents in partial index, after TTL monitor run",
);
assert.eq(1, coll.find().itcount(), "Wrong number of documents in collection, after TTL monitor run");
MongoRunner.stopMongod(runner);
