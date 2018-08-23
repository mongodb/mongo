// Runner for validateCollections that runs full validation on all collections when loaded into
// the mongo shell.
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.
    load('jstests/libs/parallelTester.js');     // For ScopedThread.

    assert.eq(typeof db, 'object', 'Invalid `db` object, is the shell connected to a mongod?');
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

    const hostList = [];
    let setFCVHost;

    if (topology.type === Topology.kStandalone) {
        hostList.push(topology.mongod);
        setFCVHost = topology.mongod;
    } else if (topology.type === Topology.kReplicaSet) {
        hostList.push(...topology.nodes);
        setFCVHost = topology.primary;
    } else if (topology.type === Topology.kShardedCluster) {
        hostList.push(...topology.configsvr.nodes);

        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];

            if (shard.type === Topology.kStandalone) {
                hostList.push(shard.mongod);
            } else if (shard.type === Topology.kReplicaSet) {
                hostList.push(...shard.nodes);
            } else {
                throw new Error('Unrecognized topology format: ' + tojson(topology));
            }
        }
        // Any of the mongos instances can be used for setting FCV.
        setFCVHost = topology.mongos.nodes[0];
    } else {
        throw new Error('Unrecognized topology format: ' + tojson(topology));
    }

    // Run a separate thread to validate collections on each server in parallel.
    var validateCollectionsThread = function(host, testData) {
        load('jstests/hooks/validate_collections.js');  // For validateCollections.
        TestData = testData;  // Pass the TestData object from main thread.

        try {
            print('Running validate() on ' + host);
            const conn = new Mongo(host);
            conn.setSlaveOk();
            jsTest.authenticate(conn);

            if (jsTest.options().forceValidationWithFeatureCompatibilityVersion) {
                let adminDB = conn.getDB('admin');
                // Make sure this node has the desired FCV.
                assert.soon(() => {
                    const res =
                        adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
                    return res !== null &&
                        res.version ===
                        jsTest.options().forceValidationWithFeatureCompatibilityVersion;
                });
            }

            const dbNames = conn.getDBNames();
            for (let dbName of dbNames) {
                if (!validateCollections(conn.getDB(dbName), {full: true})) {
                    return {ok: 0};
                }
            }
            return {ok: 1};
        } catch (e) {
            print('Exception caught in scoped thread running validationCollections on server: ' +
                  host);
            return {ok: 0, error: e.toString(), stack: e.stack};
        }
    };

    // We run the scoped threads in a try/finally block in case any thread throws an exception, in
    // which case we want to still join all the threads.
    let threads = [];
    let adminDB;
    let originalFCV;

    function getFeatureCompatibilityVersion(adminDB) {
        const res = adminDB.system.version.findOne({_id: "featureCompatibilityVersion"});
        if (res === null) {
            return "3.2";
        }
        return res.version;
    }

    if (jsTest.options().forceValidationWithFeatureCompatibilityVersion) {
        let conn = new Mongo(setFCVHost);
        adminDB = conn.getDB('admin');
        try {
            originalFCV = getFeatureCompatibilityVersion(adminDB);
        } catch (e) {
            if (jsTest.options().skipValidationOnInvalidViewDefinitions &&
                e.code === ErrorCodes.InvalidViewDefinition) {
                print("Reading the featureCompatibilityVersion from the admin.system.version" +
                      " collection failed due to an invalid view definition on the admin database");
                // The view catalog would only have been resolved if the namespace doesn't exist as
                // a collection. The absence of the admin.system.version collection is equivalent to
                // having featureCompatibilityVersion=3.2.
                originalFCV = "3.2";
            } else {
                throw e;
            }
        }

        if (originalFCV !== jsTest.options().forceValidationWithFeatureCompatibilityVersion) {
            assert.commandWorked(adminDB.adminCommand({
                setFeatureCompatibilityVersion:
                    jsTest.options().forceValidationWithFeatureCompatibilityVersion
            }));
        }
    }

    try {
        hostList.forEach(host => {
            const thread = new ScopedThread(validateCollectionsThread, host, TestData);
            threads.push(thread);
            thread.start();
        });
    } finally {
        // Wait for each thread to finish. Throw an error if any thread fails.
        const returnData = threads.map(thread => {
            thread.join();
            return thread.returnData();
        });

        returnData.forEach(res => {
            assert.commandWorked(res, 'Collection validation failed');
        });
    }

    if (jsTest.options().forceValidationWithFeatureCompatibilityVersion !== originalFCV) {
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: originalFCV}));
    }
})();
