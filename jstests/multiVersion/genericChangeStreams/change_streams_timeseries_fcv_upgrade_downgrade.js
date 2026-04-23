// Tests that change streams correctly emit timeseries change events when the FCV is upgraded or downgraded.
//
// @tags: [
//   uses_change_streams,
//   requires_replication,
//   requires_timeseries,
//   # The test requires the feature flag to be enabled to create viewless timeseries collections.
//   featureFlagCreateViewlessTimeseriesCollections,
//   featureFlagChangeStreamPreciseShardTargeting,
// ]

import "jstests/multiVersion/libs/verify_versions.js";
import "jstests/multiVersion/libs/multi_cluster.js";
import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {before, beforeEach, afterEach, after, describe, it} from "jstests/libs/mochalite.js";
import {
    ChangeStreamTest,
    ChangeStreamWatchMode,
    assertOpenCursors,
    cursorCommentFilter,
    getClusterTime,
    getNextClusterTime,
} from "jstests/libs/query/change_stream_util.js";
import {
    CreateTimeseriesCollectionCommand,
    TimeseriesInsertCommand,
    FCVUpgradeCommand,
    FCVDowngradeCommand,
} from "jstests/libs/util/change_stream/change_stream_timeseries_commands.js";
import {CreateDatabaseCommand} from "jstests/libs/util/change_stream/change_stream_commands.js";
import {crossProduct} from "jstests/libs/query/query_settings_index_hints_tests.js";
import {assertCreateCollection} from "jstests/libs/collection_drop_recreate.js";

// TODO: SERVER-117540 Remove change stream timeseries multiversion test once 9.0 is the last LTS.
if (lastLTSFCV !== "8.0") {
    jsTest.log.info("Skipping test because lastLTSFCV is not 8.0");
    quit();
}

function makeNss(dbName, collName) {
    return {db: dbName, coll: collName};
}

function createDbCmdOnShard(dbName, shard) {
    return new CreateDatabaseCommand(dbName, null, null, null, shard);
}

function withChangeStreamTest(db, fn) {
    const cst = new ChangeStreamTest(db);
    try {
        fn(cst);
    } finally {
        cst.cleanUp();
    }
}

function openChangeStreamCursor(cst, collName, {watchMode, comment, rawData, version = "v2", ...spec}) {
    if (watchMode === ChangeStreamWatchMode.kCluster) {
        spec.allChangesForCluster = true;
    }

    if (version) {
        spec.version = version;
    }

    const collection = watchMode === ChangeStreamWatchMode.kCollection ? collName : 1;
    const aggregateOptions = {};
    if (rawData !== undefined) aggregateOptions.rawData = rawData;
    if (comment !== undefined) aggregateOptions.comment = comment;

    return cst.startWatchingChanges({
        pipeline: [{$changeStream: spec}, {$project: {"fullDocument._id": 0, documentKey: 0}}],
        collection,
        aggregateOptions,
    });
}

function runScenarioAndAssert({
    testHelper,
    conn,
    dbForCstName,
    phases,
    rawData,
    openChangeStreamAfterRunningAllCommands,
    makeCursorAndWatchCtx,
    comment,
}) {
    // Execute commands from all phases, if change stream needs to be open after all cmd execution.
    if (openChangeStreamAfterRunningAllCommands) {
        phases.flatMap((phase) => phase.cmds).forEach((cmd) => cmd.execute(conn));
    }

    const dbForCst = conn.getDB(dbForCstName);
    withChangeStreamTest(dbForCst, (cst) => {
        const {cursor, watchCtx} = makeCursorAndWatchCtx({cst, rawData, comment});
        for (const {cmds, unordered, assertCursors} of phases) {
            // Execute commands of a given phase, if change stream has been open before processing each phase.
            if (!openChangeStreamAfterRunningAllCommands) {
                cmds.forEach((cmd) => cmd.execute(conn));
            }

            const expectedChanges = cmds.flatMap((cmd) => cmd.getChangeEvents(watchCtx));
            jsTest.log.info("Expected changes for phase", {expectedChanges, unordered});

            cst.assertNextChangesEqual({cursor, expectedChanges, ignoreOrder: unordered});

            if (testHelper.isShardedCluster() && assertCursors && expectedChanges.length > 0) {
                assertCursors();
            }
        }
        cst.assertNoChange(cursor);
    });
}

class ReplSetTestHelper {
    setUp() {
        this.rst = new ReplSetTest({nodes: 2});
        this.rst.startSet();
        this.rst.initiate();
    }

    tearDown() {
        this.rst.stopSet();
    }

    getConn() {
        return this.rst.getPrimary();
    }

    getShardingTest() {
        return null;
    }

    setClusterToVersion(version) {
        jsTest.log.info(`Setting entire set to ${version}`);
        this.rst.upgradeSet({binVersion: version});
        assert.binVersion(this.rst.getPrimary(), version);
    }

    isShardedCluster() {
        return false;
    }

    allShards() {
        return [];
    }

    toString() {
        return "replica set";
    }
}

class ShardedClusterTestHelper {
    setUp() {
        this.st = new ShardingTest({
            shards: 2,
            config: 1,
            configShard: false,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            configOptions: {setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
            other: {enableBalancer: false},
        });
    }

    tearDown() {
        this.st.stop();
    }

    getConn() {
        return this.st.s;
    }

    getShardingTest() {
        return this.st;
    }

    setClusterToVersion(version) {
        jsTest.log.info(`Setting entire set to ${version}`);

        // Upgrade all parts of the sharded cluster to the given binVersion.
        this.st.upgradeCluster(version, {
            upgradeShards: true,
            upgradeConfigs: true,
            upgradeMongos: true,
            waitUntilStable: true,
        });

        assert.binVersion(this.st.s, version);
    }

    isShardedCluster() {
        return true;
    }

    allShards() {
        return [this.st.shard0.shardName, this.st.shard1.shardName];
    }

    toString() {
        return "sharded cluster";
    }
}

// Run multiversion tests for both replica set and sharded cluster configurations.
for (const [downgradeFCV, testHelper] of crossProduct(
    [lastLTSFCV, lastContinuousFCV],
    [new ReplSetTestHelper(), new ShardedClusterTestHelper()],
)) {
    describe(`$changeStream in ${testHelper.toString()} in ${downgradeFCV} FCV`, function () {
        const testDB1Name = "db1";
        const testDB2Name = "db2";
        const ts1CollName = "tsColl1";
        const ts2CollName = "tsColl2";
        const nonTsCollName = "nonTsColl";

        const date1 = ISODate(`2024-05-01T00:00:00Z`);
        const date2 = ISODate(`2024-05-01T00:01:00Z`);
        const date3 = ISODate(`2024-05-01T00:02:00Z`);

        let adminDB;
        let st;

        let db1PrimaryShard;
        let db2PrimaryShard;

        function setUpVariables(conn) {
            adminDB = conn.getDB("admin");
        }

        function downgrade() {
            testHelper.setClusterToVersion("last-lts");
            setUpVariables(testHelper.getConn());
        }

        function upgrade() {
            testHelper.setClusterToVersion("latest");
            setUpVariables(testHelper.getConn());
        }

        function getConn() {
            return testHelper.getConn();
        }

        before(function () {
            testHelper.setUp();

            st = testHelper.getShardingTest();

            db1PrimaryShard = testHelper.isShardedCluster() ? st.shard0.shardName : null;
            db2PrimaryShard = testHelper.isShardedCluster() ? st.shard1.shardName : null;
        });

        beforeEach(function () {
            setUpVariables(testHelper.getConn());

            // Reset to 'downgradeFCV' for each test case.
            new FCVDowngradeCommand({targetFCV: downgradeFCV}).execute(testHelper.getConn());
        });

        afterEach(function () {
            assert.commandWorked(testHelper.getConn().getDB(testDB1Name).dropDatabase());
            assert.commandWorked(testHelper.getConn().getDB(testDB2Name).dropDatabase());
        });

        after(function () {
            testHelper.tearDown();
        });

        const comment = "comment";
        function assertOpenCursorsOnShards(shards, expectedConfigCursor) {
            if (testHelper.isShardedCluster()) {
                assertOpenCursors(st, shards, expectedConfigCursor, cursorCommentFilter(comment));
            }
        }

        function assertOpenCursorsOnAllShards(expectedConfigCursor = true) {
            assertOpenCursorsOnShards(testHelper.allShards(), expectedConfigCursor);
        }

        describe("will emit timeseries change events on FCV upgrade", function () {
            function buildUpgradeTimeseriesScenario({db1Name, db2Name, coll1Name, coll2Name}) {
                const ts1 = {
                    regularNss: makeNss(db1Name, coll1Name),
                    bucketsNss: makeNss(db1Name, `system.buckets.${coll1Name}`),
                };
                const ts2 = {
                    regularNss: makeNss(db2Name, coll2Name),
                    bucketsNss: makeNss(db2Name, `system.buckets.${coll2Name}`),
                };

                const createDbCmds = (() => {
                    if (!testHelper.isShardedCluster()) {
                        return [];
                    }

                    return db1Name == db2Name
                        ? [createDbCmdOnShard(db1Name, db1PrimaryShard)]
                        : [createDbCmdOnShard(db1Name, db1PrimaryShard), createDbCmdOnShard(db2Name, db2PrimaryShard)];
                })();

                const createTs1CollCmd = new CreateTimeseriesCollectionCommand({
                    dbName: ts1.regularNss.db,
                    collName: ts1.regularNss.coll,
                    timeField: "t",
                    metaField: "m",
                });
                const createTs2CollCmd = new CreateTimeseriesCollectionCommand({
                    dbName: ts2.regularNss.db,
                    collName: ts2.regularNss.coll,
                    timeField: "t",
                    metaField: "m",
                });

                const preBucketDoc = {
                    control: {
                        version: 2,
                        min: {_id: 1, t: date1, v: "preUpgrade"},
                        max: {_id: 1, t: date1, v: "preUpgrade"},
                        count: 1,
                    },
                    meta: "preUpgrade",
                    data: {
                        t: BinData(7, "CQAAcHMxjwEAAAA="),
                        _id: BinData(7, "AQAAAAAAAADwPwA="),
                        v: BinData(7, "AgALAAAAcHJlVXBncmFkZQAA"),
                    },
                };

                const postBucketDoc = {
                    control: {
                        version: 2,
                        min: {_id: 2, t: date2, v: "postUpgrade"},
                        max: {_id: 2, t: date2, v: "postUpgrade"},
                        count: 1,
                    },
                    meta: "postUpgrade",
                    data: {
                        t: BinData(7, "CQBgWnQxjwEAAAA="),
                        _id: BinData(7, "AQAAAAAAAAAAQAA="),
                        v: BinData(7, "AgAMAAAAcG9zdFVwZ3JhZGUAAA=="),
                    },
                };

                const ts1PreInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts1.regularNss,
                    eventNss: ts1.bucketsNss,
                    insertDoc: {_id: 1, t: date1, m: "preUpgrade", v: "preUpgrade"},
                    expectedFullDocument: preBucketDoc,
                });
                const ts2PreInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts2.regularNss,
                    eventNss: ts2.bucketsNss,
                    insertDoc: {_id: 1, t: date1, m: "preUpgrade", v: "preUpgrade"},
                    expectedFullDocument: preBucketDoc,
                });

                const ts1PostInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts1.regularNss,
                    eventNss: ts1.regularNss,
                    insertDoc: {_id: 2, t: date2, m: "postUpgrade", v: "postUpgrade"},
                    expectedFullDocument: postBucketDoc,
                    requiresRawData: true,
                });
                const ts2PostInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts2.regularNss,
                    eventNss: ts2.regularNss,
                    insertDoc: {_id: 2, t: date2, m: "postUpgrade", v: "postUpgrade"},
                    expectedFullDocument: postBucketDoc,
                    requiresRawData: true,
                });

                const fcvUpgradeCmd = new FCVUpgradeCommand({
                    timeseriesCollections: [ts1, ts2],
                });

                const phases = {
                    preFCVUpgrade: [
                        ...createDbCmds,
                        createTs1CollCmd,
                        createTs2CollCmd,
                        ts1PreInsertCmd,
                        ts2PreInsertCmd,
                    ],
                    fcvUpgrade: [fcvUpgradeCmd],
                    postFCVUpgrade: [ts1PostInsertCmd, ts2PostInsertCmd],
                };
                return {colls: {ts1, ts2}, phases};
            }

            // collection and expect to see an insert, followed by rename and invalidation event due to FCV upgrade.
            //
            // We also open the change stream in 8.0 (last-lts) binary and ensure that we can safely open the change
            // stream that will be reading the oplog which contains an oplog entry it didn't know about
            // (upgradeDowngradeTimeseriesCollection). The change stream will ignore this oplog entry as it will be
            // filtered out by the change stream filter.
            // TODO: SERVER-124882 Revisit FCV upgrade/downgrade for change streams over timeseries collections.
            it.skip("while reading system.buckets collection until rename+invalidate", function () {
                if (testHelper.isShardedCluster()) {
                    jsTest.log.info(
                        "Skipping test in sharded cluster because opening change stream over system.buckets collection is not allowed",
                    );
                    return;
                }

                function withDowngradedBinary(fn) {
                    downgrade();
                    try {
                        fn();
                    } finally {
                        upgrade();
                    }
                }

                const watchMode = ChangeStreamWatchMode.kCollection;
                const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                const conn = getConn();
                const scenario = buildUpgradeTimeseriesScenario({
                    db1Name: testDB1Name,
                    db2Name: testDB1Name,
                    coll1Name: ts1CollName,
                    coll2Name: ts2CollName,
                });

                // Only care about ts1.buckets here; we stop at rename+invalidate.
                runScenarioAndAssert({
                    testHelper,
                    conn,
                    dbForCstName: testDB1Name,
                    phases: [
                        {cmds: scenario.phases.preFCVUpgrade, unordered: false},
                        {cmds: scenario.phases.fcvUpgrade, unordered: false},
                    ],
                    rawData: false, // rawData flag is redundant here as we stop at rename+invalidate events.
                    openChangeStreamAfterRunningAllCommands: false,
                    makeCursorAndWatchCtx: ({cst, rawData}) => {
                        const watchCtx = {
                            watchMode: ChangeStreamWatchMode.kCollection,
                            watchedNss: scenario.colls.ts1.bucketsNss,
                            showSystemEvents: true,
                            rawData,
                        };
                        const cursor = openChangeStreamCursor(cst, scenario.colls.ts1.bucketsNss.coll, {
                            showSystemEvents: true,
                            allowToRunOnSystemNS: true,
                            startAtOperationTime: initClusterTime,
                            rawData,
                            watchMode: ChangeStreamWatchMode.kCollection,
                        });
                        return {cursor, watchCtx};
                    },
                });

                // Ensure that the newly introduced oplog entry: 'upgradeDowngradeTimeseriesCollection' is not causing change streams in v8.0 to crash.
                {
                    new FCVDowngradeCommand({targetFCV: downgradeFCV}).execute(conn);
                    withDowngradedBinary(function () {
                        const dbForCst = testHelper.getConn().getDB(testDB1Name);
                        withChangeStreamTest(dbForCst, (cst) => {
                            const cursor = openChangeStreamCursor(cst, scenario.colls.ts1.bucketsNss.coll, {
                                showSystemEvents: true,
                                allowToRunOnSystemNS: true,
                                startAtOperationTime: initClusterTime,
                                watchMode: ChangeStreamWatchMode.kCollection,
                                // NOTE: downgraded binary does not accept version field.
                                version: null,
                            });

                            const cmds = scenario.phases.preFCVUpgrade;
                            const expectedChanges = cmds.flatMap((cmd) => cmd.getChangeEvents(watchMode));
                            cst.assertNextChangesEqual({cursor, expectedChanges});
                            cst.assertNoChange(cursor);
                        });
                    });
                }
            });

            for (const [rawData, openAfterRunningCmds] of crossProduct([true, false], [true, false])) {
                // As part of this test, we open a change stream over a database that has timeseries collections
                // and expect to see an insert, followed by rename and subsequent inserts after FCV upgrade
                // (if change stream was opened with rawData: true).
                it(`while reading database owning timeseries collections with rawData:${rawData} openChangeStreamAfterRunningAllCommands:${openAfterRunningCmds}`, function () {
                    const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                    const conn = getConn();
                    const scenario = buildUpgradeTimeseriesScenario({
                        db1Name: testDB1Name,
                        db2Name: testDB1Name,
                        coll1Name: ts1CollName,
                        coll2Name: ts2CollName,
                    });

                    // If we open change stream before FCV upgrade, then we will open change stream in v2 and will target
                    // all shards, including configsvr. If we open after FCV upgrade, even at the time before FCV upgrade
                    // occurred, then we will target all shards, without configsvr.
                    const expectedConfigCursor = !openAfterRunningCmds;

                    runScenarioAndAssert({
                        testHelper,
                        conn,
                        dbForCstName: testDB1Name,
                        phases: [
                            {
                                cmds: scenario.phases.preFCVUpgrade,
                                unordered: false,
                                assertCursors: () => assertOpenCursorsOnAllShards(expectedConfigCursor),
                            },
                            {
                                cmds: scenario.phases.fcvUpgrade,
                                unordered: true,
                                assertCursors: () => assertOpenCursorsOnAllShards(expectedConfigCursor),
                            },
                            {
                                cmds: scenario.phases.postFCVUpgrade,
                                unordered: false,
                                assertCursors: () => {
                                    if (openAfterRunningCmds) {
                                        assertOpenCursorsOnShards([db1PrimaryShard], false /* expectedConfigCursor */);
                                    } else {
                                        assertOpenCursorsOnAllShards(expectedConfigCursor);
                                    }
                                },
                            },
                        ],
                        rawData: rawData,
                        openChangeStreamAfterRunningAllCommands: openAfterRunningCmds,
                        comment,
                        makeCursorAndWatchCtx: ({cst, rawData, comment}) => {
                            const watchCtx = {
                                watchMode: ChangeStreamWatchMode.kDb,
                                watchedNss: {db: testDB1Name},
                                showSystemEvents: true,
                                rawData,
                            };
                            const cursor = openChangeStreamCursor(cst, 1, {
                                showSystemEvents: true,
                                allowToRunOnSystemNS: true,
                                startAtOperationTime: initClusterTime,
                                rawData,
                                comment,
                                watchMode: ChangeStreamWatchMode.kDb,
                            });
                            return {cursor, watchCtx};
                        },
                    });
                });

                // As part of this test, we open a change stream over the whole cluster that has timeseries
                // collections and expect to see an inserts, followed by reaname and subsequent inserts after FCV
                // collections and expect to see an insert, followed by rename and subsequent inserts after FCV
                // upgrade (if change stream was opened with rawData: true).
                it(`while reading cluster owning timeseries collections with rawData:${rawData} openChangeStreamAfterRunningAllCommands:${openAfterRunningCmds}`, function () {
                    const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                    const conn = getConn();
                    const scenario = buildUpgradeTimeseriesScenario({
                        db1Name: testDB1Name,
                        db2Name: testDB2Name,
                        coll1Name: ts1CollName,
                        coll2Name: ts2CollName,
                    });

                    runScenarioAndAssert({
                        testHelper,
                        conn,
                        dbForCstName: "admin",
                        phases: [
                            {
                                cmds: scenario.phases.preFCVUpgrade,
                                unordered: false,
                                assertCursors: () => assertOpenCursorsOnAllShards(true),
                            },
                            {
                                cmds: scenario.phases.fcvUpgrade,
                                unordered: true,
                                assertCursors: () => assertOpenCursorsOnAllShards(true),
                            },
                            {
                                cmds: scenario.phases.postFCVUpgrade,
                                unordered: false,
                                assertCursors: () => assertOpenCursorsOnAllShards(true),
                            },
                        ],
                        rawData: rawData,
                        openChangeStreamAfterRunningAllCommands: openAfterRunningCmds,
                        comment,
                        makeCursorAndWatchCtx: ({cst, rawData, comment}) => {
                            const watchCtx = {
                                watchMode: ChangeStreamWatchMode.kCluster,
                                watchedNss: {},
                                showSystemEvents: true,
                                rawData,
                            };
                            const cursor = openChangeStreamCursor(cst, 1, {
                                showSystemEvents: true,
                                allowToRunOnSystemNS: true,
                                startAtOperationTime: initClusterTime,
                                rawData,
                                comment,
                                watchMode: ChangeStreamWatchMode.kCluster,
                            });
                            return {cursor, watchCtx};
                        },
                    });
                });
            }
        });

        describe("will emit timeseries change events on FCV downgrade", function () {
            function buildDowngradeTimeseriesScenario({db1Name, db2Name, coll1Name, coll2Name}) {
                const ts1 = {
                    regularNss: makeNss(db1Name, coll1Name),
                    bucketsNss: makeNss(db1Name, `system.buckets.${coll1Name}`),
                };
                const ts2 = {
                    regularNss: makeNss(db2Name, coll2Name),
                    bucketsNss: makeNss(db2Name, `system.buckets.${coll2Name}`),
                };

                const createDbCmds = (() => {
                    if (!testHelper.isShardedCluster()) {
                        return [];
                    }

                    return db1Name == db2Name
                        ? [createDbCmdOnShard(db1Name, db1PrimaryShard)]
                        : [createDbCmdOnShard(db1Name, db1PrimaryShard), createDbCmdOnShard(db2Name, db2PrimaryShard)];
                })();

                const createTs1CollCmd = new CreateTimeseriesCollectionCommand({
                    dbName: ts1.regularNss.db,
                    collName: ts1.regularNss.coll,
                    timeField: "t",
                    metaField: "m",
                });
                const createTs2CollCmd = new CreateTimeseriesCollectionCommand({
                    dbName: ts2.regularNss.db,
                    collName: ts2.regularNss.coll,
                    timeField: "t",
                    metaField: "m",
                });

                const preBucketDoc = {
                    control: {
                        "version": 2,
                        "min": {"_id": 1, "t": date1, "v": "preDowngrade"},
                        "max": {"_id": 1, "t": date1, "v": "preDowngrade"},
                        "count": 1,
                    },
                    meta: "preDowngrade",
                    data: {
                        "v": BinData(7, "AgANAAAAcHJlRG93bmdyYWRlAAA="),
                        "_id": BinData(7, "AQAAAAAAAADwPwA="),
                        "t": BinData(7, "CQAAcHMxjwEAAAA="),
                    },
                };
                const postBucketDoc = {
                    control: {
                        version: 2,
                        min: {_id: 2, t: date2, v: "postDowngrade"},
                        max: {_id: 2, t: date2, v: "postDowngrade"},
                        count: 1,
                    },
                    meta: "postDowngrade",
                    data: {
                        t: BinData(7, "CQBgWnQxjwEAAAA="),
                        _id: BinData(7, "AQAAAAAAAAAAQAA="),
                        v: BinData(7, "AgAOAAAAcG9zdERvd25ncmFkZQAA"),
                    },
                };

                const ts1PreInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts1.regularNss,
                    eventNss: ts1.regularNss,
                    insertDoc: {_id: 1, t: date1, m: "preDowngrade", v: "preDowngrade"},
                    expectedFullDocument: preBucketDoc,
                    requiresRawData: true,
                });
                const ts2PreInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts2.regularNss,
                    eventNss: ts2.regularNss,
                    insertDoc: {_id: 1, t: date1, m: "preDowngrade", v: "preDowngrade"},
                    expectedFullDocument: preBucketDoc,
                    requiresRawData: true,
                });

                const ts1PostInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts1.regularNss,
                    eventNss: ts1.bucketsNss,
                    insertDoc: {_id: 2, t: date2, m: "postDowngrade", v: "postDowngrade"},
                    expectedFullDocument: postBucketDoc,
                    requiresRawData: false,
                });
                const ts2PostInsertCmd = new TimeseriesInsertCommand({
                    insertNss: ts2.regularNss,
                    eventNss: ts2.bucketsNss,
                    insertDoc: {_id: 2, t: date2, m: "postDowngrade", v: "postDowngrade"},
                    expectedFullDocument: postBucketDoc,
                    requiresRawData: false,
                });

                const fcvDowngradeCmd = new FCVDowngradeCommand({
                    timeseriesCollections: [ts1, ts2],
                    targetFCV: downgradeFCV,
                });

                const phases = {
                    preFCVDowngrade: [
                        ...createDbCmds,
                        createTs1CollCmd,
                        createTs2CollCmd,
                        ts1PreInsertCmd,
                        ts2PreInsertCmd,
                    ],
                    fcvDowngrade: [fcvDowngradeCmd],
                    postFCVDowngrade: [ts1PostInsertCmd, ts2PostInsertCmd],
                };
                return {colls: {ts1, ts2}, phases};
            }

            // As part of this test, we open a change stream over the viewless timeseries collection and expect to see insert,
            // followed by rename and invalidation event due to FCV downgrade.
            //
            // NOTE: Not running this test case in any other configuration because:
            // - we can not open change stream over the timeseries collection name in FCV 8.0, because it's a view
            //   therefore running with openChangeStreamAfterRunningAllCommands: false
            // - we can not open change stream over the timeseries collection name in FCV 9.0 without rawData.
            it("while reading timeseries collection until rename+invalidate", function () {
                // Upgrade to latestFCV (9.0).
                new FCVUpgradeCommand().execute(testHelper.getConn());

                const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                const conn = getConn();
                const scenario = buildDowngradeTimeseriesScenario({
                    db1Name: testDB1Name,
                    db2Name: testDB1Name,
                    coll1Name: ts1CollName,
                    coll2Name: ts2CollName,
                });

                runScenarioAndAssert({
                    testHelper,
                    conn,
                    dbForCstName: testDB1Name,
                    phases: [
                        {cmds: scenario.phases.preFCVDowngrade, unordered: false},
                        {cmds: scenario.phases.fcvDowngrade, unordered: false},
                    ],
                    rawData: true,
                    openChangeStreamAfterRunningAllCommands: false,
                    makeCursorAndWatchCtx: ({cst, rawData, comment}) => {
                        const watchCtx = {
                            watchMode: ChangeStreamWatchMode.kCollection,
                            watchedNss: scenario.colls.ts1.regularNss,
                            showSystemEvents: true,
                            rawData,
                        };
                        const cursor = openChangeStreamCursor(cst, scenario.colls.ts1.regularNss.coll, {
                            showSystemEvents: true,
                            allowToRunOnSystemNS: true,
                            startAtOperationTime: initClusterTime,
                            rawData,
                            // TODO: SERVER-124882 Revisit FCV upgrade/downgrade for change streams over timeseries collections.
                            version: "v1",
                            comment,
                            watchMode: ChangeStreamWatchMode.kCollection,
                        });
                        return {cursor, watchCtx};
                    },
                    comment,
                });
            });

            for (const [rawData, openAfterRunningCmds, showSystemEvents] of crossProduct(
                [true, false],
                [true, false],
                [true, false],
            )) {
                // As part of this test, we open a change stream over a database that has timeseries collections
                // and expect to see an insert, followed by rename and subsequent inserts after FCV downgrade
                // (if change stream was opened with rawData: true and showSystemEvents: true).
                it(`while reading database owning timeseries collections with rawData:${rawData} openChangeStreamAfterRunningAllCommands:${openAfterRunningCmds} showSystemEvents:${showSystemEvents}`, function () {
                    // Upgrade to latestFCV (9.0).
                    new FCVUpgradeCommand().execute(testHelper.getConn());

                    const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                    const conn = getConn();
                    const scenario = buildDowngradeTimeseriesScenario({
                        db1Name: testDB1Name,
                        db2Name: testDB1Name,
                        coll1Name: ts1CollName,
                        coll2Name: ts2CollName,
                    });

                    runScenarioAndAssert({
                        testHelper,
                        conn,
                        dbForCstName: testDB1Name,
                        phases: [
                            {
                                cmds: scenario.phases.preFCVDowngrade,
                                unordered: false,
                                assertCursors: () => {
                                    if (openAfterRunningCmds) {
                                        assertOpenCursorsOnAllShards();
                                    } else {
                                        assertOpenCursorsOnShards([db1PrimaryShard], false);
                                    }
                                },
                            },
                            {
                                cmds: scenario.phases.fcvDowngrade,
                                unordered: true,
                                assertCursors: () => assertOpenCursorsOnAllShards(),
                            },
                            {
                                cmds: scenario.phases.postFCVDowngrade,
                                unordered: false,
                                assertCursors: () => assertOpenCursorsOnAllShards(),
                            },
                        ],
                        rawData,
                        openChangeStreamAfterRunningAllCommands: openAfterRunningCmds,
                        comment,
                        makeCursorAndWatchCtx: ({cst, rawData, comment}) => {
                            const watchCtx = {
                                watchMode: ChangeStreamWatchMode.kDb,
                                watchedNss: {db: testDB1Name},
                                showSystemEvents,
                                rawData,
                            };
                            const cursor = openChangeStreamCursor(cst, 1, {
                                showSystemEvents,
                                allowToRunOnSystemNS: true,
                                startAtOperationTime: initClusterTime,
                                rawData,
                                comment,
                                watchMode: ChangeStreamWatchMode.kDb,
                            });
                            return {cursor, watchCtx};
                        },
                    });
                });

                // As part of this test, we open a change stream over the whole cluster that has timeseries
                // collections and expect to see an insert, followed by rename and subsequent inserts after FCV
                // downgrade (if change stream was opened with rawData: true and showSystemEvents: true).
                it(`while reading cluster owning timeseries collections with rawData:${rawData} openChangeStreamAfterRunningAllCommands:${openAfterRunningCmds} showSystemEvents:${showSystemEvents}`, function () {
                    // Upgrade to latestFCV (9.0).
                    new FCVUpgradeCommand().execute(testHelper.getConn());

                    const initClusterTime = getNextClusterTime(getClusterTime(adminDB));
                    const conn = getConn();
                    const scenario = buildDowngradeTimeseriesScenario({
                        db1Name: testDB1Name,
                        db2Name: testDB2Name,
                        coll1Name: ts1CollName,
                        coll2Name: ts2CollName,
                    });

                    runScenarioAndAssert({
                        testHelper,
                        conn,
                        dbForCstName: "admin",
                        phases: [
                            {cmds: scenario.phases.preFCVDowngrade, unordered: false},
                            {cmds: scenario.phases.fcvDowngrade, unordered: true},
                            {cmds: scenario.phases.postFCVDowngrade, unordered: false},
                        ],
                        rawData,
                        openChangeStreamAfterRunningAllCommands: openAfterRunningCmds,
                        comment,
                        makeCursorAndWatchCtx: ({cst, rawData, comment}) => {
                            const watchCtx = {
                                watchMode: ChangeStreamWatchMode.kCluster,
                                watchedNss: {},
                                showSystemEvents,
                                rawData,
                            };
                            const cursor = openChangeStreamCursor(cst, 1, {
                                showSystemEvents,
                                allowToRunOnSystemNS: true,
                                startAtOperationTime: initClusterTime,
                                rawData,
                                comment,
                                watchMode: ChangeStreamWatchMode.kCluster,
                            });
                            return {cursor, watchCtx};
                        },
                    });
                });
            }
        });

        // If change stream has been opened without rawData: true flag and with showSystemEvents: false,
        // we should not see any timeseries change events. Neither before nor after FCV upgrade/downgrade.
        it("will not emit timeseries change events neither before nor after FCV upgrade with showSystemEvents=false", function () {
            const db = adminDB.getSiblingDB(testDB1Name);
            const nonTsCollNss = makeNss(testDB1Name, nonTsCollName);
            const nonTsColl = assertCreateCollection(db, nonTsCollName);

            const ts1 = {
                regularNss: makeNss(testDB1Name, ts1CollName),
                bucketsNss: makeNss(testDB1Name, `system.buckets.${ts1CollName}`),
            };
            const tsColl = assertCreateCollection(db, ts1CollName, {timeseries: {timeField: "t", metaField: "m"}});

            function insertData(marker) {
                assert.commandWorked(tsColl.insert({t: ISODate(), m: marker, v: marker}));
                assert.commandWorked(nonTsColl.insert({marker: marker}));
            }

            withChangeStreamTest(db, (cst) => {
                const cursor = openChangeStreamCursor(cst, 1, {
                    showSystemEvents: false,
                    watchMode: ChangeStreamWatchMode.kDb,
                });

                insertData("preUpgrade");
                new FCVUpgradeCommand({timeseriesCollections: [ts1]}).execute(testHelper.getConn());
                insertData("postUpgrade/preDowngrade");
                new FCVDowngradeCommand({timeseriesCollections: [ts1], targetFCV: downgradeFCV}).execute(
                    testHelper.getConn(),
                );
                insertData("postDowngrade");

                // Assert that no timeseries insert events were observed.
                cst.assertNextChangesEqual({
                    cursor: cursor,
                    expectedChanges: [
                        {
                            operationType: "insert",
                            fullDocument: {marker: "preUpgrade"},
                            ns: nonTsCollNss,
                        },
                        {
                            operationType: "rename",
                            ns: ts1.bucketsNss,
                            to: ts1.regularNss,
                        },
                        {
                            operationType: "insert",
                            fullDocument: {marker: "postUpgrade/preDowngrade"},
                            ns: nonTsCollNss,
                        },
                        {
                            operationType: "rename",
                            ns: ts1.regularNss,
                            to: ts1.bucketsNss,
                        },
                        {
                            operationType: "insert",
                            fullDocument: {marker: "postDowngrade"},
                            ns: nonTsCollNss,
                        },
                    ],
                });
                cst.assertNoChange(cursor);
            });
        });

        // Test that after upgrading from lastLTSFCV to latestFCV, timeseries change events are no longer emitted
        // even for change streams opened with showSystemEvents: true. This is because in latestFCV, timeseries collections are
        // viewless collections and corresponding oplog entries are marked with 'isTimeseries': true flag, which are filtered out
        // unless the change stream is opened with rawData: true.
        it("will no longer emit timeseries change events after FCV upgrade for change streams with $showSystemEvents=true", function () {
            const st = testHelper.getShardingTest();
            if (testHelper.isShardedCluster()) {
                assert.commandWorked(
                    adminDB.runCommand({enableSharding: testDB1Name, primaryShard: st.shard0.shardName}),
                );
            }

            const watchMode = ChangeStreamWatchMode.kDb;
            const watchedNss = {db: testDB1Name};
            const db = adminDB.getSiblingDB(testDB1Name);
            const nonTsCollNss = makeNss(testDB1Name, nonTsCollName);
            const nonTsColl = assertCreateCollection(db, nonTsCollName);

            const ts1 = {
                regularNss: makeNss(testDB1Name, ts1CollName),
                bucketsNss: makeNss(testDB1Name, `system.buckets.${ts1CollName}`),
            };
            const tsColl = assertCreateCollection(db, ts1CollName, {timeseries: {timeField: "t", metaField: "m"}});

            function insertData(_id, marker, date = ISODate()) {
                assert.commandWorked(tsColl.insert({_id, t: date, m: marker, v: marker}));
                assert.commandWorked(nonTsColl.insert({marker: marker}));
            }

            withChangeStreamTest(db, (cst) => {
                const cursor = openChangeStreamCursor(cst, 1, {
                    showSystemEvents: true,
                    watchMode,
                    comment,
                });

                {
                    insertData(1, "preUpgrade", date1);
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: [
                            {
                                operationType: "insert",
                                fullDocument: {
                                    "control": {
                                        "version": 2,
                                        "min": {"_id": 1, "t": date1, "v": "preUpgrade"},
                                        "max": {"_id": 1, "t": date1, "v": "preUpgrade"},
                                        "count": 1,
                                    },
                                    "meta": "preUpgrade",
                                    "data": {
                                        "t": BinData(7, "CQAAcHMxjwEAAAA="),
                                        "_id": BinData(7, "AQAAAAAAAADwPwA="),
                                        "v": BinData(7, "AgALAAAAcHJlVXBncmFkZQAA"),
                                    },
                                },
                                ns: ts1.bucketsNss,
                            },
                            {
                                operationType: "insert",
                                fullDocument: {marker: "preUpgrade"},
                                ns: nonTsCollNss,
                            },
                        ],
                    });
                    assertOpenCursorsOnAllShards();
                }

                {
                    // FCV upgrade.
                    const upgradeCmd = new FCVUpgradeCommand({timeseriesCollections: [ts1]});
                    upgradeCmd.execute(testHelper.getConn());
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: upgradeCmd.getChangeEvents({watchMode, watchedNss}),
                    });
                    assertOpenCursorsOnAllShards();
                }

                {
                    insertData(2, "postUpgrade/preDowngrade", date2);
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: [
                            // NOTE: no postUpgrade timeseries insert event is expected here because viewless timeseries
                            // inserts are only observable with rawData:true.
                            {
                                operationType: "insert",
                                fullDocument: {marker: "postUpgrade/preDowngrade"},
                                ns: nonTsCollNss,
                            },
                        ],
                    });
                    assertOpenCursorsOnAllShards();
                }

                {
                    // FCV downgrade.
                    const downgradeCmd = new FCVDowngradeCommand({
                        timeseriesCollections: [ts1],
                        targetFCV: downgradeFCV,
                    });
                    downgradeCmd.execute(testHelper.getConn());
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: downgradeCmd.getChangeEvents({watchMode, watchedNss}),
                    });
                    assertOpenCursorsOnAllShards();
                }

                {
                    insertData(3, "postDowngrade", date3);
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: [
                            {
                                operationType: "insert",
                                fullDocument: {
                                    "control": {
                                        "version": 2,
                                        "min": {"_id": 3, "t": date3, "v": "postDowngrade"},
                                        "max": {"_id": 3, "t": date3, "v": "postDowngrade"},
                                        "count": 1,
                                    },
                                    "meta": "postDowngrade",
                                    "data": {
                                        "t": BinData(7, "CQDARHUxjwEAAAA="),
                                        "_id": BinData(7, "AQAAAAAAAAAIQAA="),
                                        "v": BinData(7, "AgAOAAAAcG9zdERvd25ncmFkZQAA"),
                                    },
                                },
                                ns: ts1.bucketsNss,
                            },
                            {
                                operationType: "insert",
                                fullDocument: {marker: "postDowngrade"},
                                ns: nonTsCollNss,
                            },
                        ],
                    });
                    assertOpenCursorsOnAllShards();
                }
            });
        });

        // Test that after downgrading from latestFCV to lastLTSFCV, timeseries change events are no longer emitted
        // even for change streams opened with rawData: true. This is because in lastLTSFCV, timeseries collections are
        // written to system.buckets collection and therefore require showSystemEvents:true to be observed.
        it("will no longer emit time-series events after the downgrade, with rawData: true enabled before the downgrade", function () {
            const st = testHelper.getShardingTest();
            if (testHelper.isShardedCluster()) {
                assert.commandWorked(adminDB.runCommand({enableSharding: testDB1Name, primaryShard: db1PrimaryShard}));
            }

            const watchMode = ChangeStreamWatchMode.kDb;
            const watchedNss = {db: testDB1Name};
            const db = adminDB.getSiblingDB(testDB1Name);
            const nonTsCollNss = makeNss(testDB1Name, nonTsCollName);
            const nonTsColl = assertCreateCollection(db, nonTsCollName);

            const ts1 = {
                regularNss: makeNss(testDB1Name, ts1CollName),
                bucketsNss: makeNss(testDB1Name, `system.buckets.${ts1CollName}`),
            };
            const tsColl = assertCreateCollection(db, ts1CollName, {timeseries: {timeField: "t", metaField: "m"}});

            function insertData(_id, marker, date = ISODate()) {
                assert.commandWorked(tsColl.insert({_id, t: date, m: marker, v: marker}));
                assert.commandWorked(nonTsColl.insert({marker: marker}));
            }

            withChangeStreamTest(db, (cst) => {
                new FCVUpgradeCommand().execute(testHelper.getConn());

                const cursor = openChangeStreamCursor(cst, 1, {
                    showSystemEvents: false,
                    rawData: true,
                    watchMode,
                    comment,
                });
                const v2CursorId = cursor.id;

                // Pre FCV Downgrade.
                {
                    insertData(1, "preDowngrade", date1);
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: [
                            {
                                operationType: "insert",
                                fullDocument: {
                                    "control": {
                                        "version": 2,
                                        "min": {"_id": 1, "t": date1, "v": "preDowngrade"},
                                        "max": {"_id": 1, "t": date1, "v": "preDowngrade"},
                                        "count": 1,
                                    },
                                    "meta": "preDowngrade",
                                    "data": {
                                        "t": BinData(7, "CQAAcHMxjwEAAAA="),
                                        "_id": BinData(7, "AQAAAAAAAADwPwA="),
                                        "v": BinData(7, "AgANAAAAcHJlRG93bmdyYWRlAAA="),
                                    },
                                },
                                ns: ts1.regularNss,
                            },
                            {
                                operationType: "insert",
                                fullDocument: {marker: "preDowngrade"},
                                ns: nonTsCollNss,
                            },
                        ],
                    });

                    if (testHelper.isShardedCluster()) {
                        assertOpenCursors(
                            st,
                            [st.shard0.shardName],
                            /*expectedConfigCursor=*/ false,
                            cursorCommentFilter(comment),
                        );
                    }
                }

                // Perform FCV downgrade.
                {
                    const downgradeCmd = new FCVDowngradeCommand({
                        timeseriesCollections: [ts1],
                        targetFCV: downgradeFCV,
                    });
                    downgradeCmd.execute(testHelper.getConn());
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: downgradeCmd.getChangeEvents({watchMode, watchedNss}),
                    });
                }

                // Post FCV Downgrade.
                {
                    insertData(2, "postDowngrade", date2);
                    cst.assertNextChangesEqual({
                        cursor: cursor,
                        expectedChanges: [
                            // NOTE: no postDowngrade timeseries insert event is expected here because view"ful" timeseries
                            // inserts are only observable with showSystemEvents:true.
                            {
                                operationType: "insert",
                                fullDocument: {marker: "postDowngrade"},
                                ns: nonTsCollNss,
                            },
                        ],
                    });

                    if (testHelper.isShardedCluster()) {
                        assertOpenCursors(
                            st,
                            testHelper.allShards(),
                            /*expectedConfigCursor=*/ true,
                            cursorCommentFilter(comment),
                        );
                    }
                }
                cst.assertNoChange(cursor);

                if (testHelper.isShardedCluster()) {
                    // The stream was reopened as v1 after RetryChangeStream, so the cursor ID must differ.
                    const v1CursorId = cursor.id;
                    assert.neq(v1CursorId, v2CursorId, "cursor should have been reopened after FCV downgrade");
                }
            });
        });
    });
}
