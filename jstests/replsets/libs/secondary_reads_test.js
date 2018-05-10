/**
 * This is a library for testing secondary reads against a replica set
 */

"use strict";

load("jstests/replsets/rslib.js");

function SecondaryReadsTest(name = "secondary_reads_test") {
    let rst = performStandardSetup();
    let dbName = name;

    let primary = rst.getPrimary();
    let primaryDB = primary.getDB(dbName);
    let secondary = rst.getSecondary();
    let secondaryDB = secondary.getDB(dbName);
    secondaryDB.getMongo().setSlaveOk();

    let readers = [];
    let signalColl = "signalColl";
    const testDoneId = "testDone";
    /**
     * Return an instance of ReplSetTest initialized with a standard
     * two-node replica set running with the latest version.
     */
    function performStandardSetup() {
        // TODO: Periodic noop writes can be removed once SERVER-33248 is complete.
        let replSet = new ReplSetTest(
            {name, nodes: 2, nodeOptions: {setParameter: {writePeriodicNoops: true}}});
        replSet.startSet();

        const nodes = replSet.nodeList();
        replSet.initiate({
            _id: name,
            members: [{_id: 0, host: nodes[0]}, {_id: 1, host: nodes[1], priority: 0}]
        });
        return replSet;
    }

    this.startSecondaryReaders = function(nReaders, readFn) {

        let read = function() {
            db.getMongo().setSlaveOk();
            db = db.getSiblingDB(TestData.dbName);
            while (true) {
                readFn();
                let signalDoc = db.getCollection(TestData.signalColl)
                                    .find({_id: TestData.testDoneId})
                                    .itcount();
                if (signalDoc != 0) {
                    print("signal doc found. quitting...");
                    quit();
                }
            }
        };

        TestData.dbName = dbName;
        TestData.signalColl = signalColl;
        TestData.testDoneId = testDoneId;
        let serializedFn = "let readFn = " + readFn.toString() + ";(" + read.toString() + ")();";

        for (let i = 0; i < nReaders; i++) {
            readers.push(startParallelShell(serializedFn, secondary.port));
            print("reader " + readers.length + " started");
        }
    };

    let failPoint = "pauseBatchApplicationBeforeCompletion";

    // This returns a function that should be called once after performing a replicated write on a
    // primary. The write will start a batch on a secondary and immediately pause before completion.
    // The returned function will return once the batch has reached the point where it has applied
    // but not updated the last applied optime.
    this.pauseSecondaryBatchApplication = function() {

        clearRawMongoProgramOutput();

        assert.commandWorked(
            secondaryDB.adminCommand({configureFailPoint: failPoint, mode: "alwaysOn"}));

        return function() {
            assert.soon(function() {
                return rawMongoProgramOutput().match(failPoint + " fail point enabled");
            });
        };
    };

    this.resumeSecondaryBatchApplication = function() {
        assert.commandWorked(
            secondaryDB.adminCommand({configureFailPoint: failPoint, mode: "off"}));
    };

    this.getPrimaryDB = function() {
        return primaryDB;
    };

    this.getSecondaryDB = function() {
        return secondaryDB;
    };

    this.stopReaders = function() {
        print("signaling readers to stop...");
        assert.gt(readers.length, 0, "no readers to stop");
        assert.writeOK(primaryDB.getCollection(signalColl).insert({_id: testDoneId}));
        for (let i = 0; i < readers.length; i++) {
            const await = readers[i];
            await();
            print("reader " + i + " done");
        }
        readers = [];
    };

    this.getReplset = function() {
        return rst;
    };

    this.stop = function() {
        if (readers.length > 0) {
            this.stopReaders();
        }
        rst.stopSet();
    };
}
