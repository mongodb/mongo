// Test that setting readOnly mode on the command line causes readOnly to be properly set in both
// isMaster and serverStatus output.
(function() {
    "use strict";

    var mongod = MongoRunner.runMongod();

    // ensure dbpath gets set up.
    assert.writeOK(mongod.getDB("foo").x.insert({x:1}));

    assert(!mongod.getDB("admin").isMaster().readOnly);
    assert(!mongod.getDB("admin").serverStatus().storageEngine.readOnly);
    MongoRunner.stopMongod(mongod);

    // TODO: uncomment when readOnly mode is implemented in mmapv1.
    //
    // mongod = MongoRunner.runMongod({readOnly: ""});
    // assert(mongod.getDB("admin").isMaster().readOnly);
    // assert(mongod.getDB("admin").serverStatus().storageEngine.readOnly);
    // MongoRunner.stopMongod(mongod);
}());
