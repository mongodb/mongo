/**
 * Ensures that a server started with --shardsvr or --configsvr must also be started with --replSet,
 * unless it is started with enableTestCommands=1.
 */
(function() {
    jsTest.setOption('enableTestCommands', false);

    // Standalone tests. If the server fails to start, MongoRunner.runMongod returns null.

    jsTest.log("Ensure starting a standalone with --shardsvr fails");
    let standaloneWithShardsvr = MongoRunner.runMongod({shardsvr: ''});
    assert.eq(null, standaloneWithShardsvr);

    jsTest.log("Ensure starting a standalone with --shardsvr and enableTestCommands=1 succeeds");
    let standaloneWithShardsvrForTest =
        MongoRunner.runMongod({shardsvr: '', setParameter: "enableTestCommands=1"});
    assert.neq(null, standaloneWithShardsvrForTest);
    MongoRunner.stopMongod(standaloneWithShardsvrForTest);

    jsTest.log("Ensure starting a standalone with --configsvr fails");
    let standaloneWithConfigsvr = MongoRunner.runMongod({configsvr: ''});
    assert.eq(null, standaloneWithConfigsvr);

    jsTest.log("Ensure starting a standalone with --configsvr and enableTestCommands=1 succeeds");
    let standaloneWithConfigsvrForTest =
        MongoRunner.runMongod({configsvr: '', setParameter: "enableTestCommands=1"});
    assert.neq(null, standaloneWithConfigsvrForTest);
    MongoRunner.stopMongod(standaloneWithConfigsvrForTest);

    // Replset tests. If any server fails to start, ReplSetTest.startSet throws an error.

    jsTest.log("Ensure starting a replset with --shardsvr succeeds");
    let replsetWithShardsvr = new ReplSetTest({nodes: 1});
    replsetWithShardsvr.startSet({shardsvr: ''});
    replsetWithShardsvr.stopSet();

    jsTest.log("Ensure starting a replset with --shardsvr and enableTestCommands=1 succeeds");
    let replsetWithShardsvrForTest = new ReplSetTest({nodes: 1});
    replsetWithShardsvrForTest.startSet({shardsvr: '', setParameter: "enableTestCommands=1"});
    replsetWithShardsvrForTest.stopSet();

    jsTest.log("Ensure starting a replset with --configsvr succeeds");
    let replsetWithConfigsvr = new ReplSetTest({nodes: 1});
    replsetWithConfigsvr.startSet({configsvr: ''});
    replsetWithConfigsvr.stopSet();

    jsTest.log("Ensure starting a replset with --configsvr and enableTestCommands=1 succeeds");
    let replsetWithConfigsvrForTest = new ReplSetTest({nodes: 1});
    replsetWithConfigsvrForTest.startSet({configsvr: '', setParameter: "enableTestCommands=1"});
    replsetWithConfigsvrForTest.stopSet();

})();
