// Validate that starting a replica set with auth enabled requires a keyfile
(function() {
'use strict';

const rsTest = new ReplSetTest({nodes: 1});

clearRawMongoProgramOutput();

assert.throws(function() {
    rsTest.startSet({auth: "", oplogSize: 10});
});

const mongoOutput = rawMongoProgramOutput();
assert(mongoOutput.indexOf(
           "security.keyFile is required when authorization is enabled with replica sets") >= 0,
       "Expected error message about missing keyFile on startup");
})();
