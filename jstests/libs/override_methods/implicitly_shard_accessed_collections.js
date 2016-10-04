/**
 * Loading this file overrides DB.prototype.getCollection() with a function that attempts to shard
 * the collection before returning it.
 *
 * The DB.prototype.getCollection() function is called whenever an undefined property is accessed
 * on the db object.
 */

(function() {
    'use strict';

    // Save a reference to the original getCollection method in the IIFE's scope.
    // This scoping allows the original method to be called by the getCollection override below.
    var originalGetCollection = DB.prototype.getCollection;

    // Blacklisted namespaces that should not be sharded.
    var blacklistedNamespaces = [
        /\$cmd/,
        /^admin\./,
        /^config\./,
        /\.system\./,
    ];

    DB.prototype.getCollection = function() {
        var dbName = this.getName();
        var collection = originalGetCollection.apply(this, arguments);
        var fullName = collection.getFullName();

        // If the collection exists, there must have been a previous call to getCollection
        // where we sharded the collection so there's no need to do it again.
        if (collection.exists()) {
            return collection;
        }

        for (var ns of blacklistedNamespaces) {
            if (fullName.match(ns)) {
                return collection;
            }
        }

        var res = this.adminCommand({enableSharding: dbName});

        // enableSharding may only be called once for a database.
        if (res.code !== ErrorCodes.AlreadyInitialized) {
            assert.commandWorked(res, "enabling sharding on the '" + dbName + "' db failed");
        }

        res = this.adminCommand({shardCollection: fullName, key: {_id: 'hashed'}});
        assert.commandWorked(res, "sharding '" + fullName + "' with a hashed _id key failed");

        return collection;
    };
}());
