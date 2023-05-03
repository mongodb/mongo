/**
 * Test that $telemetry properly applies hmac to find commands, on mongod and mongos.
 */
load("jstests/libs/telemetry_utils.js");
(function() {
"use strict";

const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";
const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

function runTest(conn) {
    const db = conn.getDB("test");
    const admin = conn.getDB("admin");

    db.test.drop();
    db.test.insert({v: 1});

    db.test.find({v: 1}).toArray();

    let telemetry = getTelemetryRedacted(admin);

    assert.eq(1, telemetry.length);
    assert.eq(kHashedCollName, telemetry[0].key.queryShape.find);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, telemetry[0].key.queryShape.filter);

    db.test.insert({v: 2});

    const cursor = db.test.find({v: {$gt: 0, $lt: 3}}).batchSize(1);
    telemetry = getTelemetryRedacted(admin);
    // Cursor isn't exhausted, so there shouldn't be another entry yet.
    assert.eq(1, telemetry.length);

    assert.commandWorked(
        db.runCommand({getMore: cursor.getId(), collection: db.test.getName(), batchSize: 2}));

    telemetry = getTelemetryRedacted(admin);
    assert.eq(2, telemetry.length);
    assert.eq(kHashedCollName, telemetry[1].key.queryShape.find);
    assert.eq({
        "$and": [{[kHashedFieldName]: {"$gt": "?number"}}, {[kHashedFieldName]: {"$lt": "?number"}}]
    },
              telemetry[1].key.queryShape.filter);
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryConfigureTelemetrySamplingRate: -1,
        featureFlagTelemetry: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryConfigureTelemetrySamplingRate: -1,
            featureFlagTelemetry: true,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
        }
    },
});
runTest(st.s);
st.stop();
}());
