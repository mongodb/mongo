/**
 * Tests the collection of metrics calculating the duration of time that index builds were blocked from committing.
 *
 * @tags: [
 * # Primary-driven index builds does not use commit quorum.
 * primary_driven_index_builds_incompatible,
 * # Required for start-up recovery
 * requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const dbName = jsTestName();
const collName = "t";
const lastTimeBetweenVoteAndCommitMillis = "lastTimeBetweenVoteAndCommitMillis";
const lastTimeBetweenCommitOplogAndCommitMillis = "lastTimeBetweenCommitOplogAndCommitMillis";

function getIndexBuildServerStatusMetric(conn, metricName) {
    const serverStatus = conn.serverStatus();
    assert(
        serverStatus.hasOwnProperty("indexBuilds"),
        "Server status missing indexBuilds section: " + tojson(serverStatus),
    );

    const indexBuilds = serverStatus.indexBuilds;

    assert(indexBuilds.hasOwnProperty("phases"), "indexBuilds section missing phases field: " + tojson(indexBuilds));

    const phases = indexBuilds.phases;

    assert(
        phases.hasOwnProperty(metricName),
        "phases section missing " + metricName + " field: " + tojson(indexBuilds),
    );

    return phases[metricName];
}

describe("indexBuilds serverStatus metrics", function () {
    before(() => {
        this.rst = new ReplSetTest({
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
    });

    beforeEach(() => {
        {
            this.rst.startSet();
            this.rst.initiate();

            this.primary = this.rst.getPrimary();
            this.primaryDB = this.primary.getDB(dbName);
            this.secondary = this.rst.getSecondary();
            this.secondaryDB = this.secondary.getDB(dbName);
            this.coll = this.primaryDB.getCollection(collName);
            this.coll.drop();

            assert.commandWorked(this.coll.insertMany(Array.from({length: 100}, () => ({x: 1}))));
            this.rst.awaitReplication();
        }
    });

    afterEach(() => {
        this.rst.stopSet();
    });

    it("duration between voting to commit and committing", () => {
        let durationBetweenVotingToCommitAndCommitting = getIndexBuildServerStatusMetric(
            this.primaryDB,
            lastTimeBetweenVoteAndCommitMillis,
        );

        assert.eq(
            durationBetweenVotingToCommitAndCommitting,
            0,
            `Expected initial value of ${lastTimeBetweenVoteAndCommitMillis} to be 0, found ${durationBetweenVotingToCommitAndCommitting}`,
        );
        const fp = configureFailPoint(this.primaryDB, "hangIndexBuildAfterSignalPrimaryForCommitReadiness");
        const awaitCreateIndex = IndexBuildTest.startIndexBuild(this.primary, this.coll.getFullName(), {x: 1});

        fp.wait();
        // Sleep so that we have a non-trivial duration for lastTimeBetweenVoteAndCommitMillis.
        sleep(1000);
        fp.off();

        IndexBuildTest.waitForIndexBuildToStop(this.primaryDB);
        IndexBuildTest.waitForIndexBuildToStop(this.secondaryDB);
        awaitCreateIndex();

        durationBetweenVotingToCommitAndCommitting = getIndexBuildServerStatusMetric(
            this.primaryDB,
            lastTimeBetweenVoteAndCommitMillis,
        );

        assert.gte(
            durationBetweenVotingToCommitAndCommitting,
            1000,
            `Expected value of ${lastTimeBetweenVoteAndCommitMillis} to be greater than 1000, found ${durationBetweenVotingToCommitAndCommitting}`,
        );
    });

    it("receive commitIndexBuild during steady state", () => {
        let duration = getIndexBuildServerStatusMetric(this.secondaryDB, lastTimeBetweenCommitOplogAndCommitMillis);

        assert.eq(
            duration,
            0,
            `Expected initial value of ${lastTimeBetweenCommitOplogAndCommitMillis} to be 0, found ${duration}`,
        );

        const fp = configureFailPoint(this.secondary, "hangIndexBuildAfterReceivingCommitIndexBuildOplogEntry");

        const awaitCreateIndex = IndexBuildTest.startIndexBuild(this.primary, this.coll.getFullName(), {x: 1});

        IndexBuildTest.waitForIndexBuildToStart(this.secondaryDB);

        fp.wait();
        // Sleep so that we have a non-trivial duration for lastTimeBetweenCommitOplogAndCommitMillis.
        sleep(1000);
        fp.off();

        IndexBuildTest.waitForIndexBuildToStop(this.primaryDB);
        IndexBuildTest.waitForIndexBuildToStop(this.secondaryDB);

        awaitCreateIndex();

        duration = getIndexBuildServerStatusMetric(this.secondaryDB, lastTimeBetweenCommitOplogAndCommitMillis);

        assert.gt(
            duration,
            1000,
            `Expected value of ${lastTimeBetweenCommitOplogAndCommitMillis} to be greater than 1000, found ${duration}`,
        );
    });

    it("receive commitIndexBuild during startupRecovery", () => {
        let durationBetweenReceivingCommitIndexBuildEntryAndCommitting = getIndexBuildServerStatusMetric(
            this.secondaryDB,
            lastTimeBetweenCommitOplogAndCommitMillis,
        );

        assert.eq(
            durationBetweenReceivingCommitIndexBuildEntryAndCommitting,
            0,
            `Expected initial value of ${lastTimeBetweenCommitOplogAndCommitMillis} to be 0, found ${durationBetweenReceivingCommitIndexBuildEntryAndCommitting}`,
        );

        const hangAfterVotingFP = configureFailPoint(
            this.secondary,
            "hangIndexBuildAfterSignalPrimaryForCommitReadiness",
        );

        const awaitCreateIndex = IndexBuildTest.startIndexBuild(this.primary, this.coll.getFullName(), {x: 1});

        IndexBuildTest.waitForIndexBuildToStart(this.secondaryDB);
        hangAfterVotingFP.wait();

        IndexBuildTest.waitForIndexBuildToStop(this.primaryDB);

        this.rst.stop(this.secondary, /*signal=*/ 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

        this.rst.start(
            this.secondary,
            {
                setParameter: {
                    "failpoint.hangIndexBuildBeforeCommit": tojson({
                        mode: {times: 1},
                    }),
                },
            },
            true /* restart */,
        );

        const secondary = this.rst.getSecondary();
        const secondaryDBAfterRestart = secondary.getDB(dbName);

        awaitCreateIndex();

        durationBetweenReceivingCommitIndexBuildEntryAndCommitting = getIndexBuildServerStatusMetric(
            secondaryDBAfterRestart,
            lastTimeBetweenCommitOplogAndCommitMillis,
        );

        assert.gt(
            durationBetweenReceivingCommitIndexBuildEntryAndCommitting,
            0,
            `Expected value of ${lastTimeBetweenCommitOplogAndCommitMillis} to be greater than 0, found ${durationBetweenReceivingCommitIndexBuildEntryAndCommitting}`,
        );
    });
});
