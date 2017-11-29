/**
 * Ensures that a server started with --shardsvr or --configsvr must also be started as a replica
 * set, unless it is started with enableTestCommands=1.
 */
(function() {
    var testAllPermutations = function(enableTestCommands) {
        jsTest.setOption('enableTestCommands', enableTestCommands);
        var mongod;

        // Standalone tests.

        jsTest.log("Starting shardsvr with enableTestCommands=" + enableTestCommands);
        mongod = MongoRunner.runMongod({shardsvr: ''});
        if (enableTestCommands) {
            assert.neq(null, mongod);
            MongoRunner.stopMongod(mongod);
        } else {
            assert.eq(null, mongod);
        }

        jsTest.log("Starting configsvr with enableTestCommands=" + enableTestCommands);
        mongod = MongoRunner.runMongod({configsvr: ''});
        if (enableTestCommands) {
            assert.neq(null, mongod);
            MongoRunner.stopMongod(mongod);
        } else {
            assert.eq(null, mongod);
        }

        // Replica set tests using the command line 'replSet' option.

        jsTest.log("Starting shardsvr with --replSet and enableTestCommands=" + enableTestCommands);
        mongod = MongoRunner.runMongod({shardsvr: '', replSet: 'dummy'});
        assert.neq(null, mongod);
        MongoRunner.stopMongod(mongod);

        jsTest.log("Starting configsvr with --replSet and enableTestCommands=" +
                   enableTestCommands);
        mongod = MongoRunner.runMongod({configsvr: '', replSet: 'dummy'});
        assert.neq(null, mongod);
        MongoRunner.stopMongod(mongod);

        // Replica set tests using the config file 'replSetName' option.

        jsTest.log("Starting shardsvr with 'replication.replSetName' and enableTestCommands=" +
                   enableTestCommands);
        mongod = MongoRunner.runMongod(
            {config: "jstests/libs/config_files/set_shardingrole_shardsvr.json"});
        assert.neq(null, mongod);
        MongoRunner.stopMongod(mongod);

        jsTest.log("Starting configsvr with 'replication.replSetName' and enableTestCommands=" +
                   enableTestCommands);
        mongod = MongoRunner.runMongod(
            {config: "jstests/libs/config_files/set_shardingrole_configsvr.json"});
        assert.neq(null, mongod);
        MongoRunner.stopMongod(mongod);
    };

    testAllPermutations(true /* enableTestCommands */);
    testAllPermutations(false /* enableTestCommands */);
})();
