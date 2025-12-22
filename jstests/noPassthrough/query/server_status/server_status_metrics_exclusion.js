/**
 * Tests that serverStatus metrics are filtered when requested.
 */
const mongod = MongoRunner.runMongod();
const dbName = jsTestName();
const db = mongod.getDB(dbName);

// Verify some assumptions about metrics structure for later tests.
let serverStatusMetrics = db.serverStatus().metrics;

assert(serverStatusMetrics.document.hasOwnProperty("deleted"));
assert(serverStatusMetrics.document.hasOwnProperty("inserted"));
assert(serverStatusMetrics.dotsAndDollarsFields.hasOwnProperty("inserts"));
assert(serverStatusMetrics.dotsAndDollarsFields.hasOwnProperty("updates"));

// Exclude the "document.deleted" field.
serverStatusMetrics = db.serverStatus({metrics: {document: {deleted: false}}}).metrics;

assert(!serverStatusMetrics.document.hasOwnProperty("deleted"));
assert(serverStatusMetrics.document.hasOwnProperty("inserted"));

// Exclude the "document.deleted" and "document.inserted" fields.
serverStatusMetrics =
    db.serverStatus({metrics: {document: {deleted: false, inserted: false}}}).metrics;

assert(!serverStatusMetrics.document.hasOwnProperty("deleted"));
assert(!serverStatusMetrics.document.hasOwnProperty("inserted"));

// Exclude the "document.deleted" and "dotsAndDollarsFields.inserts" fields.
serverStatusMetrics =
    db.serverStatus({
          metrics: {document: {deleted: false}, dotsAndDollarsFields: {inserts: false}}
      }).metrics;

assert(!serverStatusMetrics.document.hasOwnProperty("deleted"));
assert(!serverStatusMetrics.dotsAndDollarsFields.hasOwnProperty("inserts"));

// Include a "true" value for the "document.deleted" field. It should be included (ie, no-op).
serverStatusMetrics = db.serverStatus({metrics: {document: {deleted: true}}}).metrics;

assert(serverStatusMetrics.document.hasOwnProperty("deleted"));
assert(serverStatusMetrics.document.hasOwnProperty("inserted"));

// Attempt a non-boolean values which should be rejected (uassert).
assert.commandFailedWithCode(db.serverStatus({metrics: {document: "Non-boolean"}}),
                             [ErrorCodes.InvalidBSONType]);

assert.commandFailedWithCode(db.serverStatus({metrics: {document: {deleted: "Non-boolean"}}}),
                             [ErrorCodes.InvalidBSONType]);

assert.commandFailedWithCode(db.serverStatus({metrics: {document: {deleted: ["Non-boolean"]}}}),
                             [ErrorCodes.InvalidBSONType]);

assert.commandFailedWithCode(
    db.serverStatus({metrics: {document: {deleted: {invalidObjectAtLeftLevel: 1}}}}),
    [ErrorCodes.InvalidBSONType]);

// Exclude the "document" subtree.
serverStatusMetrics = db.serverStatus({metrics: {document: false}}).metrics;

assert(!serverStatusMetrics.hasOwnProperty("document"));

// The {none: 1} specifier should reduce the result to minimal fields.
let serverStatusOutput = db.serverStatus({none: 1});
const baseKeys = [
    "host",
    "version",
    "process",
    "service",
    "pid",
    "uptime",
    "uptimeMillis",
    "uptimeEstimate",
    "localTime",
    "ok"
];
let returnedKeys = Object.keys(serverStatusOutput);
assert.eq(
    returnedKeys.length,
    baseKeys.length,
    () => `serverStatusOutput has ${returnedKeys.length} keys when ${
        baseKeys.length} were expected. Actual keys: ${JSON.stringify(returnedKeys)}`,
);
assert.sameMembers(returnedKeys, baseKeys, JSON.stringify(returnedKeys));

// Check explicit field inclusion with {none: 1}.
serverStatusOutput = assert.commandWorked(db.serverStatus({none: 1, indexBulkBuilder: 1}));
returnedKeys = Object.keys(serverStatusOutput);
let expectedKeys = baseKeys.concat("indexBulkBuilder");
assert.eq(
    returnedKeys.length,
    expectedKeys.length,
    () => `serverStatusOutput has ${returnedKeys.length} keys when ${
        expectedKeys.length} were expected. Actual keys: ${JSON.stringify(returnedKeys)}`,
);
assert.sameMembers(returnedKeys, expectedKeys, JSON.stringify(returnedKeys));

// Check explicit multi-field inclusions with {none: 1}.
serverStatusOutput =
    assert.commandWorked(db.serverStatus({none: 1, wiredTiger: 1, connections: 1}));
returnedKeys = Object.keys(serverStatusOutput);
expectedKeys = baseKeys.concat("wiredTiger", "connections");
assert.eq(
    returnedKeys.length,
    expectedKeys.length,
    () => `serverStatusOutput has ${returnedKeys.length} keys when ${
        expectedKeys.length} were expected. Actual keys: ${JSON.stringify(returnedKeys)}`,
);
assert.sameMembers(returnedKeys, expectedKeys, JSON.stringify(returnedKeys));

// Check conflicting options {all: 1, none: 1}.
assert.commandFailedWithCode(db.serverStatus({all: 1, none: 1}), [ErrorCodes.InvalidOptions]);

MongoRunner.stopMongod(mongod);
