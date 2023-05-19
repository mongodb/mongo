/**
 * Tests that serverStatus correctly returns repl.isWritablePrimary instead of repl.ismaster.
 */
(function() {
"use strict";
const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
const primary = replTest.getPrimary();

const serverStatusMetricsRepl = primary.adminCommand({serverStatus: 1}).repl;
assert.eq(serverStatusMetricsRepl.isWritablePrimary, true, "repl.isWritablePrimary should be true");
assert.eq(
    serverStatusMetricsRepl.hasOwnProperty('ismaster'), false, "repl.ismaster should be undefined");
replTest.stopSet();
})();
