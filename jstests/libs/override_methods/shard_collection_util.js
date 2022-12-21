// Common routines for override functions that need to shard collections

var testMayRunDropInParallel = false;

const kZoneName = 'moveToHereForMigrationPassthrough';

var denylistedNamespaces = [
    /\$cmd/,
    /^admin\./,
    /^config\./,
    /\.system\./,
    /enxcol_\..*\.esc/,
    /enxcol_\..*\.ecc/,
    /enxcol_\..*\.ecoc/,
];

/**
 * Settings for the converting implictily accessed collections to sharded collections.
 */
const ImplicitlyShardAccessCollSettings = (function() {
    let mode = 0;  // Default to hashed shard key.

    return {
        Modes: {
            kUseHashedSharding: 0,
            kHashedMoveToSingleShard: 1,
        },
        setMode: function(newMode) {
            if (newMode !== 0 && newMode !== 1) {
                throw new Error("Cannot set mode to unknown mode: " + newMode);
            }

            mode = newMode;
        },
        getMode: function() {
            return mode;
        },
    };
})();

var ShardingOverrideCommon = (function() {
    "use strict";

    /**
     * Shard a collection
     * @param {*} collection is a shell object
     * @returns nothing
     */
    function shardCollection(collection) {
        return shardCollectionWithSpec(
            {db: collection.getDB(), collName: collection.getName(), shardKey: {_id: 'hashed'}});
    }

    /**
     * Shard a collection with the provided shard key and timeseries spec
     * @param {*} param0
     * @returns nothing
     */
    function shardCollectionWithSpec({db, collName, shardKey, timeseriesSpec}) {
        // Don't attempt to shard if this operation is running on mongoD.
        if (!FixtureHelpers.isMongos(db)) {
            return;
        }

        var dbName = db.getName();
        var fullName = dbName + "." + collName;

        for (var ns of denylistedNamespaces) {
            if (fullName.match(ns)) {
                return;
            }
        }

        var res = db.adminCommand({enableSharding: dbName});

        // enableSharding may only be called once for a database.
        if (res.code !== ErrorCodes.AlreadyInitialized) {
            assert.commandWorked(res, "enabling sharding on the '" + dbName + "' db failed");
        }

        let shardCollCmd = {
            shardCollection: fullName,
            key: shardKey,
            collation: {locale: "simple"}
        };
        if (timeseriesSpec) {
            shardCollCmd["timeseries"] = timeseriesSpec;
        }
        res = db.adminCommand(shardCollCmd);

        let checkResult = function(res, opDescription) {
            if (res.ok === 0 && testMayRunDropInParallel) {
                // We ignore ConflictingOperationInProgress error responses from the
                // "shardCollection" command if it's possible the test was running a "drop" command
                // concurrently. We could retry running the "shardCollection" command, but tests
                // that are likely to trigger this case are also likely running the "drop" command
                // in a loop. We therefore just let the test continue with the collection being
                // unsharded.
                assert.commandFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
                jsTest.log("Ignoring failure while " + opDescription +
                           " due to a concurrent drop operation: " + tojson(res));
            } else {
                assert.commandWorked(res, opDescription + " failed");
            }
        };

        checkResult(res, 'shard ' + fullName);

        // Set the entire chunk range to a single zone, so balancer will be forced to move the
        // evenly distributed chunks to a shard (selected at random).
        if (res.ok === 1 &&
            ImplicitlyShardAccessCollSettings.getMode() ===
                ImplicitlyShardAccessCollSettings.Modes.kHashedMoveToSingleShard) {
            let shardName =
                db.getSiblingDB('config').shards.aggregate([{$sample: {size: 1}}]).toArray()[0]._id;

            checkResult(db.adminCommand({addShardToZone: shardName, zone: kZoneName}),
                        'add ' + shardName + ' to zone ' + kZoneName);
            checkResult(db.adminCommand({
                updateZoneKeyRange: fullName,
                min: {_id: MinKey},
                max: {_id: MaxKey},
                zone: kZoneName
            }),
                        'set zone for ' + fullName);

            // Wake up the balancer.
            checkResult(db.adminCommand({balancerStart: 1}), 'turn on balancer');
        }
    }

    return {
        shardCollection: shardCollection,
        shardCollectionWithSpec: shardCollectionWithSpec,
    };
})();
