/**
 * Tests that serverStatus correctly reflects change stream option and scope usage counters
 * (metrics.changeStreams.option.* and metrics.changeStreams.scope.*) when change streams are
 * opened on mongod.
 *
 * Runs in noPassthrough to start its own replica set and avoid interference from passthrough
 * overrides that re-scope change streams (whole_db/whole_cluster passthrough suites rewrite
 * collection/db-level streams to higher scopes, which breaks exact scope counter assertions).
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

let rst, testDB, testColl;

function getCsMetrics() {
    return assert.commandWorked(testDB.adminCommand({serverStatus: 1, metrics: 1})).metrics
        .changeStreams;
}

function openAndClose(stageOpts, collOverride) {
    const target = collOverride || testColl;
    const cursor = target.aggregate([{$changeStream: stageOpts}]);
    cursor.close();
}

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

describe("metrics.changeStreams.option boolean counters", function () {
    it("showExpandedEvents increments when set to true", function () {
        const before = getCsMetrics().option.showExpandedEvents;
        openAndClose({showExpandedEvents: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.showExpandedEvents,
            "showExpandedEvents counter should have incremented",
        );
    });

    it("showMigrationEvents increments when set to true", function () {
        const before = getCsMetrics().option.showMigrationEvents;
        openAndClose({showMigrationEvents: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.showMigrationEvents,
            "showMigrationEvents counter should have incremented",
        );
    });

    it("showSystemEvents increments when set to true", function () {
        const before = getCsMetrics().option.showSystemEvents;
        openAndClose({showSystemEvents: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.showSystemEvents,
            "showSystemEvents counter should have incremented",
        );
    });

    it("showRawUpdateDescription increments when set to true", function () {
        const before = getCsMetrics().option.showRawUpdateDescription;
        openAndClose({showRawUpdateDescription: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.showRawUpdateDescription,
            "showRawUpdateDescription counter should have incremented",
        );
    });

    it("ignoreRemovedShards increments when set to true", function () {
        const before = getCsMetrics().option.ignoreRemovedShards;
        openAndClose({ignoreRemovedShards: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.ignoreRemovedShards,
            "ignoreRemovedShards counter should have incremented",
        );
    });

    it("matchCollectionUUIDForUpdateLookup increments when set to true", function () {
        const before = getCsMetrics().option.matchCollectionUUIDForUpdateLookup;
        openAndClose({fullDocument: "updateLookup", matchCollectionUUIDForUpdateLookup: true});
        assert.eq(
            before + 1,
            getCsMetrics().option.matchCollectionUUIDForUpdateLookup,
            "matchCollectionUUIDForUpdateLookup counter should have incremented",
        );
    });

    it("boolean options do NOT increment when explicitly set to false", function () {
        const before = getCsMetrics();
        openAndClose({showExpandedEvents: false});
        const after = getCsMetrics();
        assert.eq(
            before.option.showExpandedEvents,
            after.option.showExpandedEvents,
            "showExpandedEvents should not increment when explicitly false",
        );
        openAndClose({showMigrationEvents: false});
        assert.eq(
            before.option.showMigrationEvents,
            getCsMetrics().option.showMigrationEvents,
            "showMigrationEvents should not increment when explicitly false",
        );
        openAndClose({showSystemEvents: false});
        assert.eq(
            before.option.showSystemEvents,
            getCsMetrics().option.showSystemEvents,
            "showSystemEvents should not increment when explicitly false",
        );
        openAndClose({showRawUpdateDescription: false});
        assert.eq(
            before.option.showRawUpdateDescription,
            getCsMetrics().option.showRawUpdateDescription,
            "showRawUpdateDescription should not increment when explicitly false",
        );
        openAndClose({ignoreRemovedShards: false});
        assert.eq(
            before.option.ignoreRemovedShards,
            getCsMetrics().option.ignoreRemovedShards,
            "ignoreRemovedShards should not increment when explicitly false",
        );
        openAndClose({fullDocument: "updateLookup", matchCollectionUUIDForUpdateLookup: false});
        assert.eq(
            before.option.matchCollectionUUIDForUpdateLookup,
            getCsMetrics().option.matchCollectionUUIDForUpdateLookup,
            "matchCollectionUUIDForUpdateLookup should not increment when explicitly false",
        );
    });

    it("boolean options do NOT increment when omitted", function () {
        const before = getCsMetrics();
        openAndClose({});
        const after = getCsMetrics();
        assert.eq(
            before.option.showExpandedEvents,
            after.option.showExpandedEvents,
            "showExpandedEvents should not increment when omitted",
        );
        assert.eq(
            before.option.showMigrationEvents,
            after.option.showMigrationEvents,
            "showMigrationEvents should not increment when omitted",
        );
        assert.eq(
            before.option.showSystemEvents,
            after.option.showSystemEvents,
            "showSystemEvents should not increment when omitted",
        );
        assert.eq(
            before.option.showRawUpdateDescription,
            after.option.showRawUpdateDescription,
            "showRawUpdateDescription should not increment when omitted",
        );
        assert.eq(
            before.option.ignoreRemovedShards,
            after.option.ignoreRemovedShards,
            "ignoreRemovedShards should not increment when omitted",
        );
        assert.eq(
            before.option.matchCollectionUUIDForUpdateLookup,
            after.option.matchCollectionUUIDForUpdateLookup,
            "matchCollectionUUIDForUpdateLookup should not increment when omitted",
        );
    });
});

describe("metrics.changeStreams.option.fullDocument counters", function () {
    it("required increments when fullDocument=required", function () {
        const before = getCsMetrics().option.fullDocument.required;
        openAndClose({fullDocument: "required"});
        assert.eq(
            before + 1,
            getCsMetrics().option.fullDocument.required,
            "fullDocument.required should have incremented",
        );
    });

    it("updateLookup increments when fullDocument=updateLookup", function () {
        const before = getCsMetrics().option.fullDocument.updateLookup;
        openAndClose({fullDocument: "updateLookup"});
        assert.eq(
            before + 1,
            getCsMetrics().option.fullDocument.updateLookup,
            "fullDocument.updateLookup should have incremented",
        );
    });

    it("whenAvailable increments when fullDocument=whenAvailable", function () {
        const before = getCsMetrics().option.fullDocument.whenAvailable;
        openAndClose({fullDocument: "whenAvailable"});
        assert.eq(
            before + 1,
            getCsMetrics().option.fullDocument.whenAvailable,
            "fullDocument.whenAvailable should have incremented",
        );
    });

    it("default value does NOT increment any fullDocument counter", function () {
        const before = getCsMetrics().option.fullDocument;
        openAndClose({fullDocument: "default"});
        const after = getCsMetrics().option.fullDocument;
        assert.eq(
            before.required,
            after.required,
            "fullDocument.required should not increment for default",
        );
        assert.eq(
            before.updateLookup,
            after.updateLookup,
            "fullDocument.updateLookup should not increment for default",
        );
        assert.eq(
            before.whenAvailable,
            after.whenAvailable,
            "fullDocument.whenAvailable should not increment for default",
        );
    });
});

describe("metrics.changeStreams.option.fullDocumentBeforeChange counters", function () {
    it("required increments when fullDocumentBeforeChange=required", function () {
        const before = getCsMetrics().option.fullDocumentBeforeChange.required;
        openAndClose({fullDocumentBeforeChange: "required"});
        assert.eq(
            before + 1,
            getCsMetrics().option.fullDocumentBeforeChange.required,
            "fullDocumentBeforeChange.required should have incremented",
        );
    });

    it("whenAvailable increments when fullDocumentBeforeChange=whenAvailable", function () {
        const before = getCsMetrics().option.fullDocumentBeforeChange.whenAvailable;
        openAndClose({fullDocumentBeforeChange: "whenAvailable"});
        assert.eq(
            before + 1,
            getCsMetrics().option.fullDocumentBeforeChange.whenAvailable,
            "fullDocumentBeforeChange.whenAvailable should have incremented",
        );
    });

    it("off value does NOT increment any fullDocumentBeforeChange counter", function () {
        const before = getCsMetrics().option.fullDocumentBeforeChange;
        openAndClose({fullDocumentBeforeChange: "off"});
        const after = getCsMetrics().option.fullDocumentBeforeChange;
        assert.eq(
            before.required,
            after.required,
            "fullDocumentBeforeChange.required should not increment for off",
        );
        assert.eq(
            before.whenAvailable,
            after.whenAvailable,
            "fullDocumentBeforeChange.whenAvailable should not increment for off",
        );
    });
});

describe("metrics.changeStreams.option resume/start counters", function () {
    it("resumeAfter increments when the option is provided", function () {
        let token;
        const cs = testColl.watch([]);
        try {
            assert.commandWorked(testColl.insert({_id: 2}));
            assert.soon(() => cs.hasNext());
            token = cs.next()._id;
        } finally {
            cs.close();
        }

        const before = getCsMetrics().option.resumeAfter;
        openAndClose({resumeAfter: token});
        assert.eq(
            before + 1,
            getCsMetrics().option.resumeAfter,
            "resumeAfter counter should have incremented",
        );
    });

    it("startAfter increments when the option is provided", function () {
        let token;
        const cs = testColl.watch([]);
        try {
            assert.commandWorked(testColl.insert({_id: 3}));
            assert.soon(() => cs.hasNext());
            token = cs.next()._id;
        } finally {
            cs.close();
        }

        const before = getCsMetrics().option.startAfter;
        openAndClose({startAfter: token});
        assert.eq(
            before + 1,
            getCsMetrics().option.startAfter,
            "startAfter counter should have incremented",
        );
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
    it("scope.collection increments for a collection-level change stream", function () {
        const before = getCsMetrics().scope.collection;
        openAndClose({});
        assert.eq(
            before + 1,
            getCsMetrics().scope.collection,
            "scope.collection should have incremented",
        );
    });

    it("scope.db increments for a database-level change stream", function () {
        const before = getCsMetrics().scope.db;
        const dbCursor = testDB.aggregate([{$changeStream: {}}]);
        dbCursor.close();
        assert.eq(before + 1, getCsMetrics().scope.db, "scope.db should have incremented");
    });

    it("scope.cluster increments for a cluster-level change stream", function () {
        const before = getCsMetrics().scope.cluster;
        const adminDB = testDB.getSiblingDB("admin");
        const clusterCursor = adminDB.aggregate([{$changeStream: {allChangesForCluster: true}}]);
        clusterCursor.close();
        assert.eq(
            before + 1,
            getCsMetrics().scope.cluster,
            "scope.cluster should have incremented",
        );
    });
});
