// This test ensures that the replWriterThreadCount server parameter://
//       1) cannot be greater than 256 (0 - special case, meaning the value is ignored and thread
//          count is set to twice number of cores)
//       2) is actually set to the passed in value
//       3) cannot be altered at run time

// too high a count
clearRawMongoProgramOutput();
assert.throws(() => MongoRunner.runMongod({setParameter: 'replWriterThreadCount=257'}));
assert(rawMongoProgramOutput("Invalid value for parameter replWriterThreadCount: ")
           .match("257 is not less than or equal to 256"),
       "mongod started with too high a value for replWriterThreadCount");

// proper count
clearRawMongoProgramOutput();
let mongo = MongoRunner.runMongod({setParameter: 'replWriterThreadCount=24'});
assert.neq(null, mongo, "mongod failed to start with a suitable replWriterThreadCount value");
assert(rawMongoProgramOutput("Invalid value for parameter replWriterThreadCount").length == 0,
       "despite accepting the replWriterThreadCount value, mongod logged an error");

// getParameter to confirm the value was set
var result = mongo.getDB("admin").runCommand({getParameter: 1, replWriterThreadCount: 1});
assert.eq(24, result.replWriterThreadCount, "replWriterThreadCount was not set internally");

// setParameter to ensure it is not possible
assert.commandFailed(mongo.getDB("admin").runCommand({setParameter: 1, replWriterThreadCount: 1}));
MongoRunner.stopMongod(mongo);