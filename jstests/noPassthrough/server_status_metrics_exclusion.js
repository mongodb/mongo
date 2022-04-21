/**
 * Tests that serverStatus metrics are filtered when requested.
 */
(function() {
"use strict";
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

MongoRunner.stopMongod(mongod);
})();
