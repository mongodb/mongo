/**
 * Ensures that a createIndexes command request fails when the available disk space is below the
 * indexBuildMinAvailableDiskSpaceMB threshold.
 * @tags: [
 *   featureFlagIndexBuildGracefulErrorHandling,
 *   requires_fcv_71,
 *   requires_replication,
 * ]
 */

(function() {
"use strict";

load('jstests/libs/fail_point_util.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDB = primary.getDB('test');
const primaryColl = primaryDB.getCollection('test');

const simulateDiskSpaceFp =
    configureFailPoint(primaryDB, 'simulateAvailableDiskSpace', {bytes: 450 * 1024 * 1024});

// Empty collections do not start index builds, and should succeed.
assert.commandWorked(primaryColl.createIndex({b: 1}));

// Populate collection.
assert.commandWorked(primaryColl.insert({a: 1}));

// Index build should fail to start.
assert.commandFailedWithCode(primaryColl.createIndex({a: 1}), [ErrorCodes.OutOfDiskSpace]);
rst.stopSet();
})();
