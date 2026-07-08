/**
 * Tests that serverStatus correctly reflects change stream option and scope usage counters
 * (metrics.changeStreams.option.* and metrics.changeStreams.scope.*) when change streams are
 * opened on mongod and mongos.
 *
 * Runs in noPassthrough to start its own replica set and sharded cluster, to avoid interference
 * from passthrough overrides that re-scope change streams (whole_db/whole_cluster passthrough
 * suites rewrite collection/db-level streams to higher scopes, which breaks exact scope counter
 * assertions).
 *
 * @tags: [
 *   uses_change_streams,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    assertDropAndRecreateCollection,
    assertDropCollection,
} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let testDB, testColl;

function getCsMetrics() {
    return assert.commandWorked(testDB.adminCommand({serverStatus: 1, metrics: 1})).metrics
        .changeStreams;
}

function openAndClose(stageOpts) {
    const cursor = testColl.aggregate([{$changeStream: stageOpts}]);
    cursor.close();
}

function buildTests(isMongos) {
    describe("metrics.changeStreams.option boolean counters", function () {
        // 'showMigrationEvents' cannot be used on mongos. It will always produce error
        // 31123 ("Change streams from router may not show migration events").
        const booleanOptions = [
            "showExpandedEvents",
            "showMigrationEvents",
            "showSystemEvents",
            "showRawUpdateDescription",
            "ignoreRemovedShards",
        ];

        if (isMongos) {
            it("showMigrationEvents metric is present and remains 0 on mongos", function () {
                const before = getCsMetrics().option.showMigrationEvents;
                assert.eq(0, before, "showMigrationEvents should be 0 on mongos");
                assert.commandFailedWithCode(
                    testDB.runCommand({
                        aggregate: testColl.getName(),
                        pipeline: [{$changeStream: {showMigrationEvents: true}}],
                        cursor: {},
                    }),
                    31123,
                );
                assert.eq(
                    before,
                    getCsMetrics().option.showMigrationEvents,
                    "showMigrationEvents counter should not increment on mongos",
                );
            });
        }

        booleanOptions.forEach((option) => {
            it(`${option} increments when set to true`, function () {
                if (isMongos && option === "showMigrationEvents") {
                    // 'showMigrationEvents' cannot be used on mongos.
                    return;
                }

                const before = getCsMetrics().option[option];
                openAndClose({[option]: true});
                assert.eq(
                    before + 1,
                    getCsMetrics().option[option],
                    `${option} counter should have incremented`,
                );
            });
        });

        booleanOptions.forEach((option) => {
            it(`${option} does NOT increment when explicitly set to false`, function () {
                const before = getCsMetrics();
                openAndClose({[option]: false});
                const after = getCsMetrics();
                assert.eq(
                    before.option[option],
                    after.option[option],
                    `${option} should not increment when explicitly false`,
                );
            });
        });

        booleanOptions.forEach((option) => {
            it(`${option} does NOT increment when omitted`, function () {
                const before = getCsMetrics();
                openAndClose({});
                const after = getCsMetrics();
                assert.eq(
                    before.option[option],
                    after.option[option],
                    `${option} should not increment when omitted`,
                );
            });
        });
    });

    describe("matchCollectionUUIDForUpdateLookup counters", function () {
        it("matchCollectionUUIDForUpdateLookup increments when set to true", function () {
            const before = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            openAndClose({fullDocument: "updateLookup", matchCollectionUUIDForUpdateLookup: true});
            const after = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            assert.eq(
                before + 1,
                after,
                "matchCollectionUUIDForUpdateLookup counter should have incremented",
            );
        });

        it("matchCollectionUUIDForUpdateLookup does not increment when set to false", function () {
            const before = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            openAndClose({fullDocument: "updateLookup", matchCollectionUUIDForUpdateLookup: false});
            const after = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            assert.eq(
                before,
                after,
                "matchCollectionUUIDForUpdateLookup should not increment when explicitly false",
            );
        });

        it("matchCollectionUUIDForUpdateLookup should not increment when omitted", function () {
            const before = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            openAndClose({});
            const after = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
            assert.eq(
                before,
                after,
                "matchCollectionUUIDForUpdateLookup should not increment when omitted",
            );
        });
    });

    describe("metrics.changeStreams.option.fullDocument counters", function () {
        const fullDocumentOptions = ["required", "updateLookup", "whenAvailable"];

        fullDocumentOptions.forEach((option) => {
            it(`${option} increments when fullDocument=${option}`, function () {
                const before = getCsMetrics().option.fullDocument[option];
                openAndClose({fullDocument: option});
                assert.eq(
                    before + 1,
                    getCsMetrics().option.fullDocument[option],
                    `fullDocument.${option} should have incremented`,
                );
            });
        });

        it("default value does NOT increment any fullDocument counter", function () {
            const before = getCsMetrics().option.fullDocument;
            openAndClose({fullDocument: "default"});
            const after = getCsMetrics().option.fullDocument;
            fullDocumentOptions.forEach((option) => {
                assert.eq(
                    before[option],
                    after[option],
                    `fullDocument.${option} should not increment for default`,
                );
            });
        });
    });

    describe("metrics.changeStreams.option.fullDocumentBeforeChange counters", function () {
        const fullDocumentBeforeChangeOptions = ["required", "whenAvailable"];

        fullDocumentBeforeChangeOptions.forEach((option) => {
            it(`${option} increments when fullDocumentBeforeChange=${option}`, function () {
                const before = getCsMetrics().option.fullDocumentBeforeChange[option];
                openAndClose({fullDocumentBeforeChange: option});
                assert.eq(
                    before + 1,
                    getCsMetrics().option.fullDocumentBeforeChange[option],
                    `fullDocumentBeforeChange.${option} should have incremented`,
                );
            });
        });

        it("off value does NOT increment any fullDocumentBeforeChange counter", function () {
            const before = getCsMetrics().option.fullDocumentBeforeChange;
            openAndClose({fullDocumentBeforeChange: "off"});
            const after = getCsMetrics().option.fullDocumentBeforeChange;

            fullDocumentBeforeChangeOptions.forEach((option) => {
                assert.eq(
                    before[option],
                    after[option],
                    `fullDocumentBeforeChange.${option} should not increment for off`,
                );
            });
        });
    });

    describe("metrics.changeStreams.option resume/start counters", function () {
        const insertDocumentAndGetResumeToken = (doc) => {
            const cs = testColl.watch([]);
            try {
                assert.commandWorked(testColl.insert(doc));
                assert.soon(() => cs.hasNext());
                return cs.next()._id;
            } finally {
                cs.close();
            }
        };

        ["resumeAfter", "startAfter"].forEach((resumeOption, id) => {
            it(`${resumeOption} increments when the option is provided`, function () {
                const token = insertDocumentAndGetResumeToken({_id: id + 2});
                const before = getCsMetrics().option[resumeOption];
                openAndClose({[resumeOption]: token});
                assert.eq(
                    before + 1,
                    getCsMetrics().option[resumeOption],
                    `${resumeOption} counter should have incremented`,
                );
            });
        });

        it("startAtOperationTime increments when the option is provided", function () {
            const before = getCsMetrics().option.startAtOperationTime;
            const recentTs = new Timestamp(Math.floor(Date.now() / 1000) - 1, 1);
            openAndClose({startAtOperationTime: recentTs});
            assert.eq(
                before + 1,
                getCsMetrics().option.startAtOperationTime,
                "startAtOperationTime counter should have incremented",
            );
        });

        it("startAtOperationTime does NOT increment when no resume option is given", function () {
            const before = getCsMetrics().option.startAtOperationTime;
            openAndClose({});
            assert.eq(
                getCsMetrics().option.startAtOperationTime,
                before,
                "startAtOperationTime counter should not increment for auto-set value",
            );
        });
    });

    describe("metrics.changeStreams.scope counters", function () {
        const scopes = {
            collection: () => {
                openAndClose({});
            },
            db: () => {
                const dbCursor = testDB.aggregate([{$changeStream: {}}]);
                dbCursor.close();
            },
            cluster: () => {
                const adminDB = testDB.getSiblingDB("admin");
                const clusterCursor = adminDB.aggregate([
                    {$changeStream: {allChangesForCluster: true}},
                ]);
                clusterCursor.close();
            },
        };

        for (const [scope, initFn] of Object.entries(scopes)) {
            it(`scope.${scope} increments for a ${scope}-level change stream`, function () {
                const before = getCsMetrics().scope[scope];
                initFn();
                assert.eq(
                    before + 1,
                    getCsMetrics().scope[scope],
                    `scope.${scope} should have incremented`,
                );
            });
        }
    });
}

describe("change stream metrics in replica set", function () {
    let rst;

    before(function () {
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
        rst.startSet();
        rst.initiate();
        testDB = rst.getPrimary().getDB(jsTestName());
        testColl = testDB.getCollection("test");
        assertDropAndRecreateCollection(testDB, testColl.getName());
        assert.commandWorked(testColl.insert({_id: 1}));
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
        rst.stopSet();
    });

    buildTests(false);
});

describe("change stream metrics on mongos", function () {
    let st;

    before(function () {
        st = new ShardingTest({
            shards: 1,
            rs: {
                nodes: 1,
                setParameter: {
                    writePeriodicNoops: true,
                    periodicNoopIntervalSecs: 1,
                },
            },
        });

        testDB = st.s.getDB(jsTestName());
        testColl = testDB.getCollection("test");
        assertDropAndRecreateCollection(testDB, testColl.getName());
        assert.commandWorked(testColl.insert({_id: 1}));
    });

    after(function () {
        assertDropCollection(testDB, testColl.getName());
        st.stop();
    });

    buildTests(true);
});
