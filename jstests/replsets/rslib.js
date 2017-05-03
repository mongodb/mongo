var syncFrom;
var wait;
var occasionally;
var reconnect;
var getLatestOp;
var waitForAllMembers;
var reconfig;
var awaitOpTime;
var startSetIfSupportsReadMajority;
var waitUntilAllNodesCaughtUp;
var waitForState;
var reInitiateWithoutThrowingOnAbortedMember;
var awaitRSClientHosts;
var getLastOpTime;

(function() {
    "use strict";
    load("jstests/libs/write_concern_util.js");

    var count = 0;
    var w = 0;

    /**
     * A wrapper around `replSetSyncFrom` to ensure that the desired sync source is ahead of the
     * syncing node so that the syncing node can choose to sync from the desired sync source.
     * It first stops replication on the syncing node so that it can do a write on the desired
     * sync source and make sure it's ahead. When replication is restarted, the desired sync
     * source will be a valid sync source for the syncing node.
     */
    syncFrom = function(syncingNode, desiredSyncSource, rst) {
        jsTestLog("Forcing " + syncingNode.name + " to sync from " + desiredSyncSource.name);

        // Ensure that 'desiredSyncSource' doesn't already have the dummy write sitting around from
        // a previous syncFrom attempt.
        var dummyName = "dummyForSyncFrom";
        rst.getPrimary().getDB(dummyName).getCollection(dummyName).drop();
        assert.soonNoExcept(function() {
            return desiredSyncSource.getDB(dummyName).getCollection(dummyName).findOne() == null;
        });

        stopServerReplication(syncingNode);

        assert.writeOK(rst.getPrimary().getDB(dummyName).getCollection(dummyName).insert({a: 1}));
        // Wait for 'desiredSyncSource' to get the dummy write we just did so we know it's
        // definitely ahead of 'syncingNode' before we call replSetSyncFrom.
        assert.soonNoExcept(function() {
            return desiredSyncSource.getDB(dummyName).getCollection(dummyName).findOne({a: 1});
        });

        assert.commandWorked(syncingNode.adminCommand({replSetSyncFrom: desiredSyncSource.name}));
        restartServerReplication(syncingNode);
        rst.awaitSyncSource(syncingNode, desiredSyncSource);
    };

    wait = function(f, msg) {
        w++;
        var n = 0;
        while (!f()) {
            if (n % 4 == 0)
                print("waiting " + w);
            if (++n == 4) {
                print("" + f);
            }
            if (n >= 200) {
                throw new Error('tried 200 times, giving up on ' + msg);
            }
            sleep(1000);
        }
    };

    /**
     * Use this to do something once every 4 iterations.
     *
     * <pre>
     * for (i=0; i<1000; i++) {
     *   occasionally(function() { print("4 more iterations"); });
     * }
     * </pre>
     */
    occasionally = function(f, n) {
        var interval = n || 4;
        if (count % interval == 0) {
            f();
        }
        count++;
    };

    reconnect = function(a) {
        wait(function() {
            var db;
            try {
                // make this work with either dbs or connections
                if (typeof(a.getDB) == "function") {
                    db = a.getDB('foo');
                } else {
                    db = a;
                }
                db.bar.stats();
                if (jsTest.options().keyFile) {  // SERVER-4241: Shell connections don't
                                                 // re-authenticate on reconnect
                    return jsTest.authenticate(db.getMongo());
                }
                return true;
            } catch (e) {
                print(e);
                return false;
            }
        });
    };

    getLatestOp = function(server) {
        server.getDB("admin").getMongo().setSlaveOk();
        var log = server.getDB("local")['oplog.rs'];
        var cursor = log.find({}).sort({'$natural': -1}).limit(1);
        if (cursor.hasNext()) {
            return cursor.next();
        }
        return null;
    };

    waitForAllMembers = function(master, timeout) {
        var failCount = 0;

        assert.soon(function() {
            var state = null;
            try {
                state = master.getSisterDB("admin").runCommand({replSetGetStatus: 1});
                failCount = 0;
            } catch (e) {
                // Connection can get reset on replica set failover causing a socket exception
                print("Calling replSetGetStatus failed");
                print(e);
                return false;
            }
            occasionally(function() {
                printjson(state);
            }, 10);

            for (var m in state.members) {
                if (state.members[m].state != 1 &&  // PRIMARY
                    state.members[m].state != 2 &&  // SECONDARY
                    state.members[m].state != 7) {  // ARBITER
                    return false;
                }
            }
            printjson(state);
            return true;
        }, "not all members ready", timeout || 10 * 60 * 1000);

        print("All members are now in state PRIMARY, SECONDARY, or ARBITER");
    };

    reconfig = function(rs, config, force) {
        "use strict";
        var admin = rs.getPrimary().getDB("admin");
        var e;
        var master;
        try {
            assert.commandWorked(admin.runCommand(
                {replSetReconfig: rs._updateConfigIfNotDurable(config), force: force}));
        } catch (e) {
            if (!isNetworkError(e)) {
                throw e;
            }
            print("Calling replSetReconfig failed. " + tojson(e));
        }

        var master = rs.getPrimary().getDB("admin");
        waitForAllMembers(master);

        return master;
    };

    awaitOpTime = function(node, opTime) {
        var ts, ex;
        assert.soon(
            function() {
                try {
                    // The following statement extracts the timestamp field from the most recent
                    // element of
                    // the oplog, and stores it in "ts".
                    ts = node.getDB("local")['oplog.rs']
                             .find({})
                             .sort({'$natural': -1})
                             .limit(1)
                             .next()
                             .ts;
                    if ((ts.t == opTime.t) && (ts.i == opTime.i)) {
                        return true;
                    }
                    ex = null;
                    return false;
                } catch (ex) {
                    return false;
                }
            },
            function() {
                var message = "Node " + node + " only reached optime " + tojson(ts) + " not " +
                    tojson(opTime);
                if (ex) {
                    message += "; last attempt failed with exception " + tojson(ex);
                }
                return message;
            });
    };

    /**
     * Uses the results of running replSetGetStatus against an arbitrary replset node to wait until
     * all nodes in the set are replicated through the same optime.
     * 'rs' is an array of connections to replica set nodes.  This function is useful when you
     * don't have a ReplSetTest object to use, otherwise ReplSetTest.awaitReplication is preferred.
     */
    waitUntilAllNodesCaughtUp = function(rs, timeout) {
        var rsStatus;
        var firstConflictingIndex;
        var ot;
        var otherOt;
        assert.soon(
            function() {
                rsStatus = rs[0].adminCommand('replSetGetStatus');
                if (rsStatus.ok != 1) {
                    return false;
                }
                assert.eq(rs.length, rsStatus.members.length, tojson(rsStatus));
                ot = rsStatus.members[0].optime;
                for (var i = 1; i < rsStatus.members.length; ++i) {
                    var otherNode = rsStatus.members[i];

                    // Must be in PRIMARY or SECONDARY state.
                    if (otherNode.state != ReplSetTest.State.PRIMARY &&
                        otherNode.state != ReplSetTest.State.SECONDARY) {
                        return false;
                    }

                    // Fail if optimes are not equal.
                    otherOt = otherNode.optime;
                    if (!friendlyEqual(otherOt, ot)) {
                        firstConflictingIndex = i;
                        return false;
                    }
                }
                return true;
            },
            function() {
                return "Optimes of members 0 (" + tojson(ot) + ") and " + firstConflictingIndex +
                    " (" + tojson(otherOt) + ") are different in " + tojson(rsStatus);
            },
            timeout);
    };

    /**
     * Waits for the given node to reach the given state, ignoring network errors.
     */
    waitForState = function(node, state) {
        assert.soonNoExcept(function() {
            assert.commandWorked(node.adminCommand(
                {replSetTest: 1, waitForMemberState: state, timeoutMillis: 60 * 1000 * 5}));
            return true;
        });
    };

    /**
     * Starts each node in the given replica set if the storage engine supports readConcern
     *'majority'.
     * Returns true if the replica set was started successfully and false otherwise.
     *
     * @param replSetTest - The instance of {@link ReplSetTest} to start
     * @param options - The options passed to {@link ReplSetTest.startSet}
     */
    startSetIfSupportsReadMajority = function(replSetTest, options) {
        try {
            replSetTest.startSet(options);
        } catch (e) {
            var conn = MongoRunner.runMongod();
            if (!conn.getDB("admin").serverStatus().storageEngine.supportsCommittedReads) {
                MongoRunner.stopMongod(conn);
                return false;
            }
            throw e;
        }
        return true;
    };

    /**
     * Performs a reInitiate() call on 'replSetTest', ignoring errors that are related to an aborted
     * secondary member. All other errors are rethrown.
     */
    reInitiateWithoutThrowingOnAbortedMember = function(replSetTest) {
        try {
            replSetTest.reInitiate();
        } catch (e) {
            // reInitiate can throw because it tries to run an ismaster command on
            // all secondaries, including the new one that may have already aborted
            const errMsg = tojson(e);
            if (isNetworkError(e)) {
                // Ignore these exceptions, which are indicative of an aborted node
            } else {
                throw e;
            }
        }
    };

    /**
     * Waits for the specified hosts to enter a certain state.
     */
    awaitRSClientHosts = function(conn, host, hostOk, rs, timeout) {
        var hostCount = host.length;
        if (hostCount) {
            for (var i = 0; i < hostCount; i++) {
                awaitRSClientHosts(conn, host[i], hostOk, rs);
            }

            return;
        }

        timeout = timeout || 5 * 60 * 1000;

        if (hostOk == undefined)
            hostOk = {ok: true};
        if (host.host)
            host = host.host;
        if (rs)
            rs = rs.name;

        print("Awaiting " + host + " to be " + tojson(hostOk) + " for " + conn + " (rs: " + rs +
              ")");

        var tests = 0;

        assert.soon(function() {
            var rsClientHosts = conn.adminCommand('connPoolStats').replicaSets;
            if (tests++ % 10 == 0) {
                printjson(rsClientHosts);
            }

            for (var rsName in rsClientHosts) {
                if (rs && rs != rsName)
                    continue;

                for (var i = 0; i < rsClientHosts[rsName].hosts.length; i++) {
                    var clientHost = rsClientHosts[rsName].hosts[i];
                    if (clientHost.addr != host)
                        continue;

                    // Check that *all* host properties are set correctly
                    var propOk = true;
                    for (var prop in hostOk) {
                        // Use special comparator for tags because isMaster can return the fields in
                        // different order. The fields of the tags should be treated like a set of
                        // strings and 2 tags should be considered the same if the set is equal.
                        if (prop == 'tags') {
                            if (!clientHost.tags) {
                                propOk = false;
                                break;
                            }

                            for (var hostTag in hostOk.tags) {
                                if (clientHost.tags[hostTag] != hostOk.tags[hostTag]) {
                                    propOk = false;
                                    break;
                                }
                            }

                            for (var clientTag in clientHost.tags) {
                                if (clientHost.tags[clientTag] != hostOk.tags[clientTag]) {
                                    propOk = false;
                                    break;
                                }
                            }

                            continue;
                        }

                        if (isObject(hostOk[prop])) {
                            if (!friendlyEqual(hostOk[prop], clientHost[prop])) {
                                propOk = false;
                                break;
                            }
                        } else if (clientHost[prop] != hostOk[prop]) {
                            propOk = false;
                            break;
                        }
                    }

                    if (propOk) {
                        return true;
                    }
                }
            }

            return false;
        }, 'timed out waiting for replica set client to recognize hosts', timeout);
    };

    /**
     * Returns the last opTime of the connection based from replSetGetStatus. Can only
     * be used on replica set nodes.
     */
    getLastOpTime = function(conn) {
        var replSetStatus =
            assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1}));
        var connStatus = replSetStatus.members.filter(m => m.self)[0];
        return connStatus.optime;
    };
}());
