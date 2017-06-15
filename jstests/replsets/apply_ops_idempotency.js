(function() {
    'use strict';
    const debug = 0;

    let rst = new ReplSetTest({
        name: "applyOpsIdempotency",
        nodes: 1,
        nodeOptions: {setParameter: "enableCollectionUUIDs=1"}
    });
    rst.startSet();
    rst.initiate();

    /**
     *  Apply ops on mydb, asserting success.
     */
    function assertApplyOpsWorks(mydb, ops) {
        // Remaining operations in ops must still be applied
        while (ops.length) {
            let cmd = {applyOps: ops};
            let res = mydb.adminCommand(cmd);
            // If the entire operation succeeded, we're done.
            if (res.ok == 1)
                return res;

            // Skip any operations that succeeded.
            while (res.applied-- && res.results.shift())
                ops.shift();

            // These errors are expected when replaying operations and should be ignored.
            if (res.code == ErrorCodes.NamespaceNotFound || res.code == ErrorCodes.DuplicateKey) {
                ops.shift();
                continue;
            }

            // Generate the appropriate error message.
            assert.commandWorked(res, tojson(cmd));
        }
    }

    /**
     *  Run the dbHash command on mydb, assert it worked and return the md5.
     */
    function dbHash(mydb) {
        let cmd = {dbHash: 1};
        let res = mydb.runCommand(cmd);
        assert.commandWorked(res, tojson(cmd));
        return res.md5;
    }

    var getCollections = (mydb, prefixes) => prefixes.map((prefix) => mydb[prefix]);

    /**
     *  Test functions to run and test using replay of oplog.
     */
    var tests = {
        crud: (mydb) => {
            let [x, y, z] = getCollections(mydb, ['x', 'y', 'z']);
            assert.writeOK(x.insert({_id: 1}));
            assert.writeOK(x.update({_id: 1}, {$set: {x: 1}}));
            assert.writeOK(x.remove({_id: 1}));

            assert.writeOK(y.update({_id: 1}, {y: 1}));
            assert.writeOK(y.insert({_id: 2, y: false, z: false}));
            assert.writeOK(y.update({_id: 2}, {y: 2}));

            assert.writeOK(z.insert({_id: 1, z: 1}));
            assert.writeOK(z.remove({_id: 1}));
            assert.writeOK(z.insert({_id: 1}));
            assert.writeOK(z.insert({_id: 2, z: 2}));
        },
        renameCollectionWithinDatabase: (mydb) => {
            let [x, y, z] = getCollections(mydb, ['x', 'y', 'z']);
            assert.writeOK(x.insert({_id: 1, x: 1}));
            assert.writeOK(y.insert({_id: 1, y: 1}));

            assert.commandWorked(x.renameCollection(z.getName()));
            assert.writeOK(z.insert({_id: 2, x: 2}));
            assert.writeOK(x.insert({_id: 2, x: false}));
            assert.writeOK(y.insert({y: 2}));

            assert.commandWorked(y.renameCollection(x.getName(), true));
            assert.commandWorked(z.renameCollection(y.getName()));
        },
        renameCollectionAcrossDatabase: (mydb) => {
            let otherdb = mydb.getSiblingDB(mydb + '_');
            let [x, y] = getCollections(mydb, ['x', 'y']);
            let [z] = getCollections(otherdb, ['z']);
            assert.writeOK(x.insert({_id: 1, x: 1}));
            assert.writeOK(y.insert({_id: 1, y: 1}));

            assert.commandWorked(x.renameCollection(z.getName()));
            assert.writeOK(z.insert({_id: 2, x: 2}));
            assert.writeOK(x.insert({_id: 2, x: false}));
            assert.writeOK(y.insert({y: 2}));

            assert.commandWorked(y.renameCollection(x.getName(), true));
            assert.commandWorked(z.renameCollection(y.getName()));

        },
        createIndex: (mydb) => {
            let [x, y] = getCollections(mydb, ['x', 'y']);
            assert.commandWorked(x.createIndex({x: 1}));
            assert.writeOK(x.insert({_id: 1, x: 1}));
            assert.writeOK(y.insert({_id: 1, y: 1}));
            assert.commandWorked(y.createIndex({y: 1}));
            assert.writeOK(y.insert({_id: 2, y: 2}));
        },
    };

    /**
     *  Create a new uniquely named database, execute testFun and compute the dbHash. Then replay
     *  all different suffixes of the oplog and check for the correct hash.
     */
    function testIdempotency(primary, testFun, testName) {
        // Create a new database name, so it's easier to filter out our oplog records later.
        let dbname = (new Date()).toISOString().match(/[-0-9T]/g).join('');  // 2017-05-30T155055713
        let mydb = primary.getDB(dbname);
        testFun(mydb);
        let expectedMD5 = dbHash(mydb);
        let expectedInfos = mydb.getCollectionInfos();

        let oplog = mydb.getSiblingDB('local').oplog.rs;
        let ops = oplog
                      .find({op: {$ne: 'n'}, ns: new RegExp('^' + mydb.getName())},
                            {ts: 0, t: 0, h: 0, v: 0})
                      .toArray();
        assert.gt(ops.length, 0, 'Could not find any matching ops in the oplog');
        assert.commandWorked(mydb.dropDatabase());

        if (debug) {
            print(testName + ': replaying suffixes of ' + ops.length + ' operations');
            ops.forEach((op) => printjsononeline({op}));
        }

        for (let j = 0; j < ops.length; j++) {
            let replayOps = ops.slice(j);
            assertApplyOpsWorks(mydb, replayOps);
            let actualMD5 = dbHash(mydb);
            assert.eq(
                actualMD5,
                expectedMD5,
                'unexpected dbHash result after replaying final ' + replayOps.length + ' ops');
            let actualInfos = mydb.getCollectionInfos();
            assert.eq(actualInfos,
                      expectedInfos,
                      'unexpected differences in collection information after replaying final ' +
                          replayOps.length + ' ops');
        }
    }

    for (let f in tests)
        testIdempotency(rst.getPrimary(), tests[f], f);
})();
