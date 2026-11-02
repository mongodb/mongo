/**
 * Loading this file overrides runCommand with a function that converts any attempt to run
 * $changeStream on a single collection or single database into a cluster-wide $changeStream
 * filtered by that database or namespace. Single-collection/db rules regarding internal
 * collections and views are respected. Non-$changeStream commands and commands which explicitly
 * request to be exempted from modification via 'changeStreamPassthroughAwareRunCommand', are
 * passed through as-is.
 */

// For the whole_cluster passthrough, we simply override the necessary methods in the whole_db
// passthrough's ChangeStreamPassthroughHelpers.
import "jstests/libs/override_methods/query/implicit_whole_db_changestreams.js";

import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";

// Any valid single-collection or single-database request is upconvertable to cluster-wide.
globalThis.ChangeStreamPassthroughHelpers.isUpconvertableChangeStreamRequest =
    globalThis.ChangeStreamPassthroughHelpers.isValidChangeStreamRequest;

globalThis.ChangeStreamPassthroughHelpers.nsMatchFilter = function (db, collName) {
    // The $match filter we inject into the pipeline will depend on whether this is a
    // single-collection or whole-db stream.
    const isSingleCollectionStream = typeof collName === "string";

    const orBranches = [
        {"ns.db": db.getName(), "ns.coll": isSingleCollectionStream ? collName : {$exists: true}},
        // Add a clause to detect if the collection being watched is the target of a
        // renameCollection command, since that is expected to return a "rename" entry.
        {"to.db": db.getName(), "to.coll": isSingleCollectionStream ? collName : {$exists: true}},
        {operationType: "endOfTransaction"},
        {operationType: "invalidate"},
    ];

    if (!isSingleCollectionStream) {
        orBranches.push({
            operationType: "dropDatabase",
            "ns.db": db.getName(),
        });
    }

    return {$match: {$or: orBranches}};
};

globalThis.ChangeStreamPassthroughHelpers.execDBName = function () {
    return "admin";
};

globalThis.ChangeStreamPassthroughHelpers.changeStreamSpec = function () {
    return {allChangesForCluster: true};
};

globalThis.ChangeStreamPassthroughHelpers.passthroughType = function () {
    return ChangeStreamWatchMode.kCluster;
};

// Redirect the DB's 'watch' function to use the cluster-wide version. The Collection.watch helper
// has already been overridden to use DB.watch when we loaded 'implicit_whole_db_changestreams.js',
// so this ensures that both the Collection and DB helpers will actually call the Mongo function.
// Although calls to the shell helpers will ultimately resolve to the overridden runCommand anyway,
// we need to override the helper to ensure that the Mongo.watch function itself is exercised by the
// passthrough wherever Collection.watch or DB.watch is called.
const originalDbWatchImpl = DB.prototype.watch;
DB.prototype.watch = function (pipeline, options) {
    // If the database being watched is 'admin', don't attempt to upconvert.
    if (this.getName() === "admin") {
        return originalDbWatchImpl.apply(this, [pipeline, options]);
    }
    pipeline = Object.assign([], pipeline);
    pipeline.unshift(globalThis.ChangeStreamPassthroughHelpers.nsMatchFilter(this, 1));
    return this.getMongo().watch(pipeline, options);
};

// Register this override for parallel shells as well. A parallel shell only replays the override
// files registered via 'prependOverrideInParallelShell', not the suite's own imports. The whole-db
// override registers itself when it is loaded (transitively from this module), but without the
// registration below a child shell would load only that whole-db override and its change streams
// would run at database scope instead of cluster scope.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_whole_cluster_changestreams.js",
);
