/**
 * Tests the collection of metrics calculating the duration of time that index builds were blocked
 * from committing.
 *
 * @tags: [
 * # Primary-driven index builds does not use commit quorum.
 * primary_driven_index_builds_incompatible,
 * # Required for start-up recovery
 * requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const dbName = jsTestName();
const collName = "t";
const lastTimeBetweenVoteAndCommitMillis = "lastTimeBetweenVoteAndCommitMillis";
const lastTimeBetweenCommitOplogAndCommitMillis = "lastTimeBetweenCommitOplogAndCommitMillis";
const lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery =
    "lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery";
const lastTimeBetweenCommitOplogAndCommitMillisRestore =
    "lastTimeBetweenCommitOplogAndCommitMillisRestore";

function getIndexBuildServerStatusMetric(conn, metricName) {
    const serverStatus = conn.serverStatus();
    assert(
        serverStatus.hasOwnProperty("indexBuilds"),
        "Server status missing indexBuilds section: " + tojson(serverStatus),
    );

    const indexBuilds = serverStatus.indexBuilds;

    assert(indexBuilds.hasOwnProperty("phases"),
           "indexBuilds section missing phases field: " + tojson(indexBuilds));

    const phases = indexBuilds.phases;

    assert(
        phases.hasOwnProperty(metricName),
        "phases section missing " + metricName + " field: " + tojson(indexBuilds),
    );

    return phases[metricName];
}

function checkMetrics(conn, metricNames, expectedValues, comparators) {
    assert.eq(metricNames.length, expectedValues.length);
    assert.eq(metricNames.length, comparators.length);

    for (let i = 0; i < metricNames.length; i++) {
        const metricValue = getIndexBuildServerStatusMetric(conn, metricNames[i]);
        const expectedValue = expectedValues[i];
        const comparator = comparators[i];

        if (comparator === "eq") {
            assert.eq(
                metricValue,
                expectedValue,
                `Expected value of ${metricNames[i]} to be ${expectedValue}, found ${metricValue}`,
            );
        } else if (comparator === "gte") {
            assert.gte(
                metricValue,
                expectedValue,
                `Expected value of ${metricNames[i]} to be greater than or equal to ${
                    expectedValue}, found ${metricValue}`,
            );
        } else if (comparator === "gt") {
            assert.gt(
                metricValue,
                expectedValue,
                `Expected value of ${metricNames[i]} to be greater than ${expectedValue}, found ${
                    metricValue}`,
            );
        } else {
            throw new Error(`Unexpected comparator: ${comparator}`);
        }
    }
}

function testDurationBetweenVotingAndCommittingSteadyState() {
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
                // Disallow elections on the secondary.
                rsConfig: {
                    priority: 0,
                    votes: 0,
                },
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const coll = primaryDB.getCollection(collName);
    coll.drop();

    assert.commandWorked(coll.insertMany(Array.from({length: 100}, () => ({x: 1}))));
    rst.awaitReplication();

    let durationBetweenVotingToCommitAndCommitting = getIndexBuildServerStatusMetric(
        primaryDB,
        lastTimeBetweenVoteAndCommitMillis,
    );

    checkMetrics(primaryDB, [lastTimeBetweenVoteAndCommitMillis], [0], ["eq"]);
    const fp = configureFailPoint(primaryDB, "hangIndexBuildAfterSignalPrimaryForCommitReadiness");
    const awaitCreateIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});

    fp.wait();
    // Sleep so that we have a non-trivial duration for
    // lastTimeBetweenVoteAndCommitMillis.
    sleep(1000);
    fp.off();

    IndexBuildTest.waitForIndexBuildToStop(primaryDB);
    IndexBuildTest.waitForIndexBuildToStop(secondaryDB);
    awaitCreateIndex();

    checkMetrics(primaryDB, [lastTimeBetweenVoteAndCommitMillis], [1000], ["gte"]);
    rst.stopSet();
};

function testReceivingCommitIndexBuildEntryDuringSteadyState() {
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
                // Disallow elections on the secondary.
                rsConfig: {
                    priority: 0,
                    votes: 0,
                },
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const coll = primaryDB.getCollection(collName);
    coll.drop();

    assert.commandWorked(coll.insertMany(Array.from({length: 100}, () => ({x: 1}))));
    rst.awaitReplication();

    checkMetrics(secondaryDB,
                 [
                     lastTimeBetweenCommitOplogAndCommitMillis,
                     lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery,
                     lastTimeBetweenCommitOplogAndCommitMillisRestore,
                 ],
                 [0, 0, 0],
                 ["eq", "eq", "eq"]);

    const fp =
        configureFailPoint(secondary, "hangIndexBuildAfterReceivingCommitIndexBuildOplogEntry");

    const awaitCreateIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});

    IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

    fp.wait();
    // Sleep so that we have a non-trivial duration for
    // lastTimeBetweenCommitOplogAndCommitMillis.
    sleep(1000);
    fp.off();

    IndexBuildTest.waitForIndexBuildToStop(primaryDB);
    IndexBuildTest.waitForIndexBuildToStop(secondaryDB);

    awaitCreateIndex();

    checkMetrics(
        secondaryDB,
        [
            lastTimeBetweenCommitOplogAndCommitMillis,
            lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery,
            lastTimeBetweenCommitOplogAndCommitMillisRestore,
        ],
        [1000, 0, 0],
        ["gte", "eq", "eq"],
    );

    rst.stopSet();
};

function testReceivingCommitIndexBuildEntryDuringStartupRecovery() {
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
                // Disallow elections on the secondary.
                rsConfig: {
                    priority: 0,
                    votes: 0,
                },
            },
        ],
    });
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    let secondary = rst.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const coll = primaryDB.getCollection(collName);
    coll.drop();

    assert.commandWorked(coll.insertMany(Array.from({length: 100}, () => ({x: 1}))));
    rst.awaitReplication();

    checkMetrics(
        secondaryDB,
        [
            lastTimeBetweenCommitOplogAndCommitMillis,
            lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery,
            lastTimeBetweenCommitOplogAndCommitMillisRestore,
        ],
        [0, 0, 0],
        ["eq", "eq", "eq"],
    );

    const hangAfterVotingFP = configureFailPoint(
        secondary,
        "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
    );

    const awaitCreateIndex = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {x: 1});

    IndexBuildTest.waitForIndexBuildToStart(secondaryDB);
    hangAfterVotingFP.wait();

    IndexBuildTest.waitForIndexBuildToStop(primaryDB);

    rst.stop(secondary,
             /*signal=*/ 9,
             {allowedExitCode: MongoRunner.EXIT_SIGKILL},
             {forRestart: true});

    rst.start(
        secondary,
        {
            setParameter: {
                "failpoint.hangIndexBuildAfterReceivingCommitIndexBuildOplogEntry": tojson({
                    mode: {times: 1},
                }),
            },
        },
        true /* restart */,
    );

    secondary = rst.getSecondary();
    const secondaryDBAfterRestart = secondary.getDB(dbName);

    awaitCreateIndex();

    checkMetrics(
        secondaryDBAfterRestart,
        [
            lastTimeBetweenCommitOplogAndCommitMillis,
            lastTimeBetweenCommitOplogAndCommitMillisStartupRecovery,
            lastTimeBetweenCommitOplogAndCommitMillisRestore,
        ],
        [0, 0, 0],
        ["eq", "gt", "eq"],
    );

    rst.stopSet();
};

testDurationBetweenVotingAndCommittingSteadyState();
testReceivingCommitIndexBuildEntryDuringSteadyState();
testReceivingCommitIndexBuildEntryDuringStartupRecovery();
