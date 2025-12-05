/**
 * Tests that a server containing an invalid wildcard index will log a warning on startup.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

// This is a version that allows the bad index to be created.
const oldVersion = "8.0.16";

// Standalone mongod
{
    const testName = "invalid_wildcard_index_log_at_startup";
    const dbpath = MongoRunner.dataPath + testName;
    const collName = "collectionWithInvalidWildcardIndex";

    {
        // Startup mongod version where we are allowed to create the invalid index.
        const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: oldVersion});
        assert.neq(null, conn, "mongod was unable to start up");

        const testDB = conn.getDB("test");
        assert.commandWorked(testDB[collName].insert({a: 1}));

        // Invalid index
        assert.commandWorked(
            testDB[collName].createIndex({"a": 1, "$**": 1}, {wildcardProjection: {"_id": 0}}));

        MongoRunner.stopMongod(conn);
    }
    {
        const conn = MongoRunner.runMongod({dbpath: dbpath, noCleanData: true});
        assert.neq(null, conn, "mongod was unable to start up");
        const testDB = conn.getDB("test");

        const cmdRes = assert.commandWorked(testDB.adminCommand({getLog: "startupWarnings"}));
        assert(
            /Found a compound wildcard index with an invalid wildcardProjection. Such indexes can no longer be created./
                .test(
                    cmdRes.log,
                    ),
        );

        // Be sure that inserting to the collection with the invalid index succeeds.
        assert.commandWorked(testDB[collName].insert({a: 2}));

        // Inserting to another collection should succeed.
        assert.commandWorked(testDB.someOtherCollection.insert({a: 1}));
        assert.eq(testDB.someOtherCollection.find().itcount(), 1);

        MongoRunner.stopMongod(conn);
    }
}

// Replica set
{
    let nodes = {
        n1: {binVersion: oldVersion},
        n2: {binVersion: oldVersion},
    };

    const rst = new ReplSetTest({nodes: nodes});
    rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    const db = primary.getDB("test");
    const coll = db.t;
    assert.commandWorked(coll.insert({a: 1}));

    assert.commandWorked(coll.createIndex({"a": 1, "$**": 1}, {wildcardProjection: {"_id": 0}}));

    // Force checkpoint in storage engine to ensure index is part of the catalog in
    // in finished state at startup.
    rst.awaitReplication();
    let secondary = rst.getSecondary();
    assert.commandWorked(secondary.adminCommand({fsync: 1}));

    // Check that initial sync works, this node would not allow the index to be created
    // (since it is on a version with the new validation logic) but should not fail on startup.
    const initialSyncNode = rst.add({rsConfig: {priority: 0}});
    rst.reInitiate();
    rst.awaitSecondaryNodes(null, [initialSyncNode]);

    // Restart the new node and check for the startup warning in the logs.
    rst.restart(initialSyncNode);
    rst.awaitSecondaryNodes(null, [initialSyncNode]);

    checkLog.containsJson(initialSyncNode, 11389700, {
        ns: coll.getFullName(),
    });

    rst.stopSet();
}
