import {ReplSetTest} from "jstests/libs/replsettest.js";

const debug = 0;

let rst = new ReplSetTest({name: "applyOpsIdempotency", nodes: 1});
rst.startSet();
rst.initiate();

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

        // If the entire operation succeeded, we're done.
        if (res.ok == 1) return res;

        // Skip any operations that succeeded.
        while (res.applied-- && res.results.shift()) ops.shift();

        // These errors are expected when replaying operations and should be ignored.
        if (
            res.code == ErrorCodes.NamespaceNotFound ||
            res.code == ErrorCodes.DuplicateKey ||
            res.code == ErrorCodes.UnknownError
        ) {
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

let getCollections = (mydb, prefixes) => prefixes.map((prefix) => mydb[prefix]);

/**
 *  Test functions to run and test using replay of oplog.
 */
let tests = {
    crud: (mydb) => {
        let [x, y, z] = getCollections(mydb, ["x", "y", "z"]);
        assert.commandWorked(x.insert({_id: 1}));
        assert.commandWorked(x.update({_id: 1}, {$set: {x: 1}}));
        assert.commandWorked(x.remove({_id: 1}));

        assert.commandWorked(y.update({_id: 1}, {y: 1}));
        assert.commandWorked(y.insert({_id: 2, y: false, z: false}));
        assert.commandWorked(y.update({_id: 2}, {y: 2}));

        assert.commandWorked(z.insert({_id: 1, z: 1}));
        assert.commandWorked(z.remove({_id: 1}));
        assert.commandWorked(z.insert({_id: 1}));
        assert.commandWorked(z.insert({_id: 2, z: 2}));
    },
    arrayAndSubdocumentFields: (mydb) => {
        let [x, y] = getCollections(mydb, ["x", "y"]);
        // Array field.
        assert.commandWorked(x.insert({_id: 1, x: 1, y: [0]}));
        assert.commandWorked(x.update({_id: 1}, {$set: {x: 2, "y.0": 2}}));
        assert.commandWorked(x.update({_id: 1}, {$set: {y: 3}}));

        // Subdocument field.
        assert.commandWorked(y.insert({_id: 1, x: 1, y: {field: 0}}));
        assert.commandWorked(y.update({_id: 1}, {$set: {x: 2, "y.field": 2}}));
        assert.commandWorked(y.update({_id: 1}, {$set: {y: 3}}));
    },
    renameCollectionWithinDatabase: (mydb) => {
        let [x, y, z] = getCollections(mydb, ["x", "y", "z"]);
        assert.commandWorked(x.insert({_id: 1, x: 1}));
        assert.commandWorked(y.insert({_id: 1, y: 1}));

        assert.commandWorked(x.renameCollection(z.getName()));
        assert.commandWorked(z.insert({_id: 2, x: 2}));
        assert.commandWorked(x.insert({_id: 2, x: false}));
        assert.commandWorked(y.insert({y: 2}));

        assert.commandWorked(y.renameCollection(x.getName(), true));
        assert.commandWorked(z.renameCollection(y.getName()));
    },
    renameCollectionWithinDatabaseDroppingTargetByUUID: (mydb) => {
        assert.commandWorked(mydb.createCollection("x"));
        assert.commandWorked(mydb.createCollection("y"));
        assert.commandWorked(mydb.createCollection("z"));

        assert.commandWorked(mydb.x.renameCollection("xx"));
        // When replayed on a up-to-date db, this oplog entry may drop
        // collection z rather than collection x if the dropTarget is not
        // specified by UUID. (See SERVER-33087)
        assert.commandWorked(mydb.y.renameCollection("xx", true));
        assert.commandWorked(mydb.xx.renameCollection("yy"));
        assert.commandWorked(mydb.z.renameCollection("xx"));
    },
    renameCollectionWithinDatabaseDropTargetEvenWhenSourceIsEmpty: (mydb) => {
        assert.commandWorked(mydb.createCollection("x"));
        assert.commandWorked(mydb.createCollection("y"));
        assert.commandWorked(mydb.x.renameCollection("y", true));
        assert(mydb.y.drop());
    },
    renameCollectionAcrossDatabases: (mydb) => {
        let otherdb = mydb.getSiblingDB(mydb + "_");
        let [x, y] = getCollections(mydb, ["x", "y"]);
        let [z] = getCollections(otherdb, ["z"]);
        assert.commandWorked(x.insert({_id: 1, x: 1}));
        assert.commandWorked(y.insert({_id: 1, y: 1}));

        assert.commandWorked(mydb.adminCommand({renameCollection: x.getFullName(), to: z.getFullName()})); // across databases
        assert.commandWorked(z.insert({_id: 2, x: 2}));
        assert.commandWorked(x.insert({_id: 2, x: false}));
        assert.commandWorked(y.insert({y: 2}));

        assert.commandWorked(
            mydb.adminCommand({
                renameCollection: y.getFullName(),
                to: x.getFullName(),
                dropTarget: true,
            }),
        ); // within database
        assert.commandWorked(mydb.adminCommand({renameCollection: z.getFullName(), to: y.getFullName()})); // across databases
        return [mydb, otherdb];
    },
    renameCollectionAcrossDatabasesWithDropAndConvertToCapped: (db1) => {
        let db2 = db1.getSiblingDB(db1 + "_");
        assert.commandWorked(db1.createCollection("a"));
        assert.commandWorked(db1.createCollection("b"));
        assert.commandWorked(db2.createCollection("c"));
        assert.commandWorked(db2.createCollection("d"));
        let [a, b] = getCollections(db1, ["a", "b"]);
        let [c, d] = getCollections(db2, ["c", "d"]);

        assert.commandWorked(
            db1.adminCommand({renameCollection: a.getFullName(), to: c.getFullName(), dropTarget: true}),
        );

        assert(d.drop());

        assert.commandWorked(
            db1.adminCommand({renameCollection: c.getFullName(), to: d.getFullName(), dropTarget: false}),
        );

        assert.commandWorked(
            db1.adminCommand({renameCollection: b.getFullName(), to: c.getFullName(), dropTarget: false}),
        );
        assert.commandWorked(db2.runCommand({convertToCapped: "d", size: 1000}));

        return [db1, db2];
    },
    createIndex: (mydb) => {
        let [x, y] = getCollections(mydb, ["x", "y"]);
        assert.commandWorked(x.createIndex({x: 1}));
        assert.commandWorked(x.insert({_id: 1, x: 1}));
        assert.commandWorked(y.insert({_id: 1, y: 1}));
        assert.commandWorked(y.createIndex({y: 1}));
        assert.commandWorked(y.insert({_id: 2, y: 2}));
    },
};

/**
 *  Create a new uniquely named database, execute testFun and compute the dbHash. Then replay
 *  all different suffixes of the oplog and check for the correct hash. If testFun creates
 *  additional databases, it should return an array with all databases to check.
 */
function testIdempotency(primary, testFun, testName) {
    // It is not possible to test createIndexes in applyOps because that command is not accepted
    // by applyOps in that mode.
    if ("createIndex" === testName) {
        return;
    }

    jsTestLog(`Execute ${testName}`);

    // Create a new database name, so it's easier to filter out our oplog records later.
    let dbname = new Date()
        .toISOString()
        .match(/[-0-9T]/g)
        .join(""); // 2017-05-30T155055713
    let mydb = primary.getDB(dbname);

    // Allow testFun to return the array of databases to check (default is mydb).
    let testdbs = testFun(mydb) || [mydb];
    let expectedInfo = dbInfo(testdbs);

    let oplog = mydb.getSiblingDB("local").oplog.rs;
    let ops = oplog
        .find(
            {
                op: {$ne: "n"},
                // admin.$cmd needed for cross-db rename applyOps
                ns: new RegExp("^" + mydb.getName() + "|^admin\.[$]cmd$"),
                "o.startIndexBuild": {$exists: false},
                "o.abortIndexBuild": {$exists: false},
                "o.commitIndexBuild": {$exists: false},
            },
            {ts: 0, t: 0, h: 0, v: 0},
        )
        .toArray();
    assert.gt(ops.length, 0, "Could not find any matching ops in the oplog");
    testdbs.forEach((db) => assert.commandWorked(db.dropDatabase()));

    // In actual initial sync, oplog application will never try to create a collection with an ident
    // that is drop pending, as the first phase won't create and then drop the collection. Reapplying
    // the same oplog to a database multiple times does, however. Rather than test something which
    // doesn't happen in practice (and doesn't work), remove the replicated idents from the oplog we reapply.
    // TODO(SERVER-107069): Once initial sync replicates idents we may want to reevaluate this. We
    // could instead wait for pending drops to complete, but that significantly slows down this test (20s -> 200s).
    for (let op of ops) {
        if (op.op == "c") {
            delete op.o2;
        }
    }

    if (debug) {
        print(testName + ": replaying suffixes of " + ops.length + " operations");
        printjson(ops);
    }

    for (let j = 0; j < ops.length; j++) {
        let replayOps = ops.slice(j);
        assertApplyOpsWorks(testdbs, replayOps);
        let actualInfo = dbInfo(testdbs);
        assert.eq(
            actualInfo,
            expectedInfo,
            "unexpected differences between databases after replaying final " +
                replayOps.length +
                " ops in test " +
                testName +
                ": " +
                tojson(replayOps),
        );
    }
}

for (let f in tests) testIdempotency(rst.getPrimary(), tests[f], f);

rst.stopSet();
