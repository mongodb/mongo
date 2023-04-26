/**
 * Unit test to verify that 'retryOnNetworkError' works correctly for common network connection
 * issues.
 */

(function() {
"use strict";
let node = MongoRunner.runMongod();
let hostname = node.host;

jsTestLog("Test connecting to a healthy node.");
let numRetries = 5;
let sleepMs = 50;
let attempts = 0;
retryOnNetworkError(function() {
    attempts++;
    new Mongo(hostname);
}, numRetries, sleepMs);
assert.eq(attempts, 1);

jsTestLog("Test connecting to a node that is down.");
MongoRunner.stopMongod(node);
attempts = 0;
try {
    retryOnNetworkError(function() {
        attempts++;
        new Mongo(hostname);
    }, numRetries, sleepMs);
} catch (e) {
    jsTestLog("Caught exception after exhausting retries: " + e);
}
assert.eq(attempts, numRetries + 1);

jsTestLog("Test connecting to a node with an invalid hostname.");
let invalidHostname = "very-invalid-host-name";
attempts = 0;
try {
    retryOnNetworkError(function() {
        attempts++;
        new Mongo(invalidHostname);
    }, numRetries, sleepMs);
} catch (e) {
    jsTestLog("Caught exception after exhausting retries: " + e);
}
assert.eq(attempts, numRetries + 1);
}());
