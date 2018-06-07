/**
 * Loading this file overrides DB.prototype._runCommandImpl with a function that converts any
 * attempt to run $changeStream on a single collection or single database into a cluster-wide
 * $changeStream filtered by that database or namespace. Single-collection/db rules regarding
 * internal collections and views are respected. Non-$changeStream commands and commands which
 * explicitly request to be exempted from modification by setting the 'noPassthrough' flag, are
 * passed through as-is.
 */

// For the whole_cluster passthrough, we simply override the necessary methods in the whole_db
// passthrough's ChangeStreamPassthroughHelpers.
load("jstests/libs/override_methods/implicit_whole_db_changestreams.js");

// Any valid single-collection or single-database request is upconvertable to cluster-wide.
ChangeStreamPassthroughHelpers.isUpconvertableChangeStreamRequest =
    ChangeStreamPassthroughHelpers.isValidChangeStreamRequest;

ChangeStreamPassthroughHelpers.nsMatchFilter = function(db, collName) {
    // The $match filter we inject into the pipeline will depend on whether this is a
    // single-collection or whole-db stream.
    const isSingleCollectionStream = (typeof collName === 'string');

    return {
        $match: {
            $or: [
                {
                  "ns.db": db.getName(),
                  "ns.coll": (isSingleCollectionStream ? collName : {$exists: true})
                },
                // Add a clause to detect if the collection being watched is the target of a
                // renameCollection command, since that is expected to return a "rename" entry.
                {
                  "to.db": db.getName(),
                  "to.coll": (isSingleCollectionStream ? collName : {$exists: true})
                },
                {operationType: "invalidate"}
            ]
        }
    };
};

ChangeStreamPassthroughHelpers.execDBName = function(db) {
    return "admin";
};

ChangeStreamPassthroughHelpers.changeStreamSpec = function() {
    return {allChangesForCluster: true};
};

ChangeStreamPassthroughHelpers.passthroughType = function() {
    return ChangeStreamWatchMode.kCluster;
};

// Redirect the DB's 'watch' function to use the cluster-wide version. The Collection.watch helper
// has already been overridden to use DB.watch when we loaded 'implicit_whole_db_changestreams.js',
// so this ensures that both the Collection and DB helpers will actually call the Mongo function.
// Although calls to the shell helpers will ultimately resolve to the overridden runCommand anyway,
// we need to override the helper to ensure that the Mongo.watch function itself is exercised by the
// passthrough wherever Collection.watch or DB.watch is called.
DB.prototype.watch = function(pipeline, options) {
    pipeline = Object.assign([], pipeline);
    pipeline.unshift(ChangeStreamPassthroughHelpers.nsMatchFilter(this, 1));
    return this.getMongo().watch(pipeline, options);
};
