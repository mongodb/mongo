'use strict';

/**
 * Stress tests writes to config.databases while continuously running setFCV to ensure that the
 * config.databases schema always matches the FCV.
 *
 * @tags: [requires_sharding]
 */
var $config = (function() {

    var states = (function() {

        function init(db, collName) {
            // Dynamically load the shard names for the movePrimary thread to avoid hard-coding
            // them.
            this.shards = db.getSiblingDB("config").shards.find().toArray().map(shard => shard._id);
        }

        function setFCV(db, data) {
            if (data.fcv === undefined) {
                data.fcv = "4.0";
            }

            // First check that the current entries match the current FCV's schema.
            const databases = db.getSiblingDB("config").databases.find().toArray();
            for (let i in databases) {
                const database = databases[i];
                if (data.fcv === "3.6") {
                    assertAlways.eq(undefined,
                                    database.version,
                                    "database had a version in FCV 3.6: " + tojson(database));
                } else {
                    assertAlways.neq(
                        undefined,
                        database.version,
                        "database did not have a version in FCV 4.0: " + tojson(database));
                }
            }

            // Then change the FCV.
            data.fcv = (data.fcv === "4.0") ? "3.6" : "4.0";
            assertAlways.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: data.fcv}));
        }

        function createWithEnableSharding(db, data) {
            data.dbNameCount = (data.dbNameCount === undefined) ? 0 : data.dbNameCount + 1;
            assertAlways.commandWorked(
                db.adminCommand({enableSharding: "createWithEnableShardingDb" + data.dbNameCount}));
        }

        function createWithInsert(db, data) {
            data.dbNameCount = (data.dbNameCount === undefined) ? 0 : data.dbNameCount + 1;
            assertAlways.commandWorked(
                db.getSiblingDB("createWithInsertDb" + data.dbNameCount).foo.insert({x: 1}));
        }

        function movePrimary(db, data) {
            // Assume an arbitrary shard is the current primary shard; if it's not, the first
            // iteration will be a no-op.
            if (data.primaryShard === undefined) {
                data.primaryShard = data.shards[0];
            }

            const toShard =
                (data.primaryShard === data.shards[0]) ? data.shards[1] : data.shards[0];
            const res = db.adminCommand({movePrimary: "movePrimaryDb", to: toShard});

            // movePrimary will correctly error if the FCV changes while it is running.
            if (res.code === ErrorCodes.ConflictingOperationInProgress) {
                return;
            }

            assertAlways.commandWorked(res);
            data.primaryShard = toShard;
        }

        function state(db, collName) {
            switch (this.tid) {
                case 0:
                    setFCV(db, this);
                    break;
                case 1:
                    createWithEnableSharding(db, this);
                    break;
                case 2:
                    createWithInsert(db, this);
                    break;
                case 3:
                    movePrimary(db, this);
                    break;
            }
        }

        return {init: init, state: state};

    })();

    var transitions = {init: {state: 1}, state: {state: 1}};

    function setup(db, collName, cluster) {
        // Ensure the cluster starts in FCV 4.0.
        assertAlways.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: "4.0"}));

        // Create a database for the thread doing movePrimary. Use 'enableSharding' rather than
        // an insert to create the database (if an unsharded collection exists in a database and an
        // initial movePrimary attempt fails after copying the unsharded collection to the new
        // primary, retrying the movePrimary will fail).
        assertAlways.commandWorked(db.adminCommand({enableSharding: "movePrimaryDb"}));
    }

    function teardown(db, collName, cluster) {
        // Ensure all databases have data in them so that they show up in listDatabases (which calls
        // listDatabases against all shards rather than reading config.databases). This guarantees
        // they are dropped by the cleanup hook.
        const databases = db.getSiblingDB("config").databases.find().toArray();
        for (let i in databases) {
            const database = databases[i];
            assertAlways.commandWorked(db.getSiblingDB(databases[i]._id).foo.insert({x: 1}));
        }

        // If this workload is run with --repeat, mongos will already have all the database entries
        // cached. Because of SERVER-xxx (mongos does not mark its database entry as invalid on
        // CannotImplicitlyCreateCollection), this mongos will never realize the databases have been
        // dropped, and so will never send the implicit createDatabase for writes in the next run
        // (and instead will exhaust retries of CannotImplicitlyCreateCollection).
        // As a temporary workaround, flush mongos's cache at the end of each workload.
        assertAlways.commandWorked(db.adminCommand({flushRouterConfig: 1}));
    }

    // This test performs sharding catalog operations (which take distributed locks) concurrently
    // from many threads. Since a distributed lock is acquired by repeatedly attempting to grab the
    // lock every half second for 20 seconds (a max of 40 attempts), it's possible that some thread
    // will be starved by the other threads and fail to grab the lock after 40 attempts. To reduce
    // the likelihood of this, we choose threadCount and iterations so that threadCount * iterations
    // does not exceed 40.
    // Note that this test's structure requires at least 4 threads (one per sharding catalog op).
    // The iterations can be increased after PM-697 ("Remove all usages of distributed lock").
    return {
        threadCount: 4,
        iterations: 10,
        data: {},
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };

})();
