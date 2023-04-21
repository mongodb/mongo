/**
 * Tests that collection and index idents in the drop pending state are maintained in the
 * CollectionCatalog.
 *
 * @tags: [
 *     requires_persistence,
 *     requires_replication,
 *     requires_wiredtiger
 * ]
 */
(function() {
"use strict";

load("jstests/disk/libs/wt_file_helper.js");
load("jstests/libs/feature_flag_util.js");

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Set the history window to zero to explicitly control the oldest timestamp.
            minSnapshotHistoryWindowInSeconds: 0,
            logComponentVerbosity: tojson({storage: 1})
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const db = primary.getDB(dbName);

if (!FeatureFlagUtil.isEnabled(db, "PointInTimeCatalogLookups")) {
    jsTestLog("Skipping as featureFlagPointInTimeCatalogLookups is not enabled");
    rst.stopSet();
    return;
}

// Pause the checkpoint thread to control the checkpoint timestamp.
assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "alwaysOn"}));

const collName = "a";
assert.commandWorked(db.createCollection(collName));

const coll = db.getCollection(collName);
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({_id: i, x: i}));
}

assert.commandWorked(coll.createIndex({x: 1}));

const collUri = getUriForColl(coll);
const idIndexUri = getUriForIndex(coll, /*indexName=*/ "_id_");
const xIndexUri = getUriForIndex(coll, /*indexName=*/ "x_1");

jsTestLog("Idents: " + tojson({collection: collUri, idIndex: idIndexUri, xIndex: xIndexUri}));

// Take the initial checkpoint.
assert.commandWorked(db.adminCommand({fsync: 1}));

jsTestLog("Starting a two-phase index drop");
coll.dropIndex({x: 1});

// Deferring table drop for index.
checkLog.containsJson(primary, 22206, {
    ident: function(ident) {
        return ident == xIndexUri;
    }
});

// Registering drop pending index entry ident.
checkLog.containsJson(primary, 6825301, {
    ident: function(ident) {
        return ident == xIndexUri;
    }
});

// Perform an operation and take a checkpoint to advance the checkpoint timestamp. The ident reaper
// will drop any drop pending idents earlier than the checkpoint timestamp.
assert.commandWorked(db.adminCommand({appendOplogNote: 1, data: {msg: "advance timestamp"}}));
assert.commandWorked(db.adminCommand({fsync: 1}));

// "The ident was successfully dropped".
checkLog.containsJson(primary, 6776600, {
    ident: function(ident) {
        return ident == xIndexUri;
    }
});

// Deregistering drop pending ident.
checkLog.containsJson(primary, 6825302, {
    ident: function(ident) {
        return ident == xIndexUri;
    }
});

jsTestLog("Starting a two-phase collection drop");
coll.drop();

// Deferring table drop for index.
checkLog.containsJson(primary, 22206, {
    ident: function(ident) {
        return ident == idIndexUri;
    }
});

// Registering drop pending index entry ident.
checkLog.containsJson(primary, 6825301, {
    ident: function(ident) {
        return ident == idIndexUri;
    }
});

// Deferring table drop for collection.
checkLog.containsJson(primary, 22214, {
    ident: function(ident) {
        return ident == collUri;
    }
});

// Registering drop pending collection ident.
checkLog.containsJson(primary, 6825300, {
    ident: function(ident) {
        return ident == collUri;
    }
});

// Perform an operation and take a checkpoint to advance the checkpoint timestamp. The ident reaper
// will drop any drop pending idents earlier than the checkpoint timestamp.
assert.commandWorked(db.adminCommand({appendOplogNote: 1, data: {msg: "advance timestamp"}}));
assert.commandWorked(db.adminCommand({fsync: 1}));

// "The ident was successfully dropped".
checkLog.containsJson(primary, 6776600, {
    ident: function(ident) {
        return ident == collUri;
    }
});
checkLog.containsJson(primary, 6776600, {
    ident: function(ident) {
        return ident == idIndexUri;
    }
});

// Deregistering drop pending ident.
checkLog.containsJson(primary, 6825302, {
    ident: function(ident) {
        return ident == collUri;
    }
});
checkLog.containsJson(primary, 6825302, {
    ident: function(ident) {
        return ident == idIndexUri;
    }
});

assert.commandWorked(
    primary.adminCommand({configureFailPoint: "pauseCheckpointThread", mode: "off"}));

rst.stopSet();
}());
