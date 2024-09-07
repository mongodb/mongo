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
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const collName = jsTestName();
const warnLogId = 20294;
const errorAndLogId = 7488700;

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
            consideredValue: 2
        }
    };

    assert(nodesToCheck.some((conn) => checkLog.checkContainsOnceJson(conn, logId, {
        "errInfo": function(obj) {
            return documentEq(obj, errInfo);
        }
    })));
}

function runTest(db) {
    const t = db[collName];
    t.drop();

    const validatorExpression = {a: 1};
    assert.commandWorked(db.createCollection(t.getName(), {validator: validatorExpression}));

    assert.commandWorked(t.insert({_id: 1, a: 1}));
    assert.eq(1, t.count());

    if (FeatureFlagUtil.isPresentAndEnabled(db, "ErrorAndLogValidationAction")) {
        const res = assert.commandWorkedOrFailedWithCode(
            t.runCommand("collMod", {validationAction: "errorAndLog"}), ErrorCodes.InvalidOptions);
        if (res.ok) {
            assertFailsValidation(t.update({}, {$set: {a: 2}}));
            checkLogsForFailedValidation(db, errorAndLogId);
            // make sure persisted
            const info = db.getCollectionInfos({name: t.getName()})[0];
            assert.eq("errorAndLog", info.options.validationAction, tojson(info));
        }
    }

    // check we can do a bad update in warn mode
    assert.commandWorked(t.runCommand("collMod", {validationAction: "warn"}));
    assert.commandWorked(t.update({}, {$set: {a: 2}}));
    assert.eq(1, t.find({a: 2}).itcount());

    // check log for message. In case of sharded deployments, look on all shards and expect the log
    // to be found on one of them.
    checkLogsForFailedValidation(db, warnLogId);

    // make sure persisted
    const info = db.getCollectionInfos({name: t.getName()})[0];
    assert.eq("warn", info.options.validationAction, tojson(info));
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
