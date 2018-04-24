/**
 * This is a library for testing secondary reads against a replica set
 */

"use strict";

load("jstests/replsets/rslib.js");

function SecondaryReadsTest(name = "secondary_reads_test", replSet) {
    let rst = (replSet) ? replSet : performStandardSetup();
    let dbName = name;

    let primary = rst.getPrimary();
    let primaryDB = primary.getDB(dbName);
    let secondary = rst.getSecondary();
    let readers = [];

    /**
     * Return an instance of ReplSetTest initialized with a standard
     * two-node replica set running with the latest version.
     */
    function performStandardSetup() {
        let replSet = new ReplSetTest({name, nodes: 2});
        replSet.startSet();

        const nodes = replSet.nodeList();
        replSet.initiate({
            _id: name,
            members: [{_id: 0, host: nodes[0]}, {_id: 1, host: nodes[1], priority: 0}]
        });
        return replSet;
    }

    this.startSecondaryReaders = function(nReaders, cmd) {
        let readCmd = `db.getMongo().setSlaveOk();
                       db.getMongo().setReadPref("secondaryPreferred");
                       db = db.getSiblingDB("${dbName}");
                       ${cmd}`;

        for (let i = 0; i < nReaders; i++) {
            readers.push(
                startMongoProgramNoConnect("mongo", "--port", secondary.port, "--eval", readCmd));
            print("reader " + readers.length + " started");
        }
    };

    this.doOnPrimary = function(writeFn) {
        let db = primary.getDB(dbName);
        writeFn(db);
    };

    this.stopReaders = function() {
        for (let i = 0; i < readers.length; i++) {
            const pid = readers[i];
            const ec = stopMongoProgramByPid(pid);
            const expect = _isWindows() ? 1 : -15;
            assert.eq(
                ec, expect, "Expected mongo shell to exit with code " + expect + ". PID: " + pid);
            print("reader " + i + " done");
        }
        readers = [];
    };

    this.getReplset = function() {
        return rst;
    };

    this.stop = function() {
        this.stopReaders();
        rst.stopSet();
    };
}
