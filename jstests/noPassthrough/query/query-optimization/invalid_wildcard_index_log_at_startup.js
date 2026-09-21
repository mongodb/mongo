/**
 * Tests that a server containing an invalid wildcard index will log a warning on startup.
 *
 * Compound wildcard indexes with an invalid 'wildcardProjection' could only be created by binaries
 * before SERVER-113685. Use a current binary with the 'skipWildcardIndexProjectionValidation'
 * failpoint to create the invalid index.
 * TODO (SERVER-132386): This test relies on the 'skipWildcardIndexProjectionValidation'
 * failpoint, which is removed once 10.0 becomes last LTS.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const startupWarning =
    /Found a compound wildcard index with an invalid wildcardProjection. Such indexes can no longer be created./;

// Startup parameter which lets a node create the otherwise-banned index, including when applying
// the corresponding oplog entry on a secondary.
const skipValidationParam = {
    "failpoint.skipWildcardIndexProjectionValidation": tojson({mode: "alwaysOn"}),
};

const invalidKeyPattern = {"a": 1, "$**": 1};
const invalidIndexOptions = {wildcardProjection: {"_id": 0}};

describe("standalone containing an invalid wildcard index", function () {
    const dbpath = MongoRunner.dataPath + jsTestName();
    const collName = "collectionWithInvalidWildcardIndex";
    let conn;
    let testDB;

    before(function () {
        // Startup a mongod which is allowed to create the invalid index, then shut it down.
        const seedConn = MongoRunner.runMongod({
            dbpath: dbpath,
            setParameter: skipValidationParam,
        });
        assert.neq(null, seedConn, "mongod was unable to start up");
        const seedDB = seedConn.getDB("test");
        assert.commandWorked(seedDB[collName].insert({a: 1}));
        assert.commandWorked(seedDB[collName].createIndex(invalidKeyPattern, invalidIndexOptions));
        MongoRunner.stopMongod(seedConn);

        // Restart on the same dbpath without the failpoint, so the invalid index is subject to the
        // real validation logic at startup.
        conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up");
        testDB = conn.getDB("test");
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("logs a startup warning", function () {
        const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
        assert(startupWarning.test(cmdRes.log), "missing startup warning", {log: cmdRes.log});
    });

    it("accepts inserts to the collection with the invalid index", function () {
        assert.commandWorked(testDB[collName].insert({a: 2}));
    });

    it("accepts inserts to another collection", function () {
        assert.commandWorked(testDB.someOtherCollection.insert({a: 1}));
        assert.eq(testDB.someOtherCollection.find().itcount(), 1);
    });
});

describe("replica set node initial syncing an invalid wildcard index", function () {
    let rst;
    let coll;
    let initialSyncNode;

    before(function () {
        rst = new ReplSetTest({
            nodes: {
                n1: {setParameter: skipValidationParam},
                n2: {setParameter: skipValidationParam},
            },
        });
        rst.startSet();
        rst.initiate();

        coll = rst.getPrimary().getDB("test").t;
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.createIndex(invalidKeyPattern, invalidIndexOptions));

        // Force checkpoint in storage engine to ensure index is part of the catalog in
        // in finished state at startup.
        rst.awaitReplication();
        assert.commandWorked(rst.getSecondary().adminCommand({fsync: 1}));

        // Check that initial sync works. This node would not allow the index to be created (it
        // does not skip the validation logic) but should not fail on startup.
        initialSyncNode = rst.add({rsConfig: {priority: 0}});
        rst.reInitiate();
        rst.awaitSecondaryNodes(null, [initialSyncNode]);
    });

    after(function () {
        rst.stopSet();
    });

    it("logs a startup warning after restart", function () {
        rst.restart(initialSyncNode);
        rst.awaitSecondaryNodes(null, [initialSyncNode]);

        checkLog.containsJson(initialSyncNode, 11389700, {
            ns: coll.getFullName(),
        });
    });
});
