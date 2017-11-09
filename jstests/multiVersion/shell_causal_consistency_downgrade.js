/**
 * Tests that the mongo shell doesn't gossip its highest seen clusterTime or inject an
 * afterClusterTime into its command requests after downgrading from 3.6 to 3.4.
 */
(function() {
    "use strict";

    load("jstests/replsets/rslib.js");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    primary.setCausalConsistency();

    const db = primary.getDB("test");
    const coll = db.shell_causal_consistency_downgrade;

    function testCommandCanBeCausallyConsistent(func, {
        expectedGossipClusterTime: expectedGossipClusterTime = true,
        expectedAfterClusterTime: expectedAfterClusterTime = true
    } = {}) {
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

        if (expectedGossipClusterTime) {
            assert(cmdObjSeen.hasOwnProperty("$clusterTime"),
                   "Expected operation " + tojson(cmdObjSeen) + " to have a $clusterTime object: " +
                       func.toString());
        } else {
            assert(!cmdObjSeen.hasOwnProperty("$clusterTime"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to not have a $clusterTime object: " + func.toString());
        }

        if (expectedAfterClusterTime) {
            assert(cmdObjSeen.hasOwnProperty("readConcern"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to have a readConcern object since it can be causally consistent: " +
                       func.toString());

            const readConcern = cmdObjSeen.readConcern;
            assert(readConcern.hasOwnProperty("afterClusterTime"),
                   "Expected operation " + tojson(cmdObjSeen) +
                       " to specify afterClusterTime since it can be causally consistent: " +
                       func.toString());
        } else {
            assert(!cmdObjSeen.hasOwnProperty("readConcern"),
                   "Expected operation " + tojson(cmdObjSeen) + " to not have a readConcern" +
                       " object since it cannot be causally consistent: " + func.toString());
        }
    }

    assert.writeOK(coll.insert({}));

    testCommandCanBeCausallyConsistent(function() {
        assert.commandWorked(db.runCommand({count: coll.getName()}));
    });

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "3.4"}));

    // The server continues to accept $clusterTime and afterClusterTime while in
    // featureCompatibilityVersion=3.4.
    testCommandCanBeCausallyConsistent(function() {
        assert.commandWorked(db.runCommand({count: coll.getName()}));
    });

    rst.restart(primary, {binVersion: "3.4"});
    rst.waitForMaster();
    reconnect(primary);

    assert(primary.isCausalConsistency(),
           "Re-establishing the connection shouldn't change the state of the Mongo object");

    // After downgrading to MongoDB 3.4, the mongo shell shouldn't gossip its highest seen
    // clusterTime or inject an afterClusterTime into its command requests.
    testCommandCanBeCausallyConsistent(function() {
        assert.commandWorked(db.runCommand({count: coll.getName()}));
    }, {expectedGossipClusterTime: false, expectedAfterClusterTime: false});

    rst.restart(primary, {binVersion: "latest", noReplSet: true});
    rst.waitForMaster();
    reconnect(primary);

    // When upgrading to MongoDB 3.6 but running as a stand-alone server, the mongo shell shouldn't
    // gossip its highest seen clusterTime or inject an afterClusterTime into its command requests.
    testCommandCanBeCausallyConsistent(function() {
        assert.commandWorked(db.runCommand({count: coll.getName()}));
    }, {expectedGossipClusterTime: false, expectedAfterClusterTime: false});

    rst.stopSet();
})();
