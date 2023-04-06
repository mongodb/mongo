/**
 * Test that applicationName and namespace appear in telemetry for the find command.
 * @tags: [featureFlagTelemetry]
 */
(function() {
"use strict";

const kApplicationName = "MongoDB Shell";
const kHashedApplicationName = "dXRuJCwctavU";

const getTelemetry = (conn, redactIdentifiers = false) => {
    const result = assert.commandWorked(conn.adminCommand({
        aggregate: 1,
        pipeline: [
            {$telemetry: {redactIdentifiers}},
            // Sort on telemetry key so entries are in a deterministic order.
            {$sort: {key: 1}},
            {$match: {"key.applicationName": {$in: [kApplicationName, kHashedApplicationName]}}},
            {$match: {"key.find": {$exists: true}}}
        ],
        cursor: {batchSize: 10}
    }));
    return result.cursor.firstBatch;
};

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryConfigureTelemetrySamplingRate: -1},
};

const conn = MongoRunner.runMongod(options);
conn.setLogLevel(3, "query");
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

coll.insert({v: 1});
coll.insert({v: 2});
coll.insert({v: 3});

coll.find({v: 1}).toArray();

let telemetry = getTelemetry(conn);
assert.eq(1, telemetry.length, telemetry);
assert.eq({
    cmdNs: {db: testDB.getName(), coll: coll.getName()},
    find: coll.getName(),
    filter: {v: {"$eq": "?"}},
    applicationName: kApplicationName
},
          telemetry[0].key,
          telemetry);

telemetry = getTelemetry(conn, true);
assert.eq(1, telemetry.length, telemetry);
const hashedColl = "zF15YAUWbyIP";
assert.eq({
    cmdNs: {db: "n4bQgYhMfWWa", coll: hashedColl},
    find: hashedColl,
    filter: {"TJRIXgwhrmxB": {"$eq": "?"}},
    applicationName: kHashedApplicationName
},
          telemetry[0].key,
          telemetry);

MongoRunner.stopMongod(conn);
}());
