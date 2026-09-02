/**
 * Tests that document validation correctly produces logs when expected. This test must be in
 * noPassthrough because it uses the getLog command, which only fetches the most recent 1024 logs.
 * When run in suites, logs from previously run tests can cause the log this test looks for to get
 * lost.
 *
 * @tags: [
 *   no_selinux,
 * ]
 */

import {documentEq} from "jstests/aggregation/extras/utils.js";
import {assertFailsValidation} from "jstests/libs/doc_validation_utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();
const warnLogId = 20294;
const errorAndLogId = 7488700;
const hasEnterpriseModule = getBuildInfo().modules.includes("enterprise");

function checkLogsForFailedValidation(db, logId) {
    // In case of sharded deployments, look on all shards and expect the log to be found on one of
    // them.
    const nodesToCheck = FixtureHelpers.isStandalone(db) ? [db] : FixtureHelpers.getPrimaries(db);

    const errInfo = {
        failingDocumentId: 1,
        details: {
            operatorName: "$eq",
            specifiedAs: {a: 1},
            reason: "comparison failed",
            consideredValue: 2,
        },
    };

    assert(
        nodesToCheck.some((conn) =>
            checkLog.checkContainsOnceJson(conn, logId, {
                "errInfo": function (obj) {
                    return documentEq(obj, errInfo);
                },
            }),
        ),
    );
}

function checkLogsForRedactedValidation(db, logId) {
    const nodesToCheck = FixtureHelpers.isStandalone(db) ? [db] : FixtureHelpers.getPrimaries(db);

    const redactedErrInfo = {
        failingDocumentId: "###",
        details: {
            operatorName: "###",
            specifiedAs: {a: "###"},
            reason: "###",
            consideredValue: "###",
        },
    };

    // With full log redaction, all leaf values are replaced with "###". Objects and arrays retain
    // their structure but every scalar inside becomes "###".
    assert(
        nodesToCheck.some((conn) =>
            checkLog.checkContainsOnceJson(conn, logId, {
                "errInfo": function (obj) {
                    return documentEq(obj, redactedErrInfo);
                },
            }),
        ),
    );
}

function runTest(db) {
    const coll = db[collName];
    coll.drop();

    const validatorExpression = {a: 1};
    assert.commandWorked(db.createCollection(coll.getName(), {validator: validatorExpression}));

    assert.commandWorked(coll.insert({_id: 1, a: 1}));
    assert.eq(1, coll.count());

    assert.commandWorked(coll.runCommand("collMod", {validationAction: "errorAndLog"}));
    assertFailsValidation(coll.update({}, {$set: {a: 2}}));
    checkLogsForFailedValidation(db, errorAndLogId);
    // make sure persisted
    const errorAndLogInfo = db.getCollectionInfos({name: coll.getName()})[0];
    assert.eq("errorAndLog", errorAndLogInfo.options.validationAction, tojson(errorAndLogInfo));

    // check we can do a bad update in warn mode
    assert.commandWorked(coll.runCommand("collMod", {validationAction: "warn"}));
    assert.commandWorked(coll.update({}, {$set: {a: 2}}));
    assert.eq(1, coll.find({a: 2}).itcount());

    checkLogsForFailedValidation(db, warnLogId);

    // make sure persisted
    const info = db.getCollectionInfos({name: coll.getName()})[0];
    assert.eq("warn", info.options.validationAction, tojson(info));

    // Verify that errInfo is fully redacted when log redaction is enabled.
    if (hasEnterpriseModule) {
        const adminDb = db.getSiblingDB("admin");
        FixtureHelpers.runCommandOnEachPrimary({
            db: adminDb,
            cmdObj: {setParameter: 1, redactClientLogData: true},
        });
        try {
            assert.commandWorked(coll.update({}, {$set: {a: 3}}));
        } finally {
            FixtureHelpers.runCommandOnEachPrimary({
                db: adminDb,
                cmdObj: {setParameter: 1, redactClientLogData: false},
            });
        }
        checkLogsForRedactedValidation(db, warnLogId);

        // Restore the document to a validator-compliant state while still in warn mode. The prior
        // warn-mode updates set "a" to 2 then 3, which violate the validator {a: 1}. Bringing it
        // back into compliance ensures the upcoming errorAndLog-mode case rejects only because of
        // its own update, not pre-existing invalid state.
        assert.commandWorked(coll.update({}, {$set: {a: 1}}));

        // Verify errorAndLog mode also redacts errInfo.
        assert.commandWorked(coll.runCommand("collMod", {validationAction: "errorAndLog"}));
        FixtureHelpers.runCommandOnEachPrimary({
            db: adminDb,
            cmdObj: {setParameter: 1, redactClientLogData: true},
        });
        try {
            assertFailsValidation(coll.update({}, {$set: {a: 4}}));
        } finally {
            FixtureHelpers.runCommandOnEachPrimary({
                db: adminDb,
                cmdObj: {setParameter: 1, redactClientLogData: false},
            });
        }
        checkLogsForRedactedValidation(db, errorAndLogId);
    }
}

(function testStandalone() {
    const conn = MongoRunner.runMongod();
    const db = conn.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        MongoRunner.stopMongod(conn);
    }
})();

(function testReplicaSet() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        rst.stopSet();
    }
})();

(function testShardedCluster() {
    const st = new ShardingTest({shards: 2, config: 1});
    const db = st.s.getDB(jsTestName());
    try {
        runTest(db);
    } finally {
        st.stop();
    }
})();
