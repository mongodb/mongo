/**
 * Tests that the mongo shell gossips the greater of the client's clusterTime and the session's
 * clusterTime.
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();

    const session1 = primary.startSession();
    const session2 = primary.startSession();

    const db = primary.getDB("test");
    const coll = db.shell_gossip_cluster_time;

    function testCommandGossipedWithClusterTime(func, expectedClusterTime) {
        const mongoRunCommandOriginal = Mongo.prototype.runCommand;

        const sentinel = {};
        let cmdObjSeen = sentinel;

        Mongo.prototype.runCommand = function runCommandSpy(dbName, cmdObj, options) {
            cmdObjSeen = cmdObj;
            return mongoRunCommandOriginal.apply(this, arguments);
        };

        try {
            assert.doesNotThrow(func);
        } finally {
            Mongo.prototype.runCommand = mongoRunCommandOriginal;
        }

        if (cmdObjSeen === sentinel) {
            throw new Error("Mongo.prototype.runCommand() was never called: " + func.toString());
        }

        let cmdName = Object.keys(cmdObjSeen)[0];

        // If the command is in a wrapped form, then we look for the actual command object inside
        // the query/$query object.
        if (cmdName === "query" || cmdName === "$query") {
            cmdObjSeen = cmdObjSeen[cmdName];
            cmdName = Object.keys(cmdObjSeen)[0];
        }

        if (expectedClusterTime === undefined) {
            assert(!cmdObjSeen.hasOwnProperty("$clusterTime"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to not have a $clusterTime object: " + func.toString());
        } else {
            assert(cmdObjSeen.hasOwnProperty("$clusterTime"),
                   "Expected operation " + tojson(cmdObjSeen) + " to have a $clusterTime object: " +
                       func.toString());

            assert(bsonBinaryEqual(expectedClusterTime, cmdObjSeen.$clusterTime));
        }
    }

    assert(
        session1.getClusterTime() === undefined,
        "session1 has yet to be used, but has clusterTime: " + tojson(session1.getClusterTime()));
    assert(
        session2.getClusterTime() === undefined,
        "session2 has yet to be used, but has clusterTime: " + tojson(session2.getClusterTime()));

    // Advance the clusterTime outside of either of the sessions.
    testCommandGossipedWithClusterTime(function() {
        assert.writeOK(coll.insert({}));
    }, primary.getClusterTime());

    assert(
        session1.getClusterTime() === undefined,
        "session1 has yet to be used, but has clusterTime: " + tojson(session1.getClusterTime()));
    assert(
        session2.getClusterTime() === undefined,
        "session2 has yet to be used, but has clusterTime: " + tojson(session2.getClusterTime()));

    // Performing an operation with session1 should use the highest clusterTime seen by the client
    // since session1 hasn't been used yet.
    testCommandGossipedWithClusterTime(function() {
        const coll = session1.getDatabase("test").mycoll;
        assert.writeOK(coll.insert({}));
    }, primary.getClusterTime());

    assert.eq(session1.getClusterTime(), primary.getClusterTime());

    testCommandGossipedWithClusterTime(function() {
        const coll = session1.getDatabase("test").mycoll;
        assert.writeOK(coll.insert({}));
    }, session1.getClusterTime());

    assert(
        session2.getClusterTime() === undefined,
        "session2 has yet to be used, but has clusterTime: " + tojson(session2.getClusterTime()));

    primary.resetClusterTime_forTesting();
    assert(primary.getClusterTime() === undefined,
           "client's cluster time should have been reset, but has clusterTime: " +
               tojson(primary.getClusterTime()));

    // Performing an operation with session2 should use the highest clusterTime seen by session2
    // since the client's clusterTime has been reset.
    session2.advanceClusterTime(session1.getClusterTime());
    testCommandGossipedWithClusterTime(function() {
        const coll = session2.getDatabase("test").mycoll;
        assert.writeOK(coll.insert({}));
    }, session2.getClusterTime());

    assert.eq(session2.getClusterTime(), primary.getClusterTime());

    primary.resetClusterTime_forTesting();
    assert(primary.getClusterTime() === undefined,
           "client's cluster time should have been reset, but has clusterTime: " +
               tojson(primary.getClusterTime()));

    // Performing an operation with session2 should use the highest clusterTime seen by session2
    // since the highest clusterTime seen by session1 is behind that of session2's.
    primary.advanceClusterTime(session1.getClusterTime());
    testCommandGossipedWithClusterTime(function() {
        const coll = session2.getDatabase("test").mycoll;
        assert.writeOK(coll.insert({}));
    }, session2.getClusterTime());

    rst.stopSet();
})();
