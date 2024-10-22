// Validate that starting a replica set with auth enabled requires a keyfile
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rsTest = new ReplSetTest({nodes: 1});

clearRawMongoProgramOutput();

assert.throws(function() {
    rsTest.startSet({auth: "", oplogSize: 10});
});

const subStr = "security.keyFile is required when authorization is enabled with replica sets";
const mongoOutput = rawMongoProgramOutput(subStr);
assert(mongoOutput.indexOf(subStr) >= 0, "Expected error message about missing keyFile on startup");