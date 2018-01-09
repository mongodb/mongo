/**
 * Loading this file overrides DB.prototype.getCollection() with a function that attempts to shard
 * the collection before returning it.
 *
 * The DB.prototype.getCollection() function is called whenever an undefined property is accessed
 * on the db object.
 *
 * DBCollection.prototype.drop() function will re-shard any non-blacklisted collection that is
 * dropped in a sharded cluster.
 */

(function() {
    'use strict';

    // Save a reference to the original getCollection method in the IIFE's scope.
    // This scoping allows the original method to be called by the getCollection override below.
    var originalGetCollection = DB.prototype.getCollection;
    var originalDBCollectionDrop = DBCollection.prototype.drop;
    var originalStartParallelShell = startParallelShell;
    var testMayRunDropInParallel = false;

    // Blacklisted namespaces that should not be sharded.
    var blacklistedNamespaces = [
        /\$cmd/,
        /^admin\./,
        /^config\./,
        /\.system\./,
    ];

    function shardCollection(collection) {
        var db = collection.getDB();
        var dbName = db.getName();
        var fullName = collection.getFullName();

        for (var ns of blacklistedNamespaces) {
            if (fullName.match(ns)) {
                return;
            }
        }

        var res = db.adminCommand({enableSharding: dbName});

        // enableSharding may only be called once for a database.
        if (res.code !== ErrorCodes.AlreadyInitialized) {
            assert.commandWorked(res, "enabling sharding on the '" + dbName + "' db failed");
        }

        res = db.adminCommand(
            {shardCollection: fullName, key: {_id: 'hashed'}, collation: {locale: "simple"}});
        if (res.ok === 0 && testMayRunDropInParallel) {
            // We ignore ConflictingOperationInProgress error responses from the
            // "shardCollection" command if it's possible the test was running a "drop" command
            // concurrently. We could retry running the "shardCollection" command, but tests
            // that are likely to trigger this case are also likely running the "drop" command
            // in a loop. We therefore just let the test continue with the collection being
            // unsharded.
            assert.commandFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
            print("collection '" + fullName +
                  "' failed to be sharded due to a concurrent drop operation");
        } else {
            assert.commandWorked(res, "sharding '" + fullName + "' with a hashed _id key failed");
        }
    }

    DB.prototype.getCollection = function() {
        var collection = originalGetCollection.apply(this, arguments);

        const collStats = this.runCommand({collStats: collection.getName()});

        // If the collection is already sharded or is non-empty, do not attempt to shard.
        if (collStats.sharded || collStats.count > 0) {
            return collection;
        }

        // Attempt to enable sharding on database and collection if not already done.
        shardCollection(collection);

        return collection;
    };

    DBCollection.prototype.drop = function() {
        var dropResult = originalDBCollectionDrop.apply(this, arguments);

        // Attempt to enable sharding on database and collection if not already done.
        shardCollection(this);

        return dropResult;
    };

    // Tests may use a parallel shell to run the "drop" command concurrently with other
    // operations. This can cause the "shardCollection" command to return a
    // ConflictingOperationInProgress error response.
    startParallelShell = function() {
        testMayRunDropInParallel = true;
        return originalStartParallelShell.apply(this, arguments);
    };
}());
