/**
 * Test that applicationName and namespace appear in telemetry for the find command.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/telemetry_utils.js");
(function() {
"use strict";

const kApplicationName = "MongoDB Shell";
const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";
const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

// Turn on the collecting of telemetry metrics.
let options = {
    setParameter: {internalQueryStatsSamplingRate: -1},
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
assert.eq(kApplicationName, telemetry[0].key.applicationName, telemetry);

telemetry = getTelemetryRedacted(conn, true);
assert.eq(1, telemetry.length, telemetry);
assert.eq(kApplicationName, telemetry[0].key.applicationName, telemetry);

MongoRunner.stopMongod(conn);
}());
