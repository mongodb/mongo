/**
 * Tests that killOp is ineffectual against repl internal threads (OplogApplier and OplogWriter).
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_83,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet({});
rst.initiate();

const primary = rst.getPrimary();
const primaryAdminDB = primary.getDB("admin");
const primaryDB = primary.getDB("test");
const secondary = rst.getSecondary();
const secondaryAdminDB = secondary.getDB("admin");
const secondaryDB = secondary.getDB("test");

// We need to set the default write concern to w:1 so that we can do inserts on the primary while
// the secondary is blocked from applying oplogs.
assert.commandWorked(
    primaryDB.adminCommand({
        "setDefaultRWConcern": 1,
        "defaultWriteConcern": {
            "w": 1,
        },
        "defaultReadConcern": {
            "level": "local",
        },
    }),
);

rst.awaitReplication();

const nDocs = 10;

function doInserts() {
    assert.commandWorked(primaryDB.createCollection("myColl"));
    for (let i = 0; i < nDocs; i++) {
        assert.commandWorked(
            primaryDB.myColl.insert({
                name: `Document ${i}`,
                value: i,
            }),
        );
    }
    assert.eq(primaryDB.myColl.countDocuments({}), nDocs);
}

//
// killOp() on primary.
//
jsTest.log.info("Kill ops on primary");

clearRawMongoProgramOutput();
primaryAdminDB.adminCommand({
    setParameter: 1,
    logComponentVerbosity: {command: {verbosity: 3}},
});

let primaryOplogWriterFP = configureFailPoint(primary, "rsSyncApplyStop");
try {
    primaryOplogWriterFP.wait();
    // Ensure Oplog Applier is stopped on the FP.
    assert.soon(
        () => rawMongoProgramOutput("21229.*Oplog Applier - rsSyncApplyStop fail point enabled."),
        "mongod did not log that OplogApplier stopped at FP",
        10 * 1000, // 10sec
    );

    const currentOpResults = primaryAdminDB.currentOp({"$all": true});

    const oplogWriterOp = currentOpResults.inprog.filter(function (op) {
        return op.desc && op.desc == "NoopWriter";
    });
    assert.gte(oplogWriterOp.length, 1, "Did not find any NoopWriter operation: " + tojson(currentOpResults));

    const oplogApplierOp = currentOpResults.inprog.filter(function (op) {
        return op.desc && op.desc == "OplogApplier-0";
    });
    assert.gte(oplogApplierOp.length, 1, "Did not find any OplogApplier operation: " + tojson(currentOpResults));

    assert.commandWorked(primaryAdminDB.killOp(oplogWriterOp[0].opid));
    assert.commandWorked(primaryAdminDB.killOp(oplogApplierOp[0].opid));

    assert.soon(
        () => rawMongoProgramOutput("11227300.*Not killing exempt op").match('.*"opId":' + oplogApplierOp[0].opid),
        "mongod did not log that it ignored killOp attempt on OplogApplier with opId=" + oplogApplierOp[0].opid,
        10 * 1000, // 10sec
    );
} finally {
    // Ensure the failpoint is turned off so the server cannot hang on shutdown.
    primaryOplogWriterFP.off();
}

// Make sure both nodes are still alive
assert.commandWorked(primaryAdminDB.runCommand({ping: 1}));
assert.commandWorked(secondaryAdminDB.runCommand({ping: 1}));

//
// killOp() on secondary.
//
jsTest.log.info("Kill ops on secondary");

clearRawMongoProgramOutput();
secondaryAdminDB.adminCommand({
    setParameter: 1,
    logComponentVerbosity: {command: {verbosity: 3}},
});

let secondaryOplogWriterFP = configureFailPoint(secondary, "rsSyncApplyStop");
try {
    secondaryOplogWriterFP.wait();
    // Ensure Oplog Writer and Applier are stopped on the FP.
    assert.soon(
        () => rawMongoProgramOutput("8543102.*Oplog Writer - rsSyncApplyStop fail point enabled."),
        "mongod did not log that OplogWriter stopped at FP",
        10 * 1000, // 10sec
    );
    assert.soon(
        () => rawMongoProgramOutput("21229.*Oplog Applier - rsSyncApplyStop fail point enabled."),
        "mongod did not log that OplogApplier stopped at FP",
        10 * 1000, // 10sec
    );

    doInserts();

    const currentOpResults = secondaryAdminDB.currentOp({"$all": true});

    const oplogWriterOp = currentOpResults.inprog.filter(function (op) {
        return op.desc && op.desc == "OplogWriter-0";
    });
    assert.gte(oplogWriterOp.length, 1, "Did not find any OplogWriter operation: " + tojson(currentOpResults));

    const oplogApplierOp = currentOpResults.inprog.filter(function (op) {
        return op.desc && op.desc == "OplogApplier-0";
    });
    assert.gte(oplogApplierOp.length, 1, "Did not find any OplogApplier operation: " + tojson(currentOpResults));

    assert.commandWorked(secondaryAdminDB.killOp(oplogWriterOp[0].opid));
    assert.commandWorked(secondaryAdminDB.killOp(oplogApplierOp[0].opid));

    assert.soon(
        () => rawMongoProgramOutput("11227300.*Not killing exempt op").match('.*"opId":' + oplogWriterOp[0].opid),
        "mongod did not log that it ignored killOp attempt on OplogWriter with opId=" + oplogWriterOp[0].opid,
        10 * 1000, // 10sec
    );
    assert.soon(
        () => rawMongoProgramOutput("11227300.*Not killing exempt op").match('.*"opId":' + oplogApplierOp[0].opid),
        "mongod did not log that it ignored killOp attempt on OplogApplier with opId=" + oplogApplierOp[0].opid,
        10 * 1000, // 10sec
    );
} finally {
    // Ensure the failpoint is turned off so the server cannot hang on shutdown.
    secondaryOplogWriterFP.off();
}

// Make sure both nodes are still alive
assert.commandWorked(primaryAdminDB.runCommand({ping: 1}));
assert.commandWorked(secondaryAdminDB.runCommand({ping: 1}));

// Make sure all inserts were correctly applied on the secondary
rst.awaitReplication();
assert.eq(secondaryDB.myColl.countDocuments({}), nDocs);

rst.stopSet();
