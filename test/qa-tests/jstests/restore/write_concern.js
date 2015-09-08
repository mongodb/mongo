(function() {

    load("jstests/configs/replset_28.config.js");

    var toolTest = new ToolTest('write_concern', null);

    rs = new ReplSetTest({
        name: "rpls",
        nodes: 3,
        useHostName: true
    });

    rs.startSet();

    rs.initiate();

    rs.awaitReplication();

    toolTest.master = rs.getMaster()

    jsTest.log("testing that mongorestore deals with write concern");

    function testWriteConcern(exitCode,writeConcern,name) {
        jsTest.log(name);
        ret = toolTest.runTool.apply(
            toolTest,
            ['restore'].
                concat(writeConcern).
                concat(getRestoreTarget(dumpTarget)).
                concat(commonToolArgs)
        );
        assert.eq(exitCode, ret, name);
        dbOne.dropDatabase()
    }

    var member1 = rs.nodes[1].getDB("admin");
    var member2 = rs.nodes[2].getDB("admin");

    var commonToolArgs = getCommonToolArguments();

    var dbOne = rs.nodes[0].getDB("dbOne");
    // create a test collection
    for(var i=0;i<=100;i++){
      dbOne.test.insert({_id:i, x:i*i})
    }

    // dump the data that we'll
    var dumpTarget = 'write_concern_dump';
    resetDbpath(dumpTarget);
    var ret = toolTest.runTool.apply(
        toolTest,
        ['dump'].
            concat(getDumpTarget(dumpTarget)).
            concat(commonToolArgs)
    );
    assert.eq(0, ret);

    // drop the database so it's empty
    dbOne.dropDatabase()

    testWriteConcern(0, [], "restore without write concern to a fully functioning repl-set should succeed");

    testWriteConcern(0, ['--writeConcern=majority'], "restore with majority to a fully functioning repl-set should succeed");

    testWriteConcern(0, ['--writeConcern={w:1,wtimeout:500}'], "restore with w:1,timeout:500 to a fully functioning repl-set should succeed");

    testWriteConcern(0, ['--writeConcern={w:2,wtimeout:500}'], "restore with w:2,timeout:500 to a fully functioning repl-set should succeed");

    jsTest.log("stopping one node from doing any further syncing");
    member1.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

    testWriteConcern(0, ['--writeConcern={w:2,wtimeout:500}'], "restore with w:2,timeout:500 repl-set with 2 working nodes should succeed");

    testWriteConcern(0, ['--writeConcern=majority'], "restore with majority with one working node should succeed");

    testWriteConcern(1, ['--writeConcern={w:3,wtimeout:500}'], "restore with w:3,timeout:500 repl-set with two working nodes should fail");

    jsTest.log("stopping the other slave");
    member2.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

    testWriteConcern(1, ['--writeConcern={w:"majority",wtimeout:500}'], "restore with majority with no working nodes should fail");

    testWriteConcern(1, ['--writeConcern={w:2,wtimeout:500}'], "restore with w:2,timeout:500 to a fully functioning repl-set should succeed");

    testWriteConcern(0, ['--writeConcern={w:1,wtimeout:500}'], "restore with w:1,timeout:500 repl-set with one working nodes should succeed");

    jsTest.log("restore with w:3 concern and no working slaves and no timeout waits until slaves are available");
    pid = _startMongoProgram.apply(null,
        ['mongorestore','--writeConcern={w:3}','--host',rs.nodes[0].host].
            concat(getRestoreTarget(dumpTarget)).
            concat(commonToolArgs)
    );

    sleep(1000);

    assert(checkProgram(pid), "restore with w:3 and no working slaves should not have finished");

    jsTest.log("starting stopped slaves");

    member1.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
    member2.runCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'});

    jsTest.log("waiting for restore to finish");
    ret = waitProgram(pid);
    assert.eq(0, ret, "restore with w:3 should succeed once enough slaves start working");

    dbOne.dropDatabase()

    rs.stopSet();
    toolTest.stop();

}());
