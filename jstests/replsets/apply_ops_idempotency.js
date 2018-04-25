(function() {
    'use strict';
    const debug = 0;

    let rst = new ReplSetTest({name: "applyOpsIdempotency", nodes: 1});
    rst.startSet();
    rst.initiate();

    /**
     * Returns true if this database contains any drop-pending collections.
     */
    function containsDropPendingCollection(mydb) {
        const res =
            assert.commandWorked(mydb.runCommand("listCollections", {includePendingDrops: true}));
        const collectionInfos = res.cursor.firstBatch;
        const collectionNames = collectionInfos.map(c => c.name);
        return Boolean(collectionNames.find(c => c.indexOf('system.drop.') == 0));
    }

    /**
     *  Apply ops on mydb, asserting success.
     */
    function assertApplyOpsWorks(testdbs, ops) {
        // Remaining operations in ops must still be applied
        while (ops.length) {
            let cmd = {applyOps: ops};
            let res = testdbs[0].adminCommand(cmd);
            if (debug) {
                printjson({applyOps: ops, res});
            }

            // Wait for any drop-pending collections to be removed by the reaper before proceeding.
            assert.soon(function() {
                return !testdbs.find(mydb => containsDropPendingCollection(mydb));
            });

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

    /**
     *  Gather collection info and dbHash results of each of the passed databases.
     */
    function dbInfo(dbs) {
        return dbs.map((db) => {
            return {name: db.getName(), info: db.getCollectionInfos(), md5: dbHash(db)};
        });
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
        renameCollectionWithinDatabaseDroppingTargetByUUID: (mydb) => {
            assert.commandWorked(mydb.createCollection("x"));
            assert.commandWorked(mydb.createCollection("y"));
            assert.commandWorked(mydb.createCollection("z"));

            assert.commandWorked(mydb.x.renameCollection('xx'));
            // When replayed on a up-to-date db, this oplog entry may drop
            // collection z rather than collection x if the dropTarget is not
            // specified by UUID. (See SERVER-33087)
            assert.commandWorked(mydb.y.renameCollection('xx', true));
            assert.commandWorked(mydb.xx.renameCollection('yy'));
            assert.commandWorked(mydb.z.renameCollection('xx'));
        },
        renameCollectionWithinDatabaseDropTargetEvenWhenSourceIsEmpty: (mydb) => {
            assert.commandWorked(mydb.createCollection("x"));
            assert.commandWorked(mydb.createCollection("y"));
            assert.commandWorked(mydb.x.renameCollection('y', true));
            assert(mydb.y.drop());
        },
        renameCollectionAcrossDatabases: (mydb) => {
            let otherdb = mydb.getSiblingDB(mydb + '_');
            let [x, y] = getCollections(mydb, ['x', 'y']);
            let [z] = getCollections(otherdb, ['z']);
            assert.writeOK(x.insert({_id: 1, x: 1}));
            assert.writeOK(y.insert({_id: 1, y: 1}));

            assert.commandWorked(
                mydb.adminCommand({renameCollection: x.getFullName(), to: z.getFullName()}));
            assert.writeOK(z.insert({_id: 2, x: 2}));
            assert.writeOK(x.insert({_id: 2, x: false}));
            assert.writeOK(y.insert({y: 2}));

            assert.commandWorked(mydb.adminCommand(
                {renameCollection: y.getFullName(), to: x.getFullName(), dropTarget: true}));
            assert.commandWorked(
                mydb.adminCommand({renameCollection: z.getFullName(), to: y.getFullName()}));
            return [mydb, otherdb];
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
     *  all different suffixes of the oplog and check for the correct hash. If testFun creates
     *  additional databases, it should return an array with all databases to check.
     */
    function testIdempotency(primary, testFun, testName) {
        // Create a new database name, so it's easier to filter out our oplog records later.
        let dbname = (new Date()).toISOString().match(/[-0-9T]/g).join('');  // 2017-05-30T155055713
        let mydb = primary.getDB(dbname);

        // Allow testFun to return the array of databases to check (default is mydb).
        let testdbs = testFun(mydb) || [mydb];
        let expectedInfo = dbInfo(testdbs);

        let oplog = mydb.getSiblingDB('local').oplog.rs;
        let ops = oplog
                      .find({op: {$ne: 'n'}, ns: new RegExp('^' + mydb.getName())},
                            {ts: 0, t: 0, h: 0, v: 0})
                      .toArray();
        assert.gt(ops.length, 0, 'Could not find any matching ops in the oplog');
        testdbs.forEach((db) => assert.commandWorked(db.dropDatabase()));

        if (debug) {
            print(testName + ': replaying suffixes of ' + ops.length + ' operations');
            printjson(ops);
        }

        for (let j = 0; j < ops.length; j++) {
            let replayOps = ops.slice(j);
            assertApplyOpsWorks(testdbs, replayOps);
            let actualInfo = dbInfo(testdbs);
            assert.eq(actualInfo,
                      expectedInfo,
                      'unexpected differences between databases after replaying final ' +
                          replayOps.length + ' ops in test ' + testName + ": " + tojson(replayOps));
        }
    }

    for (let f in tests)
        testIdempotency(rst.getPrimary(), tests[f], f);

    rst.stopSet();
})();
