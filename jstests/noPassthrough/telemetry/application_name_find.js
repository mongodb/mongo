/**
 * Test that applicationName and namespace appear in telemetry for the find command.
 * @tags: [featureFlagTelemetry]
 */
(function() {
"use strict";

const kApplicationName = "MongoDB Shell";
const kHashedApplicationName = "piOJ84Zjy9dJP6snMI5X6NQ42VGim3vofK5awkuY5q8=";

const getTelemetry = (conn, applyHmacToIdentifiers = false) => {
    const result = assert.commandWorked(conn.adminCommand({
        aggregate: 1,
        pipeline: [
            {$telemetry: {applyHmacToIdentifiers}},
            // Sort on telemetry key so entries are in a deterministic order.
            {$sort: {key: 1}},
            {$match: {"key.applicationName": {$in: [kApplicationName, kHashedApplicationName]}}},
            {$match: {"key.queryShape.find": {$exists: true}}}
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
    queryShape: {
        cmdNs: {db: testDB.getName(), coll: coll.getName()},
        find: coll.getName(),
        filter: {v: {"$eq": "?number"}},
    },
    applicationName: kApplicationName
},
          telemetry[0].key,
          telemetry);

telemetry = getTelemetry(conn, true);
assert.eq(1, telemetry.length, telemetry);
const hashedColl = "tU+RtrEU9QHrWsxNIL8OUDvfpUdavYkcuw7evPKfxdU=";
assert.eq({
    queryShape: {
        cmdNs: {db: "Q7DO+ZJl+eNMEOqdNQGSbSezn1fG1nRWHYuiNueoGfs=", coll: hashedColl},
        find: hashedColl,
        filter: {"ksdi13D4gc1BJ0Es4yX6QtG6MAwIeNLsCgeGRePOvFE=": {"$eq": "?number"}},
    },
    applicationName: kHashedApplicationName
},
          telemetry[0].key,
          telemetry);

MongoRunner.stopMongod(conn);
}());
