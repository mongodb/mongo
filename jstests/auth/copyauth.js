// Test copyDatabase command with various combinations of authed/unauthed and single node/replica
// set source and dest.

TestData.authMechanism = "SCRAM-SHA-1";                        // SERVER-11428
DB.prototype._defaultAuthenticationMechanism = "SCRAM-SHA-1";  // SERVER-11428

var baseName = "jstests_clone_copyauth";

/*
 * Helper to spawn a replica set, sharded cluster, or a single mongod and hide it all behind the
 * same interface.
 *
 * Arguments:
 *
 * clusterType - type of cluster to start.  Options are "sharded", "repl", or "single".
 * startWithAuth - whether to start the cluster with authentication.
 * startWithTransitionToAuth - whether to start the cluster with --transitionToAuth (startWithAuth
 * must also be true).
 *
 * Member variables:
 *
 * conn - a connection to the node used to access this cluster, whether it's the mongod, a primary
 * mongod in a replica set, or a mongos.
 * connString - the full connection string used to connect to this cluster.  For a replica set this
 * is the full connection string including the replica set name.
 *
 * Member functions:
 *
 * stop() - stop and cleanup whatever nodes the helper spawned when it was created.
 */
function ClusterSpawnHelper(clusterType, startWithAuth, startWithTransitionToAuth) {
    var singleNodeConfig = {};
    if (startWithAuth) {
        singleNodeConfig.keyFile = "jstests/libs/key1";
        if (startWithTransitionToAuth) {
            singleNodeConfig.transitionToAuth = "";
        }
    }
    if (clusterType === "sharded") {
        var shardingTestConfig = {
            name: baseName + "_source",
            keyFile: singleNodeConfig.keyFile,
            mongos: [singleNodeConfig],
            shards: [singleNodeConfig],
            config: [singleNodeConfig]
        };
        var shardingTest = new ShardingTest(shardingTestConfig);
        this.conn = shardingTest.s;
        this.connString = this.conn.host;
    } else if (clusterType === "repl") {
        var replSetTestConfig = {
            name: baseName + "_source",
            nodes: 3,
            nodeOptions: singleNodeConfig
        };
        var replSetTest = new ReplSetTest(replSetTestConfig);
        replSetTest.startSet();
        replSetTest.initiate();
        if (startWithAuth) {
            authutil.asCluster(
                replSetTest.nodes, replSetTestConfig.nodeOptions.keyFile, function() {
                    replSetTest.awaitReplication();
                });
        } else {
            replSetTest.awaitReplication();
        }
        this.conn = replSetTest.getPrimary();
        this.connString = replSetTest.getURL();
    } else {
        this.conn = MongoRunner.runMongod(singleNodeConfig);
        this.connString = this.conn.host;
    }

    this.stop = function() {
        if (clusterType === "sharded") {
            shardingTest.stop();
        } else if (clusterType === "repl") {
            replSetTest.stopSet();
        } else {
            MongoRunner.stopMongod(this.conn.port);
        }
    };
}

/*
 * Helper to test the running the "copydb" command between various kinds of clusters and various
 * combinations of authentication on the source and target.
 *
 * @param {Object} configObj
 *
 *   {
 *     sourceClusterType {string}: Type of cluster to use as the source of the copy.  Options are
 *         "single", "repl", "sharded".
 *     isSourceUsingAuth {bool}: Whether to use auth in the source cluster for the copy.
 *     targetClusterType {string}: Type of cluster to use as the target of the copy.  Options are
 *         "single", "repl", "sharded".
 *     isTargetUsingAuth {bool}: Whether to use auth in the target cluster for the copy.
 *   }
 */
function copydbBetweenClustersTest(configObj) {
    // First sanity check the arguments in our configObj
    var requiredKeys = [
        'sourceClusterType',
        'isSourceUsingAuth',
        'targetClusterType',
        'isTargetUsingAuth',
        'isSourceUsingTransitionToAuth',
        'isTargetUsingTransitionToAuth'
    ];

    var i;
    for (i = 0; i < requiredKeys.length; i++) {
        assert(configObj.hasOwnProperty(requiredKeys[i]),
               "Missing required key: " + requiredKeys[i] + " in config object");
    }

    // 1. Get a connection to the source database, insert data and setup auth if applicable
    source = new ClusterSpawnHelper(configObj.sourceClusterType,
                                    configObj.isSourceUsingAuth,
                                    configObj.isSourceUsingTransitionToAuth);

    if (configObj.isSourceUsingAuth) {
        // Create a super user so we can create a regular user and not be locked out afterwards
        source.conn.getDB("admin").createUser(
            {user: "sourceSuperUser", pwd: "sourceSuperUser", roles: ["root"]});
        source.conn.getDB("admin").auth("sourceSuperUser", "sourceSuperUser");

        source.conn.getDB(baseName)[baseName].save({i: 1});
        assert.eq(1, source.conn.getDB(baseName)[baseName].count());
        assert.eq(1, source.conn.getDB(baseName)[baseName].findOne().i);

        // Insert a document and create a regular user that we will use for the target
        // authenticating with the source
        source.conn.getDB(baseName).createUser({user: "foo", pwd: "bar", roles: ["dbOwner"]});

        source.conn.getDB("admin").logout();

        var readWhenLoggedOut = function() {
            source.conn.getDB(baseName)[baseName].findOne();
        };
        if (configObj.isSourceUsingTransitionToAuth) {
            // transitionToAuth does not turn on access control
            assert.doesNotThrow(readWhenLoggedOut);
        } else {
            assert.throws(readWhenLoggedOut);
        }
    } else {
        source.conn.getDB(baseName)[baseName].save({i: 1});
        assert.eq(1, source.conn.getDB(baseName)[baseName].count());
        assert.eq(1, source.conn.getDB(baseName)[baseName].findOne().i);
    }

    // 2. Get a connection to the target database, and set up auth if necessary
    target = new ClusterSpawnHelper(configObj.targetClusterType,
                                    configObj.isTargetUsingAuth,
                                    configObj.isTargetUsingTransitionToAuth);

    if (configObj.isTargetUsingAuth) {
        target.conn.getDB("admin").createUser(
            {user: "targetSuperUser", pwd: "targetSuperUser", roles: ["root"]});

        var readWhenLoggedOut = function() {
            target.conn.getDB(baseName)[baseName].findOne();
        };
        if (configObj.isTargetUsingTransitionToAuth) {
            // transitionToAuth does not turn on access control
            assert.doesNotThrow(readWhenLoggedOut);
        } else {
            assert.throws(readWhenLoggedOut);
        }

        target.conn.getDB("admin").auth("targetSuperUser", "targetSuperUser");
    }

    // 3. Run the copydb command
    target.conn.getDB(baseName).dropDatabase();
    assert.eq(0, target.conn.getDB(baseName)[baseName].count());
    if (configObj.isSourceUsingAuth) {
        // We only need to pass username and password if the target has to send authentication
        // information to the source cluster
        assert.commandWorked(target.conn.getDB(baseName).copyDatabase(
            baseName, baseName, source.connString, "foo", "bar"));
    } else {
        // We are copying from a cluster with no auth
        assert.commandWorked(
            target.conn.getDB(baseName).copyDatabase(baseName, baseName, source.connString));
    }
    assert.eq(1, target.conn.getDB(baseName)[baseName].count());
    assert.eq(1, target.conn.getDB(baseName)[baseName].findOne().i);

    // 4. Do any necessary cleanup
    source.stop();
    target.stop();
}

(function() {
    "use strict";

    var sourceClusterTypeValues = ["single", "repl", "sharded"];
    var isSourceUsingAuthValues = [true, false];
    var isSourceUsingTransitionToAuthValues = [true, false];
    var targetClusterTypeValues = ["single", "repl", "sharded"];
    var isTargetUsingAuthValues = [true, false];
    var isTargetUsingTransitionToAuthValues = [true, false];
    for (var i = 0; i < sourceClusterTypeValues.length; i++) {
        for (var j = 0; j < isSourceUsingAuthValues.length; j++) {
            for (var k = 0; k < targetClusterTypeValues.length; k++) {
                for (var l = 0; l < isTargetUsingAuthValues.length; l++) {
                    if (sourceClusterTypeValues[i] === "sharded" &&
                        targetClusterTypeValues[k] === "sharded") {
                        // SERVER-13112
                        continue;
                    }
                    if (sourceClusterTypeValues[i] === "repl" &&
                        targetClusterTypeValues[k] === "repl") {
                        // SERVER-13077
                        continue;
                    }
                    if (isSourceUsingAuthValues[j] === true &&
                        targetClusterTypeValues[k] === "sharded") {
                        // SERVER-6427
                        continue;
                    }
                    if (sourceClusterTypeValues[i] === "repl" &&
                        isSourceUsingAuthValues[j] === false &&
                        targetClusterTypeValues[k] === "sharded" &&
                        isTargetUsingAuthValues[l] === true) {
                        // SERVER-18103
                        continue;
                    }

                    for (var m = 0; m < isSourceUsingTransitionToAuthValues.length; m++) {
                        if (isSourceUsingTransitionToAuthValues[m] === true &&
                            isSourceUsingAuthValues[j] === false) {
                            // transitionToAuth requires auth parameters
                            continue;
                        }
                        for (var n = 0; n < isTargetUsingTransitionToAuthValues.length; n++) {
                            if (isTargetUsingTransitionToAuthValues[n] === true &&
                                isTargetUsingAuthValues[l] === false) {
                                // transitionToAuth requires auth parameters
                                continue;
                            }
                            var testCase = {
                                'sourceClusterType': sourceClusterTypeValues[i],
                                'isSourceUsingAuth': isSourceUsingAuthValues[j],
                                'targetClusterType': targetClusterTypeValues[k],
                                'isTargetUsingAuth': isTargetUsingAuthValues[l],
                                'isSourceUsingTransitionToAuth':
                                    isSourceUsingTransitionToAuthValues[m],
                                'isTargetUsingTransitionToAuth':
                                    isTargetUsingTransitionToAuthValues[n]
                            };
                            print("Running copydb with auth test:");
                            printjson(testCase);
                            copydbBetweenClustersTest(testCase);
                        }
                    }
                }
            }
        }
    }
}());
print(baseName + " success!");
