/**
 * apply_ops_wc.js
 *
 * This file tests SERVER-22270 that applyOps commands should take a writeConcern.
 * This first tests that invalid write concerns cause writeConcern errors.
 * Next, it tests replication with writeConcerns of w:2 and w:majority.
 * When there are 3 nodes up in a replica set, applyOps commands succeed.
 * It then stops replication at one seconday and confirms that applyOps commands still succeed.
 * It finally stops replication at another secondary and confirms that applyOps commands fail.
 */

(function() {
    "use strict";
    var nodeCount = 3;
    var replTest = new ReplSetTest({name: 'applyOpsWCSet', nodes: nodeCount});
    replTest.startSet();
    var cfg = replTest.getReplSetConfig();
    cfg.settings = {};
    cfg.settings.chainingAllowed = false;
    replTest.initiate(cfg);

    var testDB = "applyOps-wc-test";

    // Get test collection.
    var master = replTest.getPrimary();
    var db = master.getDB(testDB);
    var coll = db.apply_ops_wc;

    function dropTestCollection() {
        coll.drop();
        assert.eq(0, coll.find().itcount(), "test collection not empty");
    }

    dropTestCollection();

    // Set up the applyOps command.
    var applyOpsReq = {
        applyOps: [
            {op: "i", ns: coll.getFullName(), o: {_id: 2, x: "b"}},
            {op: "i", ns: coll.getFullName(), o: {_id: 3, x: "c"}},
            {op: "i", ns: coll.getFullName(), o: {_id: 4, x: "d"}},
        ]
    };

    function assertApplyOpsCommandWorked(res) {
        assert.eq(3, res.applied);
        assert.commandWorked(res);
        assert.eq([true, true, true], res.results);
    }

    function assertWriteConcernError(res) {
        assert(res.writeConcernError);
        assert(res.writeConcernError.code);
        assert(res.writeConcernError.errmsg);
    }

    var invalidWriteConcerns = [{w: 'invalid'}, {w: nodeCount + 1}];

    function testInvalidWriteConcern(wc) {
        jsTest.log("Testing invalid write concern " + tojson(wc));

        applyOpsReq.writeConcern = wc;
        var res = coll.runCommand(applyOpsReq);
        assertApplyOpsCommandWorked(res);
        assertWriteConcernError(res);
    }

    // Verify that invalid write concerns yield an error.
    coll.insert({_id: 1, x: "a"});
    invalidWriteConcerns.forEach(testInvalidWriteConcern);

    var secondaries = replTest.getSecondaries();

    var majorityWriteConcerns = [
        {w: 2, wtimeout: 30000},
        {w: 'majority', wtimeout: 30000},
    ];

    function testMajorityWriteConcerns(wc) {
        jsTest.log("Testing " + tojson(wc));

        // Reset secondaries to ensure they can replicate.
        secondaries[0].getDB('admin').runCommand(
            {configureFailPoint: 'rsSyncApplyStop', mode: 'off'});
        secondaries[1].getDB('admin').runCommand(
            {configureFailPoint: 'rsSyncApplyStop', mode: 'off'});

        // Set the writeConcern of the applyOps command.
        applyOpsReq.writeConcern = wc;

        dropTestCollection();

        // applyOps with a full replica set should succeed.
        coll.insert({_id: 1, x: "a"});
        var res = db.runCommand(applyOpsReq);

        assertApplyOpsCommandWorked(res);
        assert(!res.writeConcernError,
               'applyOps on a full replicaset had writeConcern error ' +
                   tojson(res.writeConcernError));

        dropTestCollection();

        // Stop replication at one secondary.
        secondaries[0].getDB('admin').runCommand(
            {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

        // applyOps should succeed with only 1 node not replicating.
        coll.insert({_id: 1, x: "a"});
        res = db.runCommand(applyOpsReq);

        assertApplyOpsCommandWorked(res);
        assert(!res.writeConcernError,
               'applyOps on a replicaset with 2 working nodes had writeConcern error ' +
                   tojson(res.writeConcernError));

        dropTestCollection();

        // Stop replication at a second secondary.
        secondaries[1].getDB('admin').runCommand(
            {configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'});

        // applyOps should fail after two nodes have stopped replicating.
        coll.insert({_id: 1, x: "a"});
        applyOpsReq.writeConcern.wtimeout = 5000;
        res = db.runCommand(applyOpsReq);

        assertApplyOpsCommandWorked(res);
        assertWriteConcernError(res);
    }

    majorityWriteConcerns.forEach(testMajorityWriteConcerns);

})();
