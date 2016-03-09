load("jstests/replsets/rslib.js");

// Test SERVER-21744 Clients may fail to discover new primaries when clock skew
// between nodes is greater than electionTimeout.
//
// In PV0, the election ID is generated based on Timestamp. On protocol version upgrade,
// the current primary updates its election ID to <max Timestamp, term> which is greater than
// any PV0 election id. On downgrade, the election id will be updated to old PV0 format.
(function() {

    "use strict";

    function checkPV1ElectionId(electionId) {
        var electionIdStr = electionId.valueOf();
        assert.eq(electionIdStr.slice(0, 8), "7fffffff");
        var res = assert.commandWorked(rst.getPrimary().adminCommand({replSetGetStatus: 1}));
        var termStr = "" + res.term;
        assert.eq(electionIdStr.slice(-termStr.length), termStr);
    }

    var name = "election_id";
    var rst = new ReplSetTest({name: name, nodes: 3});

    rst.startSet();
    // Initiate the replset in protocol version 0.
    var conf = rst.getReplSetConfig();
    conf.protocolVersion = 0;
    rst.initiate(conf);
    rst.awaitSecondaryNodes();

    var primary = rst.getPrimary();
    var primaryColl = primary.getDB("test").coll;

    // Do a write, this will set up sync sources on secondaries.
    assert.writeOK(primaryColl.insert({x: 1}, {writeConcern: {w: 3}}));

    var res = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    var oldElectionId = res.repl.electionId;

    // Upgrade protocol version
    //
    conf = rst.getReplSetConfigFromNode();
    conf.protocolVersion = 1;
    conf.version++;
    reconfig(rst, conf);
    // This write will block until all nodes finish upgrade.
    assert.writeOK(primaryColl.insert({x: 2}, {writeConcern: {w: 3}}));

    // Check election id after upgrade
    res = assert.commandWorked(rst.getPrimary().adminCommand({serverStatus: 1}));
    var newElectionId = res.repl.electionId;
    assert.lt(oldElectionId.valueOf(), newElectionId.valueOf());
    checkPV1ElectionId(newElectionId);
    oldElectionId = newElectionId;

    // Step down
    assert.throws(function() {
        var res = primary.adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 30});
        // Error out if stepdown command failed to run and throw.
        printjson(res);
    });
    rst.awaitSecondaryNodes();
    res = assert.commandWorked(rst.getPrimary().adminCommand({serverStatus: 1}));
    var newElectionId = res.repl.electionId;

    // Compare the string of ObjectId
    assert.lt(oldElectionId.valueOf(), newElectionId.valueOf());
    checkPV1ElectionId(newElectionId);
    oldElectionId = newElectionId;

    // Downgrade protocol version
    //
    conf = rst.getReplSetConfigFromNode();
    conf.protocolVersion = 0;
    conf.version++;
    reconfig(rst, conf);
    // This write will block until all nodes finish upgrade.
    assert.writeOK(rst.getPrimary().getDB("test").coll.insert({x: 2}, {writeConcern: {w: 3}}));

    // Check election id after downgrade
    res = assert.commandWorked(rst.getPrimary().adminCommand({serverStatus: 1}));
    var newElectionId = res.repl.electionId;
    // new election id in PV0 is less than the old one in PV1.
    assert.gt(oldElectionId.valueOf(), newElectionId.valueOf());
    oldElectionId = newElectionId;

    // Step down
    assert.throws(function() {
        var res =
            rst.getPrimary().adminCommand({replSetStepDown: 60, secondaryCatchUpPeriodSecs: 30});
        // Error out if stepdown command failed to run and throw.
        printjson(res);
    });
    rst.awaitSecondaryNodes();
    res = assert.commandWorked(rst.getPrimary().adminCommand({serverStatus: 1}));
    var newElectionId = res.repl.electionId;
    assert.lt(oldElectionId.valueOf(), newElectionId.valueOf());
    oldElectionId = newElectionId;

})();
