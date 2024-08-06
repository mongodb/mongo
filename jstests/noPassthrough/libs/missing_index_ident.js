load('jstests/libs/fail_point_util.js');
load('jstests/noPassthrough/libs/index_build.js');

const MissingIndexIdent = class {
    /**
     * Starts a one-node replica set and gets it into a state where the catalog entry for index
     * 'a_1' references an ident which is missing. Returns with the node stopped.
     */
    static run() {
        const replTest = new ReplSetTest({nodes: 1});
        replTest.startSet();
        replTest.initiate();

        const primary = function() {
            return replTest.getPrimary();
        };
        const testDB = function() {
            return primary().getDB('test');
        };
        const coll = function() {
            return testDB()[jsTestName()];
        };

        assert.commandWorked(coll().insert({a: 0}));

        const fp = configureFailPoint(primary(), 'hangIndexBuildBeforeCommit');
        const awaitCreateIndex =
            IndexBuildTest.startIndexBuild(primary(), coll().getFullName(), {a: 1});
        fp.wait();

        // Get a timestamp before the commit timestamp of the index build.
        const ts =
            assert
                .commandWorked(testDB().runCommand({insert: coll().getName(), documents: [{a: 1}]}))
                .operationTime;

        configureFailPoint(primary(), 'holdStableTimestampAtSpecificTimestamp', {timestamp: ts});
        fp.off();
        awaitCreateIndex();

        const ident = assert.commandWorked(testDB().runCommand({collStats: coll().getName()}))
                          .indexDetails.a_1.uri.substring('statistics:table:'.length);

        replTest.restart(primary(), {
            setParameter: {
                // Set minSnapshotHistoryWindowInSeconds to 0 so that the the oldest timestamp can
                // move forward, despite the stable timestamp being held steady.
                minSnapshotHistoryWindowInSeconds: 0,
                'failpoint.holdStableTimestampAtSpecificTimestamp':
                    tojson({mode: 'alwaysOn', data: {timestamp: ts}})
            }
        });

        MissingIndexIdent.checkRecoveryLogs(primary(), coll(), ts, ident);

        const dbpath = primary().dbpath;

        // Shut down uncleanly so that a checkpoint is not taken. This will cause the index catalog
        // entry referencing the now-dropped ident to still be present.
        replTest.stop(0, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true});

        return {replTest, dbpath, ts, ident};
    }

    /**
     * Checks that the index was recovered.
     */
    static checkRecoveryLogs(conn, coll, ts, ident) {
        // On startup, the node will recover from before the index commit timestamp.
        checkLog.containsJson(conn, 23987, {
            recoveryTimestamp: (recoveryTs) => {
                return timestampCmp(
                           Timestamp(recoveryTs['$timestamp']['t'], recoveryTs['$timestamp']['i']),
                           ts) <= 0;
            }
        });

        // Since the index build was not yet completed at the recovery timestamp, its ident will
        // be reset.
        checkLog.containsJson(conn, 6987700, {
            index: 'a_1',
            namespace: coll.getFullName(),
            ident: ident,
        });
    }
};
