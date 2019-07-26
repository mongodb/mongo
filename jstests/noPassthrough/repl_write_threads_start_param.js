// This test ensures that the replWriterThreadCount server parameter:
//       1) cannot be less than 1
//       2) cannot be greater than 256
//       3) is actually set to the passed in value
//       4) cannot be altered at run time

(function() {
"use strict";

// too low a count
clearRawMongoProgramOutput();
var mongo = MongoRunner.runMongod({setParameter: 'replWriterThreadCount=0'});
assert.soon(function() {
    return rawMongoProgramOutput().match(
        "Invalid value for parameter replWriterThreadCount: 0 is not greater than or equal to 1");
}, "mongod started with too low a value for replWriterThreadCount");

// too high a count
clearRawMongoProgramOutput();
mongo = MongoRunner.runMongod({setParameter: 'replWriterThreadCount=257'});
assert.soon(function() {
    return rawMongoProgramOutput().match(
        "Invalid value for parameter replWriterThreadCount: 257 is not less than or equal to 256");
}, "mongod started with too high a value for replWriterThreadCount");

// proper count
clearRawMongoProgramOutput();
mongo = MongoRunner.runMongod({setParameter: 'replWriterThreadCount=24'});
assert.neq(null, mongo, "mongod failed to start with a suitable replWriterThreadCount value");
assert(!rawMongoProgramOutput().match("Invalid value for parameter replWriterThreadCount"),
       "despite accepting the replWriterThreadCount value, mongod logged an error");

// getParameter to confirm the value was set
var result = mongo.getDB("admin").runCommand({getParameter: 1, replWriterThreadCount: 1});
assert.eq(24, result.replWriterThreadCount, "replWriterThreadCount was not set internally");

// setParameter to ensure it is not possible
assert.commandFailed(mongo.getDB("admin").runCommand({setParameter: 1, replWriterThreadCount: 1}));
MongoRunner.stopMongod(mongo);
}());
