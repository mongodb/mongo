var syncFrom;
var wait;
var occasionally;
var reconnect;
var getLatestOp;
var waitForAllMembers;
var reconfig;
var safeReconfigShouldFail;
var awaitOpTime;
var waitUntilAllNodesCaughtUp;
var waitForState;
var reInitiateWithoutThrowingOnAbortedMember;
var awaitRSClientHosts;
var getLastOpTime;
var getFirstOplogEntry;
var setLogVerbosity;
var stopReplicationAndEnforceNewPrimaryToCatchUp;
var isConfigCommitted;
var assertSameConfigContent;
var isSameConfigContent;
var getConfigWithNewlyAdded;
var isMemberNewlyAdded;
var waitForNewlyAddedRemovalForNodeToBeCommitted;
var assertVoteCount;
var disconnectSecondaries;
var reconnectSecondaries;
var createRstArgs;
var createRst;
var waitAllNodesHaveConfig;

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

    assert.commandWorked(rst.getPrimary().getDB(dummyName).getCollection(dummyName).insert({a: 1}));
    // Wait for 'desiredSyncSource' to get the dummy write we just did so we know it's
    // definitely ahead of 'syncingNode' before we call replSetSyncFrom.
    assert.soonNoExcept(function() {
        return desiredSyncSource.getDB(dummyName).getCollection(dummyName).findOne({a: 1});
    });

    assert.commandWorked(syncingNode.adminCommand({replSetSyncFrom: desiredSyncSource.name}));
    restartServerReplication(syncingNode);
    rst.awaitSyncSource(syncingNode, desiredSyncSource);
    rst.awaitReplication();
};

/**
 * Calls a function 'f' once a second until it returns true. Throws an exception once 'f' has
 * been called more than 'retries' times without returning true. If 'retries' is not given,
 * it defaults to 200. 'retries' must be an integer greater than or equal to zero.
 */
wait = function(f, msg, retries) {
    w++;
    var n = 0;
    var default_retries = 200;
    var delay_interval_ms = 1000;

    // Set default value if 'retries' was not given.
    if (retries === undefined) {
        retries = default_retries;
    }
    while (!f()) {
        if (n % 4 == 0) {
            print("Waiting " + w);
        }
        if (++n == 4) {
            print("" + f);
        }
        if (n >= retries) {
            throw new Error('Tried ' + retries + ' times, giving up on ' + msg);
        }
        sleep(delay_interval_ms);
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

/**
 * Attempt to re-establish and re-authenticate a Mongo connection if it was dropped, with
 * multiple retries.
 *
 * Returns upon successful re-connnection. If connection cannot be established after 200
 * retries, throws an exception.
 *
 * @param conn - a Mongo connection object or DB object.
 */
reconnect = function(conn) {
    var retries = 200;
    wait(function() {
        var db;
        try {
            // Make this work with either dbs or connections.
            if (typeof (conn.getDB) == "function") {
                db = conn.getDB('foo');
            } else {
                db = conn;
            }

            // Run a simple command to re-establish connection.
            db.bar.stats();

            // SERVER-4241: Shell connections don't re-authenticate on reconnect.
            if (jsTest.options().keyFile) {
                return jsTest.authenticate(db.getMongo());
            }
            return true;
        } catch (e) {
            print(e);
            return false;
        }
    }, retries);
};

getLatestOp = function(server) {
    server.getDB("admin").getMongo().setSecondaryOk();
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
            state = master.getSiblingDB("admin").runCommand({replSetGetStatus: 1});
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

/**
 * Run a 'replSetReconfig' command with one retry on NodeNotFound and multiple retries on
 * ConfigurationInProgress, CurrentConfigNotCommittedYet, and
 * NewReplicaSetConfigurationIncompatible.
 * Expect the reconfig to fail if shouldFail is set to true.
 */
function reconfigWithRetry(primary, config, force, shouldFail = false, errCode, errMsg) {
    const admin = primary.getDB("admin");
    force = force || false;
    let reconfigCommand = {
        replSetReconfig: config,
        force: force,
        maxTimeMS: ReplSetTest.kDefaultTimeoutMS
    };

    assert.soon(function() {
        const newVersion =
            assert.commandWorked(admin.runCommand({replSetGetConfig: 1})).config.version + 1;
        reconfigCommand.replSetReconfig.version = newVersion;
        let res = admin.runCommand(reconfigCommand);

        // Retry reconfig if quorum check failed because not enough voting nodes responded. One
        // reason for this is if the connections used for heartbeats get closed on the destination
        // node.
        if (!res.ok && res.code === ErrorCodes.NodeNotFound) {
            print("Replset reconfig failed because quorum check failed. Retry reconfig once. " +
                  "Error: " + tojson(res));
            res = admin.runCommand(reconfigCommand);
        }

        // Always retry on these errors, even if we already retried on NodeNotFound.
        if (!res.ok &&
            (res.code === ErrorCodes.ConfigurationInProgress ||
             res.code === ErrorCodes.CurrentConfigNotCommittedYet)) {
            print("Replset reconfig failed since another configuration is in progress. Retry.");
            // Retry.
            return false;
        }

        // Always retry on NewReplicaSetConfigurationIncompatible, if the current config version is
        // higher than the requested one.
        if (!res.ok && res.code === ErrorCodes.NewReplicaSetConfigurationIncompatible) {
            const curVersion =
                assert.commandWorked(admin.runCommand({replSetGetConfig: 1})).config.version;
            if (curVersion >= newVersion) {
                print("Replset reconfig failed since the config version was too low. Retry. " +
                      "Error: " + tojson(res));
                // Retry.
                return false;
            }
        }

        if (!shouldFail) {
            assert.commandWorked(res);
        } else {
            assert.commandFailed(res);
            if (errCode) {
                assert.eq(res.code, errCode);
            }

            if (errMsg) {
                assert(res.errmsg.includes(errMsg));
            }
        }

        return true;
    });
}

/**
 * Executes an arbitrary reconfig as a sequence of non 'force' reconfigs.
 *
 * If this function fails for any reason, the replica set config may be left in an intermediate
 * state i.e. neither in the original or target config.
 *
 * @param rst - a ReplSetTest instance.
 * @param targetConfig - the final, desired replica set config. After this function returns, the
 * given replica set should be in 'targetConfig', except with a higher version.
 */
function autoReconfig(rst, targetConfig) {
    //
    // The goal of this function is to transform the source config (the current config on the
    // primary) into the 'targetConfig' via a sequence of non 'force' reconfigurations. Non force
    // reconfigs are only permitted to add or remove a single voting node, so we need to represent
    // some given, arbitrary reconfig as a sequence of single node add/remove operations. We execute
    // the overall transformation in the following steps:
    //
    // (1) Remove members present in the source but not in the target.
    // (2) Update members present in both the source and target whose vote is removed.
    // (3) Update members present in both the source and target whose vote is added or unmodified.
    // (4) Add members present in the target but not in the source.
    //
    // After executing the above steps the config member set should be equal to the target config
    // member set. We then execute one last reconfig that attempts to install the given
    // targetConfig directly. This serves to update any top level properties of the config and it
    // also ensures that the order of the final config member list matches the order in the given
    // target config.
    //
    // Note that the order of the steps above is important to avoid passing through invalid configs
    // during the config transformation sequence. There are certain constraints imposed on replica
    // set configs e.g. there must be at least 1 electable node and less than a certain number of
    // maximum voting nodes. We know that the source and target configs are valid with respect to
    // these constraints, but we must ensure that any sequence of reconfigs executed by this
    // function never moves us to an intermediate config that violates one of these constraints.
    // Since the primary, an electable node, can never be removed from the config, it is safe to do
    // the removal of all voting nodes first, since we will be guaranteed to never go below the
    // minimum number of electable nodes. Doing removals first similarly ensures that when adding
    // nodes, we will never exceed an upper bound constraint, since we have already removed all
    // necessary voting nodes.
    //
    // Note also that this procedure may not perform the desired config transformation in the
    // minimal number of steps. For example, if the overall transformation removes 2 non-voting
    // nodes from a config we could do this with a single reconfig, but the procedure implemented
    // here will do it as a sequence of 2 reconfigs. We are not so worried about making this
    // procedure optimal since each reconfig should be relatively quick and most reconfigs shouldn't
    // take more than a few steps.
    //

    let primary = rst.getPrimary();
    const sourceConfig = rst.getReplSetConfigFromNode();
    let config = Object.assign({}, sourceConfig);

    // Look up the index of a given member in the given array by its member id.
    const memberIndex = (cfg, id) => cfg.members.findIndex(m => m._id === id);
    const memberInConfig = (cfg, id) => cfg.members.find(m => m._id === id);
    const getMember = (cfg, id) => cfg.members[memberIndex(cfg, id)];
    const getVotes = (cfg, id) =>
        getMember(cfg, id).hasOwnProperty("votes") ? getMember(cfg, id).votes : 1;

    print(`autoReconfig: source config: ${tojson(sourceConfig)}, target config: ${
        tojson(targetConfig)}`);

    // All the members in the target that aren't in the source.
    let membersToAdd = targetConfig.members.filter(m => !memberInConfig(sourceConfig, m._id));
    // All the members in the source that aren't in the target.
    let membersToRemove = sourceConfig.members.filter(m => !memberInConfig(targetConfig, m._id));
    // All the members that appear in both the source and target and have changed.
    let membersToUpdate = targetConfig.members.filter(
        (m) => memberInConfig(sourceConfig, m._id) &&
            bsonWoCompare(m, memberInConfig(sourceConfig, m._id)) !== 0);

    // Sort the members to ensure that we do updates that remove a node's vote first.
    let membersToUpdateRemoveVote = membersToUpdate.filter(
        (m) => (getVotes(targetConfig, m._id) < getVotes(sourceConfig, m._id)));
    let membersToUpdateAddVote = membersToUpdate.filter(
        (m) => (getVotes(targetConfig, m._id) >= getVotes(sourceConfig, m._id)));
    membersToUpdate = membersToUpdateRemoveVote.concat(membersToUpdateAddVote);

    print(`autoReconfig: Starting with membersToRemove: ${
        tojsononeline(membersToRemove)}, membersToUpdate: ${
        tojsononeline(membersToUpdate)}, membersToAdd: ${tojsononeline(membersToAdd)}`);

    // Remove members.
    membersToRemove.forEach(toRemove => {
        config.members = config.members.filter(m => m._id !== toRemove._id);
        config.version++;
        print(`autoReconfig: remove member id ${toRemove._id}, reconfiguring to member set: ${
            tojsononeline(config.members)}`);
        reconfigWithRetry(primary, config);
    });

    // Update members.
    membersToUpdate.forEach(toUpdate => {
        let configIndex = memberIndex(config, toUpdate._id);
        config.members[configIndex] = toUpdate;
        config.version++;
        print(`autoReconfig: update member id ${toUpdate._id}, reconfiguring to member set: ${
            tojsononeline(config.members)}`);
        reconfigWithRetry(primary, config);
    });

    // Add members.
    membersToAdd.forEach(toAdd => {
        config.members.push(toAdd);
        config.version++;
        print(`autoReconfig: add member id ${toAdd._id}, reconfiguring to member set: ${
            tojsononeline(config.members)}`);
        reconfigWithRetry(primary, config);
    });

    // Verify that the final set of members is correct.
    assert.sameMembers(targetConfig.members.map(m => m._id),
                       rst.getReplSetConfigFromNode().members.map(m => m._id),
                       "final config does not have the expected member set.");

    // Do a final reconfig to update any other top level config fields. This also ensures the
    // correct member order in the final config since the add/remove procedure above will result in
    // a members array that has the correct set of members but the members may not be in the same
    // order as the specified target config.
    print("autoReconfig: doing final reconfig to reach target config.");
    targetConfig.version = rst.getReplSetConfigFromNode().version + 1;
    reconfigWithRetry(primary, targetConfig);
}

/**
 * Executes a replica set reconfiguration on the given ReplSetTest instance.
 *
 * If this function fails for any reason while doing a non force reconfig, the replica set config
 * may be left in an intermediate state i.e. neither in the original or target config.
 *
 * @param rst - a ReplSetTest instance.
 * @param config - the desired target config. After this function returns, the
 * given replica set should be in 'config', except with a higher version.
 * @param force - should this be a 'force' reconfig or not.
 * @param doNotWaitForMembers - if set, we will skip waiting for all members to be in primary,
 *     secondary, or arbiter states
 */
reconfig = function(rst, config, force, doNotWaitForMembers) {
    "use strict";
    var primary = rst.getPrimary();
    config = rst._updateConfigIfNotDurable(config);

    // If this is a non 'force' reconfig, execute the reconfig as a series of reconfigs. Safe
    // reconfigs only allow addition/removal of a single voting node at a time, so arbitrary
    // reconfigs must be carried out in multiple steps. Using safe reconfigs guarantees that we
    // don't violate correctness properties of the replication protocol.
    if (!force) {
        autoReconfig(rst, config);
    } else {
        // Force reconfigs can always be executed in one step.
        reconfigWithRetry(primary, config, force);
    }

    var primaryAdminDB = rst.getPrimary().getDB("admin");
    if (!doNotWaitForMembers) {
        waitForAllMembers(primaryAdminDB);
    }
    return primaryAdminDB;
};

/**
 * Tests that a replica set safe reconfiguration on the given ReplSetTest instance should fail.
 *
 * @param rst - a ReplSetTest instance.
 * @param config - the desired target config.
 * @param force - should this be a 'force' reconfig or not.
 * @param errCode - if exists, we verify that the reconfig fails with this errCode.
 * @param errMsg - if exists, we verify that the reconfig fails with error message containing this
 * errMsg.
 */
safeReconfigShouldFail = function(rst, config, force, errCode, errMsg) {
    reconfigWithRetry(rst.getPrimary(), config, force, true /* shouldFail */, errCode, errMsg);
};

awaitOpTime = function(catchingUpNode, latestOpTimeNode) {
    var ts, ex, opTime;
    assert.soon(
        function() {
            try {
                // The following statement extracts the timestamp field from the most recent
                // element of
                // the oplog, and stores it in "ts".
                ts = getLatestOp(catchingUpNode).ts;
                opTime = getLatestOp(latestOpTimeNode).ts;
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
            var message = "Node " + catchingUpNode + " only reached optime " + tojson(ts) +
                " not " + tojson(opTime);
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
            return "Optimes of members 0 (" + tojson(ot) + ") and " + firstConflictingIndex + " (" +
                tojson(otherOt) + ") are different in " + tojson(rsStatus);
        },
        timeout);
};

/**
 * Waits for the given node to reach the given state, ignoring network errors.  Ensures that the
 * connection is re-connected and usable when the function returns.
 */
waitForState = function(node, state) {
    assert.soonNoExcept(function() {
        assert.commandWorked(node.adminCommand(
            {replSetTest: 1, waitForMemberState: state, timeoutMillis: 60 * 1000 * 5}));
        return true;
    });
    // Some state transitions cause connections to be closed, but whether the connection close
    // happens before or after the replSetTest command above returns is racy, so to ensure that
    // the connection to 'node' is usable after this function returns, reconnect it first.
    reconnect(node);
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

    print("Awaiting " + host + " to be " + tojson(hostOk) + " for " + conn + " (rs: " + rs + ")");

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
    var replSetStatus = assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1}));
    var connStatus = replSetStatus.members.filter(m => m.self)[0];
    return connStatus.optime;
};

/**
 * Returns the oldest oplog entry.
 */
getFirstOplogEntry = function(server, opts = {}) {
    server.getDB("admin").getMongo().setSecondaryOk();

    let firstEntryQuery = server.getDB('local').oplog.rs.find().sort({$natural: 1}).limit(1);
    if (opts.readConcern) {
        firstEntryQuery = firstEntryQuery.readConcern(opts.readConcern);
    }

    // The query plan may yield between the cursor establishment and iterating to retrieve the first
    // result. During this yield it's possible for the oplog to "roll over" or shrink. This is rare,
    // but if these both happen the cursor will be unable to resume after yielding and return a
    // "CappedPositionLost" error. This can be safely retried.
    let firstEntry;
    assert.soon(() => {
        try {
            firstEntry = firstEntryQuery.toArray()[0];
            return true;
        } catch (e) {
            if (e.code == ErrorCodes.CappedPositionLost) {
                return false;
            }
            throw e;
        }
    });
    return firstEntry;
};

/**
 * Set log verbosity on all given nodes.
 * e.g. setLogVerbosity(replTest.nodes, { "replication": {"verbosity": 3} });
 */
setLogVerbosity = function(nodes, logVerbosity) {
    var verbosity = {
        "setParameter": 1,
        "logComponentVerbosity": logVerbosity,
    };
    nodes.forEach(function(node) {
        assert.commandWorked(node.adminCommand(verbosity));
    });
};

/**
 * Stop replication on secondaries, do writes and step up the node that was passed in.
 *
 * The old primary has extra writes that are not replicated to the other nodes yet,
 * but the new primary steps up, getting the vote from the the third node "voter".
 */
stopReplicationAndEnforceNewPrimaryToCatchUp = function(rst, node) {
    // Write documents that cannot be replicated to secondaries in time.
    const oldSecondaries = rst.getSecondaries();
    const oldPrimary = rst.getPrimary();

    // In the case that the old primary has just stepped up and is running internal writes from
    // PrimaryOnlyService, wait for those to be replicated. This is because there could be a race
    // between stopping one of the secondaries, while the other secondary is still able to replicate
    // the internal writes from PrimaryOnlyService before stopping replication.
    rst.awaitReplication();
    stopServerReplication(oldSecondaries);
    for (let i = 0; i < 3; i++) {
        assert.commandWorked(oldPrimary.getDB("test").foo.insert({x: i}));
    }

    const latestOpOnOldPrimary = getLatestOp(oldPrimary);

    // New primary wins immediately, but needs to catch up.
    const newPrimary =
        rst.stepUp(node, {awaitReplicationBeforeStepUp: false, awaitWritablePrimary: false});
    const latestOpOnNewPrimary = getLatestOp(newPrimary);
    // Check this node is not writable.
    assert.eq(newPrimary.getDB("test").isMaster().ismaster, false);

    return {
        oldSecondaries: oldSecondaries,
        oldPrimary: oldPrimary,
        newPrimary: newPrimary,
        voter: oldSecondaries[1],
        latestOpOnOldPrimary: latestOpOnOldPrimary,
        latestOpOnNewPrimary: latestOpOnNewPrimary
    };
};

/**
 * Returns the replSetGetConfig field 'commitmentStatus', which is true or false.
 */
isConfigCommitted = function(node) {
    let adminDB = node.getDB('admin');
    return assert.commandWorked(adminDB.runCommand({replSetGetConfig: 1, commitmentStatus: true}))
        .commitmentStatus;
};

/**
 * Asserts that replica set config A is the same as replica set config B ignoring the 'version' and
 * 'term' field.
 */
assertSameConfigContent = function(configA, configB) {
    assert(isSameConfigContent(configA, configB));
};

/**
 * Returns true if replica set config A is the same as replica set config B ignoring the 'version'
 * and 'term' field.
 */
isSameConfigContent = function(configA, configB) {
    // Save original versions and terms.
    const [versionA, termA] = [configA.version, configA.term];
    const [versionB, termB] = [configB.version, configB.term];

    configA.version = configA.term = 0;
    configB.version = configB.term = 0;
    const isEqual = tojson(configA) === tojson(configB);

    // Reset values so we don't modify the original objects.
    configA.version = versionA;
    configA.term = termA;
    configB.version = versionB;
    configB.term = termB;
    return isEqual;
};

/**
 * Returns the result of 'replSetGetConfig' with the test-parameter specified that indicates to
 * include 'newlyAdded' fields.
 */
getConfigWithNewlyAdded = function(node) {
    return assert.commandWorked(
        node.adminCommand({replSetGetConfig: 1, $_internalIncludeNewlyAdded: true}));
};

/**
 * @param memberIndex is optional. If not provided, then it will return true even if
 * a single member in the replSet config has "newlyAdded" field.
 */
isMemberNewlyAdded = function(node, memberIndex) {
    const config = getConfigWithNewlyAdded(node).config;

    const allMembers = (memberIndex === undefined);
    assert(allMembers || (memberIndex >= 0 && memberIndex < config.members.length),
           "memberIndex should be between 0 and " + (config.members.length - 1) +
               ", but memberIndex is " + memberIndex);

    var hasNewlyAdded = (index) => {
        const memberConfig = config.members[index];
        if (memberConfig.hasOwnProperty("newlyAdded")) {
            assert(memberConfig["newlyAdded"] === true, config);
            return true;
        }
        return false;
    };

    if (allMembers) {
        for (let i = 0; i < config.members.length; i++) {
            if (hasNewlyAdded(i))
                return true;
        }
        return false;
    }

    return hasNewlyAdded(memberIndex);
};

waitForNewlyAddedRemovalForNodeToBeCommitted = function(node, memberIndex, force = false) {
    jsTestLog("Waiting for member " + memberIndex + " to no longer be 'newlyAdded'");
    assert.soonNoExcept(function() {
        return !isMemberNewlyAdded(node, memberIndex, force) && isConfigCommitted(node);
    }, getConfigWithNewlyAdded(node).config);
};

assertVoteCount = function(node, {
    votingMembersCount,
    majorityVoteCount,
    writableVotingMembersCount,
    writeMajorityCount,
    totalMembersCount
}) {
    const status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
    assert.eq(status["votingMembersCount"], votingMembersCount, status);
    assert.eq(status["majorityVoteCount"], majorityVoteCount, status);
    assert.eq(status["writableVotingMembersCount"], writableVotingMembersCount, status);
    assert.eq(status["writeMajorityCount"], writeMajorityCount, status);
    assert.eq(status["members"].length, totalMembersCount, status);
};

disconnectSecondaries = function(rst, secondariesDown) {
    for (let i = 1; i <= secondariesDown; i++) {
        for (const node of rst.nodes) {
            if (node !== rst.nodes[i]) {
                node.disconnect(rst.nodes[i]);
            }
        }
    }
};

reconnectSecondaries = function(rst) {
    for (const node of rst.nodes) {
        for (const node2 of rst.nodes) {
            if (node2 !== node) {
                node2.reconnect(node);
            }
        }
    }
};

createRstArgs = function(rst) {
    const rstArgs = {
        name: rst.name,
        nodeHosts: rst.nodes.map(node => `127.0.0.1:${node.port}`),
        nodeOptions: rst.nodeOptions,
        keyFile: rst.keyFile,
        host: rst.host,
        waitForKeys: false,
    };
    return rstArgs;
};

/**
 * Returns a new ReplSetTest created based on the given 'rstArgs'. If 'retryOnRetryableErrors'
 * is true, retries on retryable errors (e.g. errors caused by shutdown).
 */
createRst = function(rstArgs, retryOnRetryableErrors) {
    const kCreateRstRetryIntervalMS = 100;

    while (true) {
        try {
            return new ReplSetTest({rstArgs: rstArgs});
        } catch (e) {
            if (retryOnRetryableErrors && isNetworkError(e)) {
                jsTest.log(`Failed to create ReplSetTest for ${
                    rstArgs.name} inside tenant migration thread: ${tojson(e)}. Retrying in ${
                    kCreateRstRetryIntervalMS}ms.`);
                sleep(kCreateRstRetryIntervalMS);
                continue;
            }
            throw e;
        }
    }
};

/**
 * Wait until all the nodes in a replica set have the same config as the input config.
 */
waitAllNodesHaveConfig = function(replSet, config) {
    replSet.nodes.forEach(function(node) {
        assert.soon(function() {
            const nodeConfig = replSet.getReplSetConfigFromNode(node.nodeId);
            return isSameConfigContent(config, nodeConfig);
        });
    });
};
}());
