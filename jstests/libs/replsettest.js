import {Thread} from "jstests/libs/parallelTester.js";

/* global retryOnRetryableError */

// Symbol used to override the constructor. Please do not use this, it's only meant to aid
// in migrating the jstest corpus to proper module usage.
export const kOverrideConstructor = Symbol("overrideConstructor");

// Replica set health states
const Health = {
    UP: 1,
    DOWN: 0,
};
const kOplogName = "oplog.rs";

export class ReplSetTest {
    /**
     * Sets up a replica set. To make the set running, call {@link #startSet}, followed by {@link
     * #initiate} (and optionally, {@link #awaitSecondaryNodes} to block till the  set is fully
     * operational). Note that some of the replica start up parameters are not passed here, but to
     * the #startSet method.
     *
     * @param {Object} [opts]
     * @param {string} [opts.name='testReplSet'] Name of this replica set.
     * @param {string} [opts.host] Name of the host machine. Hostname will be used if not specified.
     * @param {boolean} [opts.useHostName] If true, use hostname of machine, otherwise use
     *     localhost.
     * @param {number|Object|Object[]} [opts.nodes=0] Number of replicas.
     *     Can also be an Object (or Array).
     *     Format for Object:
     *         {
     *            <any string>: replica member option Object. see MongoRunner.runMongod
     *            <any string2>: and so on...
     *          }
     *          If object has a special "rsConfig" field then those options will be used for each
     *          replica set member config options when used to initialize the replica set, or
     *          building the config with getReplSetConfig()
     *
     *     Format for Array:
     *         An array of replica member option Object. see MongoRunner.runMongod
     *
     *     Note: For both formats, a special boolean property 'arbiter' can be specified to denote
     *     a member is an arbiter.
     *
     *     Note: A special "bridgeOptions" property can be specified in both the object and array
     *     formats to configure the options for the mongobridge corresponding to that node. These
     *     options are merged with the opts.bridgeOptions options, where the node-specific options
     *     take precedence.
     * @param {Object} [opts.nodeOptions] Command-line options to apply to all nodes in the replica
     *     set. Format for Object: { cmdline-param-with-no-arg : "", param-with-arg : arg } This
     *     turns into "mongod --cmdline-param-with-no-arg --param-with-arg arg"
     * @param {boolean} [opts.causallyConsistent=false] Specifies whether the connections to the
     *     replica set nodes should be created with the 'causal consistency' flag enabled, which
     *     means they will gossip the cluster time and add readConcern afterClusterTime where
     *     applicable.
     * @param {number} [opts.oplogSize=40]
     * @param {boolean} [opts.useSeedList=false] Use the connection string format of this set as the
     *     replica set name (overrides the name property).
     * @param {string} [opts.keyFile]
     * @param {number} [opts.protocolVersion] Protocol version of replset used by the replset
     *     initiation.
     * @param {boolean} [opts.useBridge=false] If true, then a mongobridge process is started for
     *     each node in the replica set. Both the replica set configuration and the connections
     *     returned by startSet() will be references to the proxied connections.
     * @param {Object} [opts.bridgeOptions={}] Options to apply to all mongobridge processes.
     * @param {boolean} [opts.seedRandomNumberGenerator] Indicates whether the random number
     *     generator should be seeded when randomBinVersions is true. For ReplSetTests started by
     *     ShardingTest, the seed is generated as part of ShardingTest.
     * @param {boolean} [opts.useAutoBootstrapProcedure] If true, follow the procedure for
     *     auto-bootstrapped replica sets.
     * @param {number} [opts.timeoutMS] Timeout value in milliseconds.
     */
    constructor(opts) {
        if (this.constructor === ReplSetTest && this.constructor[kOverrideConstructor]) {
            return new this.constructor[kOverrideConstructor][kOverrideConstructor](opts);
        }

        // If opts.timeoutMS is present use that for the ReplSetTest instance, otherwise use global
        // value.
        this.timeoutMS = opts.timeoutMS || ReplSetTest.kDefaultTimeoutMS;

        // If opts is passed in as a string, let it pass unmodified since strings are pass-by-value.
        // if it is an object, though, pass in a deep copy.
        if (typeof opts === "string" || opts instanceof String) {
            retryOnRetryableError(
                () => {
                    // The primary may unexpectedly step down during startup if under heavy load
                    // and too slowly processing heartbeats. When it steps down, it closes all of
                    // its connections.
                    _constructFromExistingSeedNode(this, opts);
                },
                ReplSetTest.kDefaultRetries,
                1000,
                [ErrorCodes.NotYetInitialized],
            );
        } else if (typeof opts.rstArgs === "object") {
            _constructFromExistingNodes(this, Object.extend({}, opts.rstArgs, true));
        } else {
            _constructStartNewInstances(this, Object.extend({}, opts, true));
        }
    }

    asCluster(conn, fn, keyFileParam = undefined) {
        return asCluster(this, conn, fn, keyFileParam);
    }

    /**
     * Wait for a rs indicator to go to a particular state or states.
     *
     * @private
     * @param node is a single node, by id or conn
     * @param states is a single state or list of states
     * @param ind is the indicator specified
     * @param timeout how long to wait for the state to be reached
     * @param reconnectNode indicates that we should reconnect to a node that stepped down
     */
    _waitForIndicator(node, ind, states, timeout, reconnectNode) {
        node = resolveToConnection(this, node);
        timeout = timeout || this.timeoutMS;
        if (reconnectNode === undefined) {
            reconnectNode = true;
        }

        if (!states.length) {
            states = [states];
        }

        jsTest.log.info("ReplSetTest waitForIndicator " + ind + " on " + node);
        jsTest.log.info({states});
        jsTest.log.info("ReplSetTest waitForIndicator from node " + node);

        let lastTime = null;
        let currTime = new Date().getTime();
        let status;

        let foundState;
        assert.soon(
            () => {
                try {
                    let conn = _callHello(this);
                    if (!conn) {
                        conn = this._liveNodes[0];
                    }

                    // Try again to load connection
                    if (!conn) return false;

                    if (reconnectNode instanceof Function) {
                        // Allow caller to perform tasks on reconnect.
                        reconnectNode(conn);
                    }
                    asCluster(this, conn, function () {
                        status = conn.getDB("admin").runCommand({replSetGetStatus: 1});
                    });
                } catch (ex) {
                    jsTest.log.info("ReplSetTest waitForIndicator could not get status", {error: ex});
                    return false;
                }

                if (status.code == ErrorCodes.Unauthorized) {
                    // If we're not authorized already, then we never will be.
                    assert.commandWorked(status); // throws
                }

                let printStatus = false;
                if (lastTime == null || (currTime = new Date().getTime()) - 1000 * 5 > lastTime) {
                    if (lastTime == null) {
                        jsTest.log.info("ReplSetTest waitForIndicator Initial status (timeout : " + timeout + ") :");
                    }

                    jsTest.log.info({status});
                    lastTime = new Date().getTime();
                    printStatus = true;
                }

                if (typeof status.members == "undefined") {
                    return false;
                }

                for (let i = 0; i < status.members.length; i++) {
                    if (printStatus) {
                        jsTest.log.info(
                            "Status for : " + status.members[i].name + ", checking " + node.host + "/" + node.name,
                        );
                    }

                    if (status.members[i].name == node.host || status.members[i].name == node.name) {
                        for (let j = 0; j < states.length; j++) {
                            if (printStatus) {
                                jsTest.log.info(
                                    "Status -- " +
                                        " current state: " +
                                        status.members[i][ind] +
                                        ",  target state : " +
                                        states[j],
                                );
                            }

                            if (typeof states[j] != "number") {
                                throw new Error(
                                    "State was not an number -- type:" + typeof states[j] + ", value:" + states[j],
                                );
                            }
                            if (status.members[i][ind] == states[j]) {
                                foundState = states[j];
                                return true;
                            }
                        }
                    }
                }

                return false;
            },
            "waiting for state indicator " + ind + " for " + timeout + "ms",
            timeout,
        );

        // If we were waiting for the node to step down, wait until we can connect to it again,
        // since primaries close external connections upon stepdown. This ensures that the
        // connection to the node is usable after the function returns.
        if (reconnectNode && foundState === ReplSetTest.State.SECONDARY) {
            assert.soon(function () {
                try {
                    node.getDB("foo").bar.stats();
                    return true;
                } catch (e) {
                    return false;
                }
            }, "timed out waiting to reconnect to node " + node.name);
        }

        jsTest.log.info("ReplSetTest waitForIndicator final status:");
        jsTest.log.info({status});
    }

    /**
     * Returns the {readConcern: majority} OpTime for the host.
     * This is the OpTime of the host's "majority committed" snapshot.
     * This function may return an OpTime with Timestamp(0,0) and Term(0) if read concern majority
     * is not enabled, or if there has not been a committed snapshot yet.
     */
    getReadConcernMajorityOpTime(conn) {
        const replSetStatus = asCluster(this, conn, () =>
            assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1})),
        );

        return (
            (replSetStatus.OpTimes || replSetStatus.optimes).readConcernMajorityOpTime || {
                ts: Timestamp(0, 0),
                t: NumberLong(0),
            }
        );
    }

    /**
     * Returns the {readConcern: majority} OpTime for the host. Throws if not available.
     */
    getReadConcernMajorityOpTimeOrThrow(conn) {
        const majorityOpTime = this.getReadConcernMajorityOpTime(conn);
        if (friendlyEqual(majorityOpTime, {ts: Timestamp(0, 0), t: NumberLong(0)})) {
            throw new Error("readConcern majority optime not available");
        }
        return majorityOpTime;
    }

    /**
     * Returns list of nodes as host:port strings.
     */
    nodeList() {
        let list = [];
        for (let i = 0; i < this.ports.length; i++) {
            list.push(this.host + ":" + this.ports[i]);
        }

        return list;
    }

    getNodeId(node) {
        if (node.toFixed) {
            return parseInt(node);
        }

        for (var i = 0; i < this.nodes.length; i++) {
            if (this.nodes[i] == node) {
                return i;
            }
        }

        if (node instanceof ObjectId) {
            for (i = 0; i < this.nodes.length; i++) {
                if (this.nodes[i].runId == node) {
                    return i;
                }
            }
        }

        if (node.nodeId != null) {
            return parseInt(node.nodeId);
        }

        return undefined;
    }

    getPort(n) {
        var n = this.getNodeId(n);
        return this.ports[n];
    }

    getDbPath(node) {
        // Get a replica set node (check for use of bridge).
        const n = this.getNodeId(node);
        const replNode = this._useBridge ? this._unbridgedNodes[n] : this.nodes[n];
        return replNode.dbpath;
    }

    /** @private */
    _addPath(p) {
        if (!this._alldbpaths) this._alldbpaths = [p];
        else this._alldbpaths.push(p);

        return p;
    }

    getReplSetConfig() {
        let cfg = {};
        cfg._id = this.name;
        cfg.protocolVersion = 1;

        cfg.members = [];

        for (let i = 0; i < this.ports.length; i++) {
            let member = {};
            member._id = i;

            member.host = this.host;
            if (!member.host.includes("/")) {
                member.host += ":" + this.ports[i];
            }

            let nodeOpts = this.nodeOptions["n" + i];
            if (nodeOpts) {
                if (nodeOpts.arbiter) {
                    member.arbiterOnly = true;
                }

                if (nodeOpts.rsConfig) {
                    Object.extend(member, nodeOpts.rsConfig);
                }
            }

            cfg.members.push(member);
        }

        if (this._configSettings) {
            cfg.settings = this._configSettings;
        }

        return cfg;
    }

    getURL() {
        let hosts = [];

        // If the replica set uses mongobridge, use the hostname specified for the replica set.
        // If the hostname specified for the replica set or nodes is like 'primary', 'secondary0',
        // 'secondary1' (not 'localhost' and not an ip address (like 127.0.0.1 or
        // ip-10-122-7-63)), then this replica set is started by antithesis. In this
        // case, use the node's host for url so that the hostnames on the logs would be
        // different for each node. Otherwise, use the hostname specified for the replica set.
        for (let i = 0; i < this.ports.length; i++) {
            if (!this._useBridge && this.host !== "localhost" && !this.host.includes("-") && !this.host.includes(".")) {
                hosts.push(this.nodes[i].host);
            } else {
                hosts.push(this.host + ":" + this.ports[i]);
            }
        }

        return this.name + "/" + hosts.join(",");
    }

    /**
     * Starts each node in the replica set with the given options.
     *
     * @param options - The options passed to {@link MongoRunner.runMongod}
     * @param restart - Boolean indicating whether we are restarting the set (if true,
     *     then `forRestart` should have been passed as true to `stopSet()`.) Defaults to false.
     * @param skipStepUpOnRestart - Boolean indicating that this method should skip attempting to
     *     step up a new primary after restarting the set. Defaults to false. This must be set to
     *     true when using the in-memory storage engine, as the replica set must be re-initiated
     *     by the test on restart before a node can be elected.
     *     This option has no effect if `restart` is not also passed as true.
     */
    startSet(options, restart, skipStepUpOnRestart) {
        // If the caller has explicitly specified 'waitForConnect:false', then we will start up all
        // replica set nodes and return without waiting to connect to any of them.
        const skipWaitingForAllConnections = options && options.waitForConnect === false;

        // Keep a copy of these options
        this.startSetOptions = options;

        // Start up without waiting for connections.
        this.startSetAsync(options, restart);

        // Avoid waiting for connections to each node.
        if (skipWaitingForAllConnections) {
            jsTest.log.info(
                "ReplSetTest startSet skipping waiting for connections to all nodes in set '" + this.name + "'",
            );
            return this.nodes;
        }

        this.startSetAwait();

        // If the set is being restarted, by default we will try to find a node to step up
        // proactively rather than waiting for the election timeout.
        const triggerStepUp = (restart || (options && options.restart)) && !skipStepUpOnRestart;
        if (!triggerStepUp) {
            jsTest.log.info("ReplSetTest startSet skipping stepping a new primary");
            return this.nodes;
        }

        jsTest.log.info("ReplSetTest startSet attempting to step up a new primary");

        // Try to step up each node and stop after the first success.
        // We use asCluster as replSetStepUp requires auth.
        return asCluster(this, this.nodes, () => {
            for (const node of this.nodes) {
                if (_isElectable(node)) {
                    this.stepUp(node, {awaitReplicationBeforeStepUp: false});
                    return this.nodes;
                }
            }
            throw Error("Restarted set but failed to get a node to step up, as none were electable");
        });
    }

    /**
     * Starts each node in the replica set with the given options without waiting for a connection
     * to any node. Call 'startSetAwait' subsequently to wait for startup of each node to complete.
     *
     * @param options - The options passed to {@link MongoRunner.runMongod}
     */
    startSetAsync(options, restart) {
        jsTest.log.info("ReplSetTest starting set '" + this.name + "'");
        this.startSetStartTime = new Date(); // Measure the execution time of node startup.

        if (options && options.keyFile) {
            this.keyFile = options.keyFile;
        }

        if (options) {
            this.startOptions = options;
        }

        if (jsTest.options().useRandomBinVersionsWithinReplicaSet && this.seedRandomNumberGenerator) {
            // Set the random seed to the value passed in by TestData. The seed is undefined
            // by default. For sharded clusters, the seed is already initialized as part of
            // ShardingTest.
            Random.setRandomFixtureSeed();
        }

        // If the caller has explicitly set 'waitForConnect', then we prefer that. Otherwise we
        // default to not waiting for a connection. We merge the options object with a new field so
        // as to not modify the original options object that was passed in.
        options = options || {};
        options = options.waitForConnect === undefined ? Object.merge(options, {waitForConnect: false}) : options;

        // Start up each node without waiting to connect. This allows startup of replica set nodes
        // to proceed in parallel.
        for (let n = 0; n < this.ports.length; n++) {
            if (n == 0 && this.useAutoBootstrapProcedure && !this._hasAcquiredAutoGeneratedName) {
                // Must wait for connect in order to extract the auto-generated replica set name.
                options.waitForConnect = true;
            }

            this.start(n, options, restart, false);
        }
        return this.nodes;
    }

    /**
     * Waits for startup of each replica set node to complete by waiting until a connection can be
     * made to each.
     */
    startSetAwait() {
        // Wait until we can establish a connection to each node before proceeding.
        for (let n = 0; n < this.ports.length; n++) {
            this._waitForInitialConnection(n);
        }

        jsTest.log.info("ReplSetTest startSet", {nodes: this.nodes});

        jsTest.log.info(
            "ReplSetTest startSet took " +
                (new Date() - this.startSetStartTime) +
                "ms for " +
                this.nodes.length +
                " nodes.",
        );
        return this.nodes;
    }

    /**
     * Blocks until the secondary nodes have completed recovery and their roles are known. Blocks on
     * all secondary nodes or just 'secondaries', if specified. Does not wait for all 'newlyAdded'
     * fields to be removed by default.
     */
    awaitSecondaryNodes(timeout, secondaries, retryIntervalMS, waitForNewlyAddedRemoval) {
        timeout = timeout || this.timeoutMS;
        retryIntervalMS = retryIntervalMS || 200;
        let awaitingSecondaries;
        jsTest.log.info("AwaitSecondaryNodes: Waiting for the secondary nodes has started.");
        try {
            assert.soonNoExcept(
                () => {
                    awaitingSecondaries = [];
                    // Reload who the current secondaries are
                    _callHello(this);

                    let secondariesToCheck = secondaries || this._secondaries;
                    let len = secondariesToCheck.length;
                    for (let i = 0; i < len; i++) {
                        let hello = secondariesToCheck[i].getDB("admin")._helloOrLegacyHello();
                        let arbiter = hello.arbiterOnly === undefined ? false : hello.arbiterOnly;
                        if (!hello.secondary && !arbiter) {
                            awaitingSecondaries.push(secondariesToCheck[i]);
                        }
                    }

                    return awaitingSecondaries.length == 0;
                },
                "Awaiting secondaries: awaitingSecondariesPlaceholder",
                timeout,
                retryIntervalMS,
            );
        } catch (e) {
            e.message = e.message.replace(
                "awaitingSecondariesPlaceholder",
                tojson(awaitingSecondaries.map((n) => n.name)),
            );
            throw e;
        }

        // We can only wait for newlyAdded field removal if test commands are enabled.
        if (waitForNewlyAddedRemoval && jsTest.options().enableTestCommands) {
            this.waitForAllNewlyAddedRemovals();
        }
        jsTest.log.info("AwaitSecondaryNodes: Completed successfully.");
    }

    /**
     * A special version of awaitSecondaryNodes() used exclusively by rollback_test.js.
     * Wraps around awaitSecondaryNodes() itself and checks for an unrecoverable rollback
     * if it throws.
     */
    awaitSecondaryNodesForRollbackTest(timeout, secondaries, connToCheckForUnrecoverableRollback, retryIntervalMS) {
        retryIntervalMS = retryIntervalMS || 200;
        try {
            MongoRunner.runHangAnalyzer.disable();
            this.awaitSecondaryNodes(timeout, secondaries, retryIntervalMS);
            MongoRunner.runHangAnalyzer.enable();
        } catch (originalEx) {
            // Re-throw the original exception in all cases.
            MongoRunner.runHangAnalyzer.enable();
            throw originalEx;
        }
    }

    /**
     * Blocks until the specified node says it's syncing from the given upstream node.
     */
    awaitSyncSource(node, upstreamNode, timeout) {
        jsTest.log.info("Waiting for node " + node.name + " to start syncing from " + upstreamNode.name);
        let status = null;
        assert(this !== undefined);
        assert.soonNoExcept(
            function () {
                status = asCluster(this, node, () => assert.commandWorked(node.adminCommand({replSetGetStatus: 1})));

                for (let j = 0; j < status.members.length; j++) {
                    if (status.members[j].self) {
                        return status.members[j].syncSourceHost === upstreamNode.host;
                    }
                }
                return false;
            },
            "Awaiting node " + node + " syncing from " + upstreamNode + ": " + tojson(status),
            timeout,
        );
    }

    /**
     * Blocks until each node agrees that all other nodes have applied the most recent oplog entry.
     */
    awaitNodesAgreeOnAppliedOpTime(timeout, nodes) {
        timeout = timeout || this.timeoutMS;
        nodes = nodes || this.nodes;

        assert.soon(
            function () {
                let appliedOpTimeConsensus = undefined;
                for (let i = 0; i < nodes.length; i++) {
                    let replSetGetStatus;
                    try {
                        replSetGetStatus = nodes[i].adminCommand({replSetGetStatus: 1});
                    } catch (e) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnAppliedOpTime: Retrying because node " +
                                nodes[i].name +
                                " failed to execute replSetGetStatus",
                            {error: e},
                        );
                        return false;
                    }
                    assert.commandWorked(replSetGetStatus);

                    if (appliedOpTimeConsensus === undefined) {
                        if (replSetGetStatus.optimes) {
                            appliedOpTimeConsensus = replSetGetStatus.optimes.appliedOpTime;
                        } else {
                            // Older versions of mongod do not include an 'optimes' field in the
                            // replSetGetStatus response. We instead pull an optime from the first
                            // replica set member that includes one in its status. All we need here is
                            // any initial value that we can compare to all the other optimes.
                            let optimeMembers = replSetGetStatus.members.filter((m) => m.optime);
                            assert(
                                optimeMembers.length > 0,
                                "AwaitNodesAgreeOnAppliedOpTime: replSetGetStatus did not " +
                                    "include optimes for any members: " +
                                    tojson(replSetGetStatus),
                            );
                            appliedOpTimeConsensus = optimeMembers[0].optime;
                        }

                        assert(
                            appliedOpTimeConsensus,
                            "AwaitNodesAgreeOnAppliedOpTime: missing appliedOpTime in " +
                                "replSetGetStatus: " +
                                tojson(replSetGetStatus),
                        );
                    }

                    if (
                        replSetGetStatus.optimes &&
                        !friendlyEqual(replSetGetStatus.optimes.appliedOpTime, appliedOpTimeConsensus)
                    ) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnAppliedOpTime: Retrying because node " +
                                nodes[i].name +
                                " has appliedOpTime that does not match previously observed appliedOpTime",
                            {appliedOpTime: appliedOpTimeConsensus},
                        );
                        return false;
                    }

                    for (let j = 0; j < replSetGetStatus.members.length; j++) {
                        if (replSetGetStatus.members[j].state == ReplSetTest.State.ARBITER) {
                            // ARBITER nodes do not apply oplog entries and do not have an 'optime'
                            // field.
                            continue;
                        }

                        if (!friendlyEqual(replSetGetStatus.members[j].optime, appliedOpTimeConsensus)) {
                            jsTest.log.info(
                                "AwaitNodesAgreeOnAppliedOpTime: Retrying because node " +
                                    nodes[i].name +
                                    " sees optime not expected on node " +
                                    replSetGetStatus.members[j].name,
                                {
                                    actualOpTime: replSetGetStatus.members[j].optime,
                                    expectedOpTime: appliedOpTimeConsensus,
                                },
                            );
                            return false;
                        }
                    }
                }

                jsTest.log.info(
                    "AwaitNodesAgreeOnAppliedOpTime: All nodes agree that all ops are applied across replica set",
                    {appliedOpTimeConsensus},
                );
                return true;
            },
            "Awaiting nodes to agree that all ops are applied across replica set",
            timeout,
        );
    }

    /** @private */
    _findHighestPriorityNodes(config) {
        let highestPriority = 0;
        let highPriorityNodes = [];
        for (let i = 0; i < config.members.length; i++) {
            const member = config.members[i];
            if (member.priority > highestPriority) {
                highestPriority = member.priority;
                highPriorityNodes = [this.nodes[i]];
            } else if (member.priority === highestPriority) {
                highPriorityNodes.push(this.nodes[i]);
            }
        }
        return highPriorityNodes;
    }

    /**
     * Blocks until the node with the highest priority is the primary.  If there are multiple
     * nodes tied for highest priority, waits until one of them is the primary.
     */
    awaitHighestPriorityNodeIsPrimary(timeout) {
        timeout = timeout || this.timeoutMS;

        // First figure out the set of highest priority nodes.
        const config = asCluster(this, this.nodes, () => this.getReplSetConfigFromNode());
        const highPriorityNodes = this._findHighestPriorityNodes(config);

        // Now wait for the primary to be one of the highest priority nodes.
        assert.soon(
            () => {
                return highPriorityNodes.includes(this.getPrimary());
            },
            () => {
                return (
                    "Expected primary to be one of: " +
                    tojson(highPriorityNodes) +
                    ", but found primary to be: " +
                    tojson(this.getPrimary())
                );
            },
            timeout,
        );

        // Finally wait for all nodes to agree on the primary.
        this.awaitNodesAgreeOnPrimary(timeout);
        const primary = this.getPrimary();
        assert(
            highPriorityNodes.includes(primary),
            "Primary switched away from highest priority node.  Found primary: " +
                tojson(primary) +
                ", but expected one of: " +
                tojson(highPriorityNodes),
        );
    }

    /**
     * Blocks until all nodes agree on who the primary is.
     * Unlike awaitNodesAgreeOnPrimary, this does not require that all nodes are authenticated.
     */
    awaitNodesAgreeOnPrimaryNoAuth(timeout, nodes) {
        timeout = timeout || this.timeoutMS;
        nodes = nodes || this.nodes;

        jsTest.log.info("AwaitNodesAgreeOnPrimaryNoAuth: Waiting for nodes to agree on any primary.");

        assert.soonNoExcept(
            function () {
                let primary;

                for (let i = 0; i < nodes.length; i++) {
                    let hello = assert.commandWorked(nodes[i].getDB("admin")._helloOrLegacyHello());
                    let nodesPrimary = hello.primary;
                    // Node doesn't see a primary.
                    if (!nodesPrimary) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnPrimaryNoAuth: Retrying because " +
                                nodes[i].name +
                                " does not see a primary.",
                        );
                        return false;
                    }

                    if (!primary) {
                        // If we haven't seen a primary yet, set it to this.
                        primary = nodesPrimary;
                    } else if (primary !== nodesPrimary) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnPrimaryNoAuth: Retrying because " +
                                nodes[i].name +
                                " thinks the primary is " +
                                nodesPrimary +
                                " instead of " +
                                primary,
                        );
                        return false;
                    }
                }

                jsTest.log.info("AwaitNodesAgreeOnPrimaryNoAuth: Nodes agreed on primary " + primary);
                return true;
            },
            "Awaiting nodes to agree on primary",
            timeout,
        );
    }

    /**
     * Blocks until all nodes agree on who the primary is.
     * If 'expectedPrimaryNode' is provided, ensure that every node is seeing this node as the
     * primary. Otherwise, ensure that all the nodes in the set agree with the first node on the
     * identity of the primary.
     * This call does not guarantee that the agreed upon primary is writeable.
     */
    awaitNodesAgreeOnPrimary(timeout, nodes, expectedPrimaryNode, runHangAnalyzerOnTimeout = true) {
        timeout = timeout || this.timeoutMS;
        nodes = nodes || this.nodes;
        // indexOf will return the index of the expected node. If expectedPrimaryNode is undefined,
        // indexOf will return -1.
        const expectedPrimaryNodeIdx = this.nodes.indexOf(expectedPrimaryNode);
        if (expectedPrimaryNodeIdx === -1) {
            jsTest.log.info("AwaitNodesAgreeOnPrimary: Waiting for nodes to agree on any primary.");
        } else {
            jsTest.log.info(
                "AwaitNodesAgreeOnPrimary: Waiting for nodes to agree on " + expectedPrimaryNode.name + " as primary.",
            );
        }

        assert.soonNoExcept(
            () => {
                let primary = expectedPrimaryNodeIdx;

                for (let i = 0; i < nodes.length; i++) {
                    let node = nodes[i];
                    let replSetGetStatus = asCluster(this, node, () =>
                        assert.commandWorked(node.adminCommand({replSetGetStatus: 1})),
                    );
                    let nodesPrimary = -1;
                    for (let j = 0; j < replSetGetStatus.members.length; j++) {
                        if (replSetGetStatus.members[j].state === ReplSetTest.State.PRIMARY) {
                            // Node sees two primaries.
                            if (nodesPrimary !== -1) {
                                jsTest.log.info(
                                    "AwaitNodesAgreeOnPrimary: Retrying because " +
                                        nodes[i].name +
                                        " thinks both " +
                                        this.nodes[nodesPrimary].name +
                                        " and " +
                                        this.nodes[j].name +
                                        " are primary.",
                                );

                                return false;
                            }
                            nodesPrimary = j;
                        }
                    }
                    // Node doesn't see a primary.
                    if (nodesPrimary < 0) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnPrimary: Retrying because " + node.name + " does not see a primary.",
                        );
                        return false;
                    }

                    if (primary < 0) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnPrimary: " +
                                node.name +
                                " thinks the " +
                                " primary is " +
                                this.nodes[nodesPrimary].name +
                                ". Other nodes are expected to agree on the same primary.",
                        );
                        // If the nodes haven't seen a primary yet, set primary to nodes[i]'s primary.
                        primary = nodesPrimary;
                    } else if (primary !== nodesPrimary) {
                        jsTest.log.info(
                            "AwaitNodesAgreeOnPrimary: Retrying because " +
                                node.name +
                                " thinks the primary is " +
                                this.nodes[nodesPrimary].name +
                                " instead of " +
                                this.nodes[primary].name,
                        );
                        return false;
                    }
                }

                jsTest.log.info("AwaitNodesAgreeOnPrimary: Nodes agreed on primary " + this.nodes[primary].name);
                return true;
            },
            "Awaiting nodes to agree on primary timed out",
            timeout,
            undefined /*interval*/,
            {
                runHangAnalyzer: runHangAnalyzerOnTimeout,
            },
        );
    }

    /**
     * Blocking call, which will wait for a primary to be elected and become writable for some
     * pre-defined timeout. If a primary is available it will return a connection to it.
     * Otherwise throws an exception.
     */
    getPrimary(timeout, retryIntervalMS) {
        timeout = timeout || this.timeoutMS;
        retryIntervalMS = retryIntervalMS || 200;
        let primary = null;

        assert.soonNoExcept(
            () => {
                primary = _callHello(this);
                return primary;
            },
            "Finding primary",
            timeout,
            retryIntervalMS,
        );

        return primary;
    }

    /**
     * Blocks until all nodes agree on who the primary is and the primary is writeable.
     * This includes waiting for the optional primary catchup process to complete, which
     * getPrimary() ensures.
     * Returns the primary.
     */
    awaitNodesAgreeOnWriteablePrimary(
        expectedPrimaryNode,
        waitPrimaryWriteTimeout,
        retryIntervalMS,
        waitNodesAgreeTimeout,
        nodes,
        runHangAnalyzerOnTimeout = true,
    ) {
        this.awaitNodesAgreeOnPrimary(waitNodesAgreeTimeout, nodes, expectedPrimaryNode, runHangAnalyzerOnTimeout);
        return this.getPrimary(waitPrimaryWriteTimeout, retryIntervalMS);
    }

    awaitNoPrimary(msg, timeout) {
        msg = msg || "Timed out waiting for there to be no primary in replset: " + this.name;
        timeout = timeout || this.timeoutMS;

        assert.soonNoExcept(
            () => {
                return _callHello(this) == false;
            },
            msg,
            timeout,
        );
    }

    getSecondaries(timeout) {
        let primary = this.getPrimary(timeout);
        let secs = [];
        for (let i = 0; i < this.nodes.length; i++) {
            if (this.nodes[i] != primary) {
                secs.push(this.nodes[i]);
            }
        }

        return secs;
    }

    getSecondary(timeout) {
        return this.getSecondaries(timeout)[0];
    }

    getArbiters() {
        let arbiters = [];
        for (let i = 0; i < this.nodes.length; i++) {
            const node = this.nodes[i];

            let isArbiter = false;

            assert.retryNoExcept(
                () => {
                    isArbiter = isNodeArbiter(node);
                    return true;
                },
                `Could not call hello/isMaster on ${node}.`,
                3,
                1000,
            );

            if (isArbiter) {
                arbiters.push(node);
            }
        }
        return arbiters;
    }

    getArbiter() {
        return this.getArbiters()[0];
    }

    status(timeout) {
        let primary = _callHello(this);
        if (!primary) {
            primary = this._liveNodes[0];
        }
        return asCluster(this, primary, () => assert.commandWorked(primary.adminCommand({replSetGetStatus: 1})));
    }

    /**
     * Adds a node to the replica set managed by this instance.
     */
    add(config) {
        let nextPort = this._allocatePortForNode();
        jsTest.log.info("ReplSetTest Next port: " + nextPort);

        this.ports.push(nextPort);
        jsTest.log.info({ports: this.ports});

        if (this._useBridge) {
            this._unbridgedPorts.push(this._allocatePortForBridge());
        }

        if (jsTestOptions().shellGRPC) {
            const nextPort = this._allocatePortForNode();
            jsTest.log.info("ReplSetTest Next gRPC port: " + nextPort);

            this.grpcPorts.push(nextPort);
            jsTest.log.info({grpcPorts: this.grpcPorts});
        }

        let nextId = this.nodes.length;
        jsTest.log.info({nodes: this.nodes});

        jsTest.log.info("ReplSetTest nextId: " + nextId);
        return this.start(nextId, config);
    }

    /**
     * Calls stop() on the node identifed by nodeId and removes it from the list of nodes managed by
     * ReplSetTest.
     */
    remove(nodeId) {
        this.stop(nodeId);
        nodeId = this.getNodeId(nodeId);
        this.nodes.splice(nodeId, 1);
        this.ports.splice(nodeId, 1);

        if (this._useBridge) {
            this._unbridgedPorts.splice(nodeId, 1);
            this._unbridgedNodes.splice(nodeId, 1);
        }
    }

    /**
     * If journaling is disabled or we are using an ephemeral storage engine, set
     * 'writeConcernMajorityJournalDefault' to false for the given 'config' object. If the
     * 'writeConcernMajorityJournalDefault' field is already set, it does not override it,
     * and returns the 'config' object unchanged. Does not affect 'config' when running CSRS.
     *
     * @private
     */
    _updateConfigIfNotDurable(config) {
        // Get a replica set node (check for use of bridge).
        let replNode = this._useBridge ? this._unbridgedNodes[0] : this.nodes[0];

        // Don't update replset config for sharding config servers since config servers always
        // require durable storage.
        if (replNode.hasOwnProperty("fullOptions") && replNode.fullOptions.hasOwnProperty("configsvr")) {
            return config;
        }

        // Don't override existing value.
        let wcMajorityJournalField = "writeConcernMajorityJournalDefault";
        if (config.hasOwnProperty(wcMajorityJournalField)) {
            return config;
        }

        // Check journaling by sending commands through the bridge if it's used.
        if (_isRunningWithoutJournaling(this, this.nodes[0])) {
            config[wcMajorityJournalField] = false;
        }

        return config;
    }

    /** @private */
    _setDefaultConfigOptions(config) {
        // Update config for non journaling test variants
        this._updateConfigIfNotDurable(config);
        // Add protocolVersion if missing
        if (!config.hasOwnProperty("protocolVersion")) {
            config["protocolVersion"] = 1;
        }
    }

    /** @private */
    _notX509Auth(conn) {
        const nodeId = "n" + this.getNodeId(conn);
        const nodeOptions = this.nodeOptions[nodeId] || {};
        const options = Object.keys(nodeOptions).length !== 0 || !this.startOptions ? nodeOptions : this.startOptions;
        const authMode = options.clusterAuthMode;
        return authMode != "sendX509" && authMode != "x509" && authMode != "sendKeyFile";
    }

    /**
     * Wait until the config on the primary becomes replicated. Callers specify the primary in case
     * this must be called when two nodes are expected to be concurrently primary. This does not
     * necessarily wait for the config to be committed.
     */
    waitForConfigReplication(primary, nodes) {
        const nodeHosts = nodes ? tojson(nodes.map((n) => n.host)) : "all nodes";
        jsTest.log.info(
            "waitForConfigReplication: Waiting for the config on " + primary.host + " to replicate to " + nodeHosts,
        );

        let rst = this;
        let configVersion = -2;
        let configTerm = -2;
        asCluster(this, primary, () => {
            assert.soon(function () {
                const res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
                const primaryMember = res.members.find((m) => m.self);
                configVersion = primaryMember.configVersion;
                configTerm = primaryMember.configTerm;
                function hasSameConfig(member) {
                    return (
                        member.configVersion === primaryMember.configVersion &&
                        member.configTerm === primaryMember.configTerm
                    );
                }
                let members = res.members;
                if (nodes) {
                    members = res.members.filter((m) => nodes.some((node) => m.name === node.name));
                }
                return members.every((m) => hasSameConfig(m));
            });
        });

        jsTest.log.info(
            "waitForConfigReplication: config on " +
                primary.host +
                " replicated successfully to " +
                nodeHosts +
                " with version " +
                configVersion +
                " and term " +
                configTerm,
        );
    }

    /**
     * Waits for all 'newlyAdded' fields to be removed, for that config to be committed, and for
     * the in-memory and on-disk configs to match.
     */
    waitForAllNewlyAddedRemovals(timeout) {
        timeout = timeout || this.timeoutMS;
        jsTest.log.info("waitForAllNewlyAddedRemovals: starting for set " + this.name);
        const primary = this.getPrimary();

        // Shadow 'db' so that we can call the function on the primary without a separate shell when
        // x509 auth is not needed.
        let db = primary.getDB("admin");
        runFnWithAuthOnPrimary(
            this,
            function () {
                assert.soon(function () {
                    const getConfigRes = assert.commandWorkedOrFailedWithCode(
                        db.adminCommand({
                            replSetGetConfig: 1,
                            commitmentStatus: true,
                            $_internalIncludeNewlyAdded: true,
                        }),
                        ErrorCodes.NotWritablePrimary,
                    );

                    if (!getConfigRes.ok) {
                        jsTest.log.info(
                            "waitForAllNewlyAddedRemovals: Retrying because the old primary " + " stepped down",
                        );
                        return false;
                    }

                    const config = getConfigRes.config;
                    for (let i = 0; i < config.members.length; i++) {
                        const memberConfig = config.members[i];
                        if (memberConfig.hasOwnProperty("newlyAdded")) {
                            assert(memberConfig["newlyAdded"] === true, config);
                            jsTest.log.info(
                                "waitForAllNewlyAddedRemovals: Retrying because memberIndex " +
                                    i +
                                    " is still 'newlyAdded'",
                            );
                            return false;
                        }
                    }
                    if (!getConfigRes.hasOwnProperty("commitmentStatus")) {
                        jsTest.log.info(
                            "waitForAllNewlyAddedRemovals: Skipping wait due to no commitmentStatus." +
                                " Assuming this is an older version.",
                        );
                        return true;
                    }

                    if (!getConfigRes.commitmentStatus) {
                        jsTest.log.info(
                            "waitForAllNewlyAddedRemovals: " +
                                "Retrying because primary's config isn't committed. " +
                                "Version: " +
                                config.version +
                                ", Term: " +
                                config.term,
                        );
                        return false;
                    }

                    return true;
                });
            },
            "waitForAllNewlyAddedRemovals",
        );

        this.waitForConfigReplication(primary);

        jsTest.log.info("waitForAllNewlyAddedRemovals: finished for set " + this.name);
    }

    /**
     * Runs replSetInitiate on the first node of the replica set.
     * Ensures that a primary is elected (not necessarily node 0).
     * initiate() should be preferred instead of this, but this is useful when the connections
     * aren't authorized to run replSetGetStatus.
     */
    _initiateWithAnyNodeAsPrimary(
        cfg,
        initCmd,
        {
            doNotWaitForStableRecoveryTimestamp: doNotWaitForStableRecoveryTimestamp = false,
            doNotWaitForReplication: doNotWaitForReplication = false,
            doNotWaitForNewlyAddedRemovals: doNotWaitForNewlyAddedRemovals = false,
            doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices = false,
        } = {},
    ) {
        let startTime = new Date(); // Measure the execution time of this function.
        let primary = this.nodes[0].getDB("admin");
        let config = cfg || this.getReplSetConfig();
        let cmd = {};
        let cmdKey = initCmd || "replSetInitiate";

        // Throw an exception if nodes[0] is unelectable in the given config.
        if (!_isElectable(config.members[0])) {
            throw Error("The node at index 0 must be electable");
        }

        // Start up a single node replica set then reconfigure to the correct size (if the config
        // contains more than 1 node), so the primary is elected more quickly.
        let originalMembers, originalSettings;
        if (config.members && config.members.length > 1) {
            originalMembers = config.members.slice();
            config.members = config.members.slice(0, 1);
            originalSettings = config.settings;
            delete config.settings; // Clear settings to avoid tags referencing sliced nodes.
        }
        this._setDefaultConfigOptions(config);

        cmd[cmdKey] = config;

        // If this ReplSet is started using this.startSet and binVersions (ie:
        // rst.startSet({binVersion: [...]}) we need to make sure the binVersion combination is
        // valid.
        if (
            typeof this.startSetOptions === "object" &&
            this.startSetOptions.hasOwnProperty("binVersion") &&
            typeof this.startSetOptions.binVersion === "object"
        ) {
            let lastLTSSpecified = false;
            let lastContinuousSpecified = false;
            this.startSetOptions.binVersion.forEach(function (binVersion, _) {
                if (lastLTSSpecified === false) {
                    lastLTSSpecified = MongoRunner.areBinVersionsTheSame(binVersion, lastLTSFCV);
                }
                if (lastContinuousSpecified === false && lastLTSFCV !== lastContinuousFCV) {
                    lastContinuousSpecified = MongoRunner.areBinVersionsTheSame(binVersion, lastContinuousFCV);
                }
            });
            if (lastLTSSpecified && lastContinuousSpecified) {
                throw new Error(
                    "Can only specify one of 'last-lts' and 'last-continuous' " + "in binVersion, not both.",
                );
            }
        }
        // Initiating a replica set with a single node will use "latest" FCV. This will
        // cause IncompatibleServerVersion errors if additional "last-lts"/"last-continuous" binary
        // version nodes are subsequently added to the set, since such nodes cannot set their FCV to
        // "latest". Therefore, we make sure the primary is "last-lts"/"last-continuous" FCV before
        // adding in nodes of different binary versions to the replica set.
        let lastLTSBinVersionWasSpecifiedForSomeNode = false;
        let lastContinuousBinVersionWasSpecifiedForSomeNode = false;
        let explicitBinVersionWasSpecifiedForSomeNode = false;
        Object.keys(this.nodeOptions).forEach((key) => {
            let val = this.nodeOptions[key];
            if (typeof val === "object" && val.hasOwnProperty("binVersion")) {
                if (lastLTSBinVersionWasSpecifiedForSomeNode === false) {
                    lastLTSBinVersionWasSpecifiedForSomeNode = MongoRunner.areBinVersionsTheSame(
                        val.binVersion,
                        lastLTSFCV,
                    );
                }
                if (lastContinuousBinVersionWasSpecifiedForSomeNode === false && lastLTSFCV !== lastContinuousFCV) {
                    lastContinuousBinVersionWasSpecifiedForSomeNode = MongoRunner.areBinVersionsTheSame(
                        val.binVersion,
                        lastContinuousFCV,
                    );
                }
                explicitBinVersionWasSpecifiedForSomeNode = true;
            }
        });

        if (lastLTSBinVersionWasSpecifiedForSomeNode && lastContinuousBinVersionWasSpecifiedForSomeNode) {
            throw new Error("Can only specify one of 'last-lts' and 'last-continuous' " + "in binVersion, not both.");
        }

        // If no binVersions have been explicitly set, then we should be using the latest binary
        // version, which allows us to use the failpoint below.
        let explicitBinVersion =
            (this.startOptions !== undefined && this.startOptions.hasOwnProperty("binVersion")) ||
            explicitBinVersionWasSpecifiedForSomeNode ||
            jsTest.options().useRandomBinVersionsWithinReplicaSet;

        // If a test has explicitly disabled test commands or if we may be running an older mongod
        // version then we cannot utilize failpoints below, since they may not be supported on older
        // versions.
        const failPointsSupported = jsTest.options().enableTestCommands && !explicitBinVersion;

        // Skip waiting for new data to appear in the oplog buffer when transitioning to primary.
        // This makes step up much faster for a node that doesn't need to drain any oplog
        // operations. This is only an optimization so it's OK if we bypass it in some suites.
        if (failPointsSupported) {
            setFailPoint(this.nodes[0], "skipOplogBatcherWaitForData");
        }

        // replSetInitiate and replSetReconfig commands can fail with a NodeNotFound error if a
        // heartbeat times out during the quorum check. They may also fail with
        // NewReplicaSetConfigurationIncompatible on similar timeout during the config validation
        // stage while deducing isSelf(). This can fail with an InterruptedDueToReplStateChange
        // error when interrupted. We try several times, to reduce the chance of failing this way.
        const initiateStart = new Date(); // Measure the execution time of this section.

        if (this.useAutoBootstrapProcedure) {
            // Auto-bootstrap already initiates automatically on the first node, but if the
            // requested initiate is not empty, we need to apply the requested settings using
            // reconfig.
            if (cmd[cmdKey] != {}) {
                cmd["replSetReconfig"] = cmd[cmdKey];
                delete cmd[cmdKey];

                // We must increase the version of the new config for the reconfig
                // to succeed. The initial default config will always have a version of 1.
                cmd["replSetReconfig"].version = 2;
                replSetCommandWithRetry(primary, cmd);
            }
        } else {
            replSetCommandWithRetry(primary, cmd);
        }

        // Blocks until there is a primary. We use a faster retry interval here since we expect the
        // primary to be ready very soon. We also turn the failpoint off once we have a primary.
        this.getPrimary(this.timeoutMS, 25 /* retryIntervalMS */);
        if (failPointsSupported) {
            clearFailPoint(this.nodes[0], "skipOplogBatcherWaitForData");
        }

        jsTest.log.info(
            "ReplSetTest initiate command took " +
                (new Date() - initiateStart) +
                "ms for " +
                this.nodes.length +
                " nodes in set '" +
                this.name +
                "'",
        );

        // Set the FCV to 'last-lts'/'last-continuous' if we are running a mixed version replica
        // set. If this is a config server, the FCV will be set as part of ShardingTest.
        // versions are supported with the useRandomBinVersionsWithinReplicaSet option.
        let setLastLTSFCV =
            (lastLTSBinVersionWasSpecifiedForSomeNode ||
                jsTest.options().useRandomBinVersionsWithinReplicaSet == "last-lts") &&
            !this.isConfigServer;
        let setLastContinuousFCV =
            !setLastLTSFCV &&
            (lastContinuousBinVersionWasSpecifiedForSomeNode ||
                jsTest.options().useRandomBinVersionsWithinReplicaSet == "last-continuous") &&
            !this.isConfigServer;

        if (setLastLTSFCV || setLastContinuousFCV) {
            // Authenticate before running the command.
            asCluster(this, this.nodes, () => {
                let fcv = setLastLTSFCV ? lastLTSFCV : lastContinuousFCV;

                jsTest.log.info("Setting feature compatibility version for replica set to '" + fcv + "'");
                // When latest is not equal to last-continuous, the transition to last-continuous is
                // not allowed. Setting fromConfigServer allows us to bypass this restriction and
                // test last-continuous.
                assert.commandWorked(
                    this.getPrimary().adminCommand({
                        setFeatureCompatibilityVersion: fcv,
                        fromConfigServer: true,
                        confirm: true,
                    }),
                );
                checkFCV(this.getPrimary().getDB("admin"), fcv);

                // The server has a practice of adding a reconfig as part of upgrade/downgrade logic
                // in the setFeatureCompatibilityVersion command.
                jsTest.log.info(
                    "Fetch the config version from primary since last-lts or last-continuous downgrade might " +
                        "perform a reconfig.",
                );
                config.version = this.getReplSetConfigFromNode().version;
            });
        }

        // Wait for 2 keys to appear before adding the other nodes. This is to prevent replica
        // set configurations from interfering with the primary to generate the keys. One example
        // of problematic configuration are delayed secondaries, which impedes the primary from
        // generating the second key due to timeout waiting for write concern.
        let shouldWaitForKeys = true;
        if (this.waitForKeys != undefined) {
            shouldWaitForKeys = this.waitForKeys;
            jsTest.log.info("Set shouldWaitForKeys from RS options: " + shouldWaitForKeys);
        } else {
            Object.keys(this.nodeOptions).forEach((key) => {
                let val = this.nodeOptions[key];
                if (
                    typeof val === "object" &&
                    (val.hasOwnProperty("shardsvr") ||
                        (val.hasOwnProperty("binVersion") &&
                            // Should not wait for keys if version is less than 3.6
                            MongoRunner.compareBinVersions(val.binVersion, "3.6") == -1))
                ) {
                    shouldWaitForKeys = false;
                    jsTest.log.info("Set shouldWaitForKeys from node options: " + shouldWaitForKeys);
                }
            });
            if (this.startOptions != undefined) {
                let val = this.startOptions;
                if (
                    typeof val === "object" &&
                    (val.hasOwnProperty("shardsvr") ||
                        (val.hasOwnProperty("binVersion") &&
                            // Should not wait for keys if version is less than 3.6
                            MongoRunner.compareBinVersions(val.binVersion, "3.6") == -1))
                ) {
                    shouldWaitForKeys = false;
                    jsTest.log.info("Set shouldWaitForKeys from start options: " + shouldWaitForKeys);
                }
            }
        }
        /**
         * Blocks until the primary node generates cluster time sign keys.
         */
        if (shouldWaitForKeys) {
            asCluster(this, this.nodes, (timeout) => {
                jsTest.log.info("Waiting for keys to sign $clusterTime to be generated");
                assert.soonNoExcept(
                    (timeout) => {
                        let keyCnt = this.getPrimary(timeout)
                            .getCollection("admin.system.keys")
                            .find({purpose: "HMAC"})
                            .itcount();
                        return keyCnt >= 2;
                    },
                    "Awaiting keys",
                    timeout,
                );
            });
        }

        // Allow nodes to find sync sources more quickly. We also turn down the heartbeat interval
        // to speed up the initiation process. We use a failpoint so that we can easily turn this
        // behavior on/off without doing a reconfig. This is only an optimization so it's OK if we
        // bypass it in some suites.
        if (failPointsSupported) {
            this.nodes.forEach(function (conn) {
                setFailPoint(conn, "forceSyncSourceRetryWaitForInitialSync", {retryMS: 25});
                setFailPoint(conn, "forceHeartbeatIntervalMS", {intervalMS: 200});
                setFailPoint(conn, "forceBgSyncSyncSourceRetryWaitMS", {sleepMS: 25});
            });
        }

        // Reconfigure the set to contain the correct number of nodes (if necessary).
        const reconfigStart = new Date(); // Measure duration of reconfig and awaitSecondaryNodes.
        if (originalMembers) {
            config.members = originalMembers;
            if (originalSettings) {
                config.settings = originalSettings;
            }
            config.version = config.version ? config.version + 1 : 2;

            // Nodes started with the --configsvr flag must have configsvr = true in their config.
            if (this.nodes[0].hasOwnProperty("fullOptions") && this.nodes[0].fullOptions.hasOwnProperty("configsvr")) {
                config.configsvr = true;
            }

            // Add in nodes 1 at a time since non-force reconfig allows only single node
            // addition/removal.
            jsTest.log.info("Reconfiguring replica set to add in other nodes");
            for (let i = 2; i <= originalMembers.length; i++) {
                jsTest.log.info("ReplSetTest adding in node " + i);
                assert.soon(
                    () => {
                        primary = this.getPrimary();
                        const statusRes = asCluster(this, primary, () =>
                            assert.commandWorked(primary.adminCommand({replSetGetStatus: 1})),
                        );
                        const primaryMember = statusRes.members.find((m) => m.self);
                        config.version = primaryMember.configVersion + 1;

                        config.members = originalMembers.slice(0, i);
                        cmd = {replSetReconfig: config, maxTimeMS: this.timeoutMS};
                        jsTest.log.info("Running reconfig command", {cmd});
                        const reconfigRes = primary.adminCommand(cmd);
                        const retryableReconfigCodes = [
                            ErrorCodes.NodeNotFound,
                            ErrorCodes.NewReplicaSetConfigurationIncompatible,
                            ErrorCodes.InterruptedDueToReplStateChange,
                            ErrorCodes.ConfigurationInProgress,
                            ErrorCodes.CurrentConfigNotCommittedYet,
                            ErrorCodes.NotWritablePrimary,
                        ];
                        if (retryableReconfigCodes.includes(reconfigRes.code)) {
                            jsTest.log.info("Retrying reconfig", {reconfigRes});
                            return false;
                        }
                        assert.commandWorked(reconfigRes);
                        return true;
                    },
                    "reconfig for fixture set up failed",
                    this.timeoutMS,
                    1000,
                );
            }
        }

        // Setup authentication if running test with authentication
        if ((jsTestOptions().keyFile || this.clusterAuthMode === "x509") && cmdKey === "replSetInitiate") {
            primary = this.getPrimary();
            // The sslSpecial suite sets up cluster with x509 but the shell was not started with TLS
            // so we need to rely on the test to auth if needed.
            if (!(this.clusterAuthMode === "x509" && !primary.isTLS())) {
                jsTest.authenticateNodes(this.nodes);
            }
        }

        // Wait for initial sync to complete on all nodes. Use a faster polling interval so we can
        // detect initial sync completion more quickly.
        this.awaitSecondaryNodes(this.timeoutMS, null /* secondaries */, 25 /* retryIntervalMS */);

        // If test commands are not enabled, we cannot wait for 'newlyAdded' removals. Tests that
        // disable test commands must ensure 'newlyAdded' removals mid-test are acceptable.
        if (!doNotWaitForNewlyAddedRemovals && jsTest.options().enableTestCommands) {
            this.waitForAllNewlyAddedRemovals();
        }

        jsTest.log.info(
            "ReplSetTest initiate reconfig and awaitSecondaryNodes took " +
                (new Date() - reconfigStart) +
                "ms for " +
                this.nodes.length +
                " nodes in set '" +
                this.name +
                "'",
        );

        try {
            this.awaitHighestPriorityNodeIsPrimary();
        } catch (e) {
            // Due to SERVER-14017, the call to awaitHighestPriorityNodeIsPrimary() may fail
            // in certain configurations due to being unauthorized.  In that case we proceed
            // even though we aren't guaranteed that the highest priority node is the one that
            // became primary.
            // TODO(SERVER-14017): Unconditionally expect awaitHighestPriorityNodeIsPrimary to pass.
            assert.eq(ErrorCodes.Unauthorized, e.code, tojson(e));
            jsTest.log.info(
                "Running awaitHighestPriorityNodeIsPrimary() during ReplSetTest initialization " +
                    "failed with Unauthorized error, proceeding even though we aren't guaranteed " +
                    "that the highest priority node is primary",
            );
        }

        // We need to disable the enableDefaultWriteConcernUpdatesForInitiate parameter
        // to disallow updating the default write concern after initiating is complete.
        asCluster(this, this.nodes, () => {
            for (let node of this.nodes) {
                // asCluster() currently does not validate connections with X509 authentication.
                // If the test is using X509, we skip disabling the server parameter as the
                // 'setParameter' command will fail.
                // TODO(SERVER-57924): cleanup asCluster() to avoid checking here.
                if (this._notX509Auth(node) || node.isTLS()) {
                    const serverStatus = assert.commandWorked(node.getDB("admin").runCommand({serverStatus: 1}));
                    const currVersion = serverStatus.version;
                    const olderThan50 =
                        MongoRunner.compareBinVersions(
                            MongoRunner.getBinVersionFor("5.0"),
                            MongoRunner.getBinVersionFor(currVersion),
                        ) === 1;

                    // The following params are available only on versions greater than or equal to
                    // 5.0.
                    if (olderThan50) {
                        continue;
                    }

                    if (jsTestOptions().enableTestCommands) {
                        assert.commandWorked(
                            node.adminCommand({
                                setParameter: 1,
                                enableDefaultWriteConcernUpdatesForInitiate: false,
                            }),
                        );

                        // Re-enable the reconfig check to ensure that committed writes cannot be rolled
                        // back. We disabled this check during initialization to ensure that replica
                        // sets will not fail to start up.
                        assert.commandWorked(
                            node.adminCommand({setParameter: 1, enableReconfigRollbackCommittedWritesCheck: true}),
                        );
                    }
                }
            }
        });

        const awaitTsStart = new Date(); // Measure duration of awaitLastStableRecoveryTimestamp.
        if (!doNotWaitForStableRecoveryTimestamp) {
            // Speed up the polling interval so we can detect recovery timestamps more quickly.
            this.awaitLastStableRecoveryTimestamp(25 /* retryIntervalMS */);
        }
        jsTest.log.info(
            "ReplSetTest initiate awaitLastStableRecoveryTimestamp took " +
                (new Date() - awaitTsStart) +
                "ms for " +
                this.nodes.length +
                " nodes in set '" +
                this.name +
                "'",
        );

        // Waits for the services which write on step-up to finish rebuilding to avoid background
        // writes after initiation is done. PrimaryOnlyServices wait for the stepup optime to be
        // majority committed before rebuilding services, so we skip waiting for PrimaryOnlyServices
        // if we do not wait for replication.
        if (!doNotWaitForReplication && !doNotWaitForPrimaryOnlyServices) {
            primary = this.getPrimary();
            // TODO(SERVER-57924): cleanup asCluster() to avoid checking here.
            if (this._notX509Auth(primary) || primary.isTLS()) {
                asCluster(this, primary, () => this.waitForStepUpWrites(primary));
            }
        }

        // Make sure all nodes are up to date. Bypass this if the heartbeat interval wasn't turned
        // down or the test specifies that we should not wait for replication. This is only an
        // optimization so it's OK if we bypass it in some suites.
        if (failPointsSupported && !doNotWaitForReplication) {
            asCluster(this, this.nodes, () => this.awaitNodesAgreeOnAppliedOpTime());
        }

        // Turn off the failpoints now that initial sync and initial setup is complete.
        if (failPointsSupported) {
            this.nodes.forEach(function (conn) {
                clearFailPoint(conn, "forceSyncSourceRetryWaitForInitialSync");
                clearFailPoint(conn, "forceHeartbeatIntervalMS");
                clearFailPoint(conn, "forceBgSyncSyncSourceRetryWaitMS");
            });
        }

        jsTest.log.info(
            "ReplSetTest initiateWithAnyNodeAsPrimary took " +
                (new Date() - startTime) +
                "ms for " +
                this.nodes.length +
                " nodes.",
        );
    }

    /**
     * Runs replSetInitiate on the replica set and requests the first node to step up as primary.
     * This version should be prefered where possible but requires all connections in the
     * ReplSetTest to be authorized to run replSetGetStatus.
     */
    _initiateWithNodeZeroAsPrimary(
        cfg,
        initCmd,
        {
            doNotWaitForStableRecoveryTimestamp: doNotWaitForStableRecoveryTimestamp = false,
            doNotWaitForReplication: doNotWaitForReplication = false,
            doNotWaitForNewlyAddedRemovals: doNotWaitForNewlyAddedRemovals = false,
            doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices = false,
            allNodesAuthorizedToRunRSGetStatus: allNodesAuthorizedToRunRSGetStatus = true,
        } = {},
    ) {
        let startTime = new Date(); // Measure the execution time of this function.
        this._initiateWithAnyNodeAsPrimary(cfg, initCmd, {
            doNotWaitForStableRecoveryTimestamp: doNotWaitForStableRecoveryTimestamp,
            doNotWaitForReplication: doNotWaitForReplication,
            doNotWaitForNewlyAddedRemovals: doNotWaitForNewlyAddedRemovals,
            doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices,
        });

        // stepUp() calls awaitReplication() which requires all nodes to be authorized to run
        // replSetGetStatus.
        if (!allNodesAuthorizedToRunRSGetStatus) {
            return;
        }

        // Most of the time node 0 will already be primary so we can skip the step-up.
        let primary = this.getPrimary();
        if (this.getNodeId(this.nodes[0]) == this.getNodeId(primary)) {
            jsTest.log.info(
                "ReplSetTest initiateWithNodeZeroAsPrimary skipping step-up because node 0 is " + "already primary",
            );
            asCluster(this, primary, () => {
                if (!doNotWaitForPrimaryOnlyServices) {
                    this.waitForStepUpWrites(primary);
                }
            });
        } else {
            asCluster(this, this.nodes, () => {
                const newPrimary = this.nodes[0];
                this.stepUp(newPrimary, {doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices});
                if (!doNotWaitForPrimaryOnlyServices) {
                    this.waitForStepUpWrites(newPrimary);
                }
            });
        }

        jsTest.log.info(
            "ReplSetTest initiateWithNodeZeroAsPrimary took " +
                (new Date() - startTime) +
                "ms for " +
                this.nodes.length +
                " nodes.",
        );
    }

    _addHighElectionTimeoutIfNotSet(config) {
        config = config || this.getReplSetConfig();
        config.settings = config.settings || {};
        config.settings["electionTimeoutMillis"] =
            config.settings["electionTimeoutMillis"] || ReplSetTest.kForeverMillis;
        return config;
    }

    /**
     * Initializes the replica set with `replSetInitiate`, setting a high election timeout unless
     * 'initiateWithDefaultElectionTimeout' is true. It requests the first node to step up as
     * primary. However, if 'allNodesAuthorizedToRunRSGetStatus' is set to false, any node can
     * become the primary.
     */
    initiate(
        cfg,
        initCmd,
        {
            doNotWaitForStableRecoveryTimestamp: doNotWaitForStableRecoveryTimestamp = false,
            doNotWaitForReplication: doNotWaitForReplication = false,
            doNotWaitForNewlyAddedRemovals: doNotWaitForNewlyAddedRemovals = false,
            doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices = false,
            initiateWithDefaultElectionTimeout: initiateWithDefaultElectionTimeout = false,
            allNodesAuthorizedToRunRSGetStatus: allNodesAuthorizedToRunRSGetStatus = true,
        } = {},
    ) {
        if (!initiateWithDefaultElectionTimeout) {
            cfg = this._addHighElectionTimeoutIfNotSet(cfg);
        }

        return this._initiateWithNodeZeroAsPrimary(cfg, initCmd, {
            doNotWaitForStableRecoveryTimestamp: doNotWaitForStableRecoveryTimestamp,
            doNotWaitForReplication: doNotWaitForReplication,
            doNotWaitForNewlyAddedRemovals: doNotWaitForNewlyAddedRemovals,
            doNotWaitForPrimaryOnlyServices: doNotWaitForPrimaryOnlyServices,
            allNodesAuthorizedToRunRSGetStatus: allNodesAuthorizedToRunRSGetStatus,
        });
    }

    /**
     * Runs replSetInitiate on the first node of the replica set.
     *
     * TODO (SERVER-109841): Replsetinitiate is currently a no-op command for disagg. Determine the
     * next steps for this function if additional functionality is to be incorporated.
     */
    initiateForDisagg(cfg, initCmd) {
        const startTime = new Date(); // Measure the execution time of this function.

        // Blocks until there is a primary. We use a faster retry interval here since we expect the
        // primary to be ready very soon. We also turn the failpoint off once we have a primary.
        this.getPrimary(this.kDefaultTimeoutMS, 25 /* retryIntervalMS */);

        jsTest.log(
            "ReplSetTest initiateForDisagg took " +
                (new Date() - startTime) +
                "ms for " +
                this.nodes.length +
                " nodes.",
        );
    }

    /**
     * Steps up 'node' as primary and by default it waits for the stepped up node to become a
     * writable primary and waits for all nodes to reach the same optime before sending the
     * replSetStepUp command to 'node'.
     *
     * Calls awaitReplication() which requires all connections in 'nodes' to be authenticated.
     * This stepUp() assumes that there is no network partition in the replica set.
     */
    stepUp(
        node,
        {
            awaitReplicationBeforeStepUp: awaitReplicationBeforeStepUp = true,
            awaitWritablePrimary: awaitWritablePrimary = true,
            doNotWaitForPrimaryOnlyServices = false,
        } = {},
    ) {
        jsTest.log("ReplSetTest stepUp: Stepping up " + node.host);

        if (awaitReplicationBeforeStepUp) {
            if (!doNotWaitForPrimaryOnlyServices) {
                this.waitForStepUpWrites();
            }
            this.awaitReplication();
        }

        assert.soonNoExcept(() => {
            const res = node.adminCommand({replSetStepUp: 1});
            // This error is possible if we are running mongoDB binary < 3.4 as
            // part of multi-version upgrade test. So, for those older branches,
            // simply wait for the requested node to get elected as primary due
            // to election timeout.
            if (!res.ok && res.code === ErrorCodes.CommandNotFound) {
                jsTest.log(
                    "replSetStepUp command not supported on node " +
                        node.host +
                        " ; so wait for the requested node to get elected due to election timeout.",
                );
                if (this.getPrimary() === node) {
                    return true;
                }
            }
            assert.commandWorked(res);

            // Since assert.soon() timeout is 10 minutes (default), setting
            // awaitNodesAgreeOnPrimary() timeout as 1 minute to allow retry of replSetStepUp
            // command on failure of the replica set to agree on the primary.
            // We should not run hangAnalyzer when awaitNodesAgreeOnPrimary() timeout, otherwise the
            // mongo processes will be killed and we cannot retry.
            const timeout = 60 * 1000;
            this.awaitNodesAgreeOnPrimary(timeout, this.nodes, node, false /*runHangAnalyzerOnTimeout*/);

            if (!awaitWritablePrimary) {
                return true;
            }

            // getPrimary() guarantees that there will be only one writable primary for a replica
            // set.
            const newPrimary = this.getPrimary();
            if (newPrimary.host === node.host) {
                return true;
            }

            jsTest.log(node.host + " is not primary after stepUp command, " + newPrimary.host + " is the primary");
            return false;
        }, "Timed out while waiting for stepUp to succeed on node in port: " + node.port);

        jsTest.log("ReplSetTest stepUp: Finished stepping up " + node.host);
        return node;
    }

    /**
     * Wait for writes which may happen when nodes are stepped up.  This currently includes
     * primary-only service writes and writes from the query analysis writer, the latter being
     * a replica-set-aware service for which there is no generic way to wait.
     */
    waitForStepUpWrites(primary) {
        primary = primary || this.getPrimary();
        this.waitForPrimaryOnlyServices(primary);
        this.waitForQueryAnalysisWriterSetup(primary);
    }

    /**
     * Waits for primary only services to finish the rebuilding stage after a primary is elected.
     * This is useful for tests that are expecting particular write timestamps since some primary
     * only services can do background writes (e.g. build indexes) during rebuilding stage that
     * could advance the last write timestamp.
     */
    waitForPrimaryOnlyServices(primary) {
        jsTest.log("Waiting for primary only services to finish rebuilding");
        primary = primary || this.getPrimary();

        assert.soonNoExcept(function () {
            const res = assert.commandWorked(primary.adminCommand({serverStatus: 1, repl: 1}));
            // 'PrimaryOnlyServices' does not exist prior to v5.0, using empty
            // object to skip waiting in case of multiversion tests.
            const services = res.repl.primaryOnlyServices || {};
            return Object.keys(services).every((s) => {
                return services[s].state === undefined || services[s].state === "running";
            });
        }, "Timed out waiting for primary only services to finish rebuilding");
    }

    /**
     * If query sampling is supported, waits for the query analysis writer to finish setting up
     * after a primary is elected. This is useful for tests that expect particular write timestamps
     * since the query analysis writer setup involves building indexes for the config.sampledQueries
     * and config.sampledQueriesDiff collections.
     */
    waitForQueryAnalysisWriterSetup(primary) {
        primary = primary || this.getPrimary();

        const serverStatusRes = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        if (!serverStatusRes.hasOwnProperty("queryAnalyzers")) {
            // Query sampling is not supported on this replica set. That is, either it uses binaries
            // released before query sampling was introduced or it uses binaries where query
            // sampling is guarded by a feature flag and the feature flag is not enabled.
            return;
        }

        const getParamsRes = primary.adminCommand({getParameter: 1, multitenancySupport: 1});
        if (!getParamsRes.ok || getParamsRes["multitenancySupport"]) {
            // Query sampling is not supported on a multi-tenant replica set.
            return;
        }

        jsTest.log("Waiting for query analysis writer to finish setting up");

        assert.soonNoExcept(function () {
            const sampledQueriesIndexes = primary.getCollection("config.sampledQueries").getIndexes();
            const sampledQueriesDiffIndexes = primary.getCollection("config.sampledQueriesDiff").getIndexes();
            // There should be two indexes: _id index and TTL index.
            return sampledQueriesIndexes.length == 2 && sampledQueriesDiffIndexes.length == 2;
        }, "Timed out waiting for query analysis writer to finish setting up");
    }

    /**
     * Gets the current replica set config from the specified node index. If no nodeId is specified,
     * uses the primary node.
     */
    getReplSetConfigFromNode(nodeId) {
        if (nodeId == undefined) {
            // Use 90 seconds timeout for finding a primary
            return _replSetGetConfig(this.getPrimary(90 * 1000));
        }

        if (!isNumber(nodeId)) {
            throw Error(nodeId + " is not a number");
        }

        return _replSetGetConfig(this.nodes[nodeId]);
    }

    reInitiate() {
        let config = this.getReplSetConfigFromNode();
        let newConfig = this.getReplSetConfig();
        // Only reset members.
        config.members = newConfig.members;
        config.version += 1;

        this._setDefaultConfigOptions(config);

        // Set a maxTimeMS so reconfig fails if it times out.
        assert.adminCommandWorkedAllowingNetworkError(this.getPrimary(), {
            replSetReconfig: config,
            maxTimeMS: this.timeoutMS,
        });
    }

    /**
     * Blocks until all nodes in the replica set have the same config version as the primary.
     **/
    awaitNodesAgreeOnConfigVersion(timeout) {
        timeout = timeout || this.timeoutMS;

        assert.soonNoExcept(
            () => {
                let primaryVersion = this.getPrimary().getDB("admin")._helloOrLegacyHello().setVersion;

                for (let i = 0; i < this.nodes.length; i++) {
                    let version = this.nodes[i].getDB("admin")._helloOrLegacyHello().setVersion;
                    assert.eq(
                        version,
                        primaryVersion,
                        "waiting for secondary node " +
                            this.nodes[i].host +
                            " with config version of " +
                            version +
                            " to match the version of the primary " +
                            primaryVersion,
                    );
                }

                return true;
            },
            "Awaiting nodes to agree on config version",
            timeout,
        );
    }

    /**
     * Waits for the last oplog entry on the primary to be visible in the committed snapshot view
     * of the oplog on *all* secondaries. When majority read concern is disabled, there is no
     * committed snapshot view, so this function waits for the knowledge of the majority commit
     * point on each node to advance to the optime of the last oplog entry on the primary.
     * Returns last oplog entry.
     */
    awaitLastOpCommitted(timeout, members) {
        let rst = this;
        let primary = rst.getPrimary();
        let primaryOpTime = _getLastOpTime(this, primary);

        let membersToCheck;
        if (members !== undefined) {
            jsTest.log.info("Waiting for op to be committed on " + members.map((s) => s.host), {opTime: primaryOpTime});

            membersToCheck = members;
        } else {
            jsTest.log.info("Waiting for op to be committed on all secondaries", {opTime: primaryOpTime});

            membersToCheck = rst.nodes;
        }

        assert.soonNoExcept(
            function () {
                for (let i = 0; i < membersToCheck.length; i++) {
                    var node = membersToCheck[i];
                    // Continue if we're connected to an arbiter
                    const res = asCluster(rst, node, () =>
                        assert.commandWorked(node.adminCommand({replSetGetStatus: 1})),
                    );

                    if (res.myState == ReplSetTest.State.ARBITER) {
                        continue;
                    }
                    let rcmOpTime = rst.getReadConcernMajorityOpTime(node);
                    if (friendlyEqual(rcmOpTime, {ts: Timestamp(0, 0), t: NumberLong(0)})) {
                        return false;
                    }
                    if (globalThis.rs.compareOpTimes(rcmOpTime, primaryOpTime) < 0) {
                        return false;
                    }
                }

                return true;
            },
            "Op with OpTime " + tojson(primaryOpTime) + " failed to be committed on all secondaries",
            timeout,
        );

        jsTest.log.info("Op successfully committed on all secondaries", {opTime: primaryOpTime});
        return primaryOpTime;
    }

    /**
     * This function performs some writes and then waits for all nodes in this replica set to
     * establish a stable recovery timestamp. The writes are necessary to prompt storage engines to
     * quickly establish stable recovery timestamps.
     *
     * A stable recovery timestamp ensures recoverable rollback is possible, as well as startup
     * recovery without re-initial syncing in the case of durable storage engines. By waiting for
     * all nodes to report having a stable recovery timestamp, we ensure a degree of stability in
     * our tests to run as expected.
     */
    awaitLastStableRecoveryTimestamp(retryIntervalMS) {
        let rst = this;
        let primary = rst.getPrimary();
        let id = tojson(rst.nodeList());
        retryIntervalMS = retryIntervalMS || 200;

        // All nodes must be in primary/secondary state prior to this point. Perform a majority
        // write to ensure there is a committed operation on the set. The commit point will
        // propagate to all members and trigger a stable checkpoint on all persisted storage engines
        // nodes.
        function advanceCommitPoint(rst, primary) {
            // Shadow 'db' so that we can call the function on the primary without a separate shell
            // when x509 auth is not needed.
            let db = primary.getDB("admin");
            const appendOplogNoteFn = function () {
                assert.commandWorked(
                    db.adminCommand({
                        "appendOplogNote": 1,
                        "data": {"awaitLastStableRecoveryTimestamp": 1},
                        // We use the global kDefaultTimeoutMS value since this func is passed to a new
                        // shell without context.
                        // TODO(SERVER-14017): Remove subshell use
                        "writeConcern": {"w": "majority", "wtimeout": ReplSetTest.kDefaultTimeoutMS},
                    }),
                );
            };

            runFnWithAuthOnPrimary(rst, appendOplogNoteFn, "AwaitLastStableRecoveryTimestamp");
        }

        jsTest.log.info("AwaitLastStableRecoveryTimestamp: Beginning for " + id);

        let replSetStatus = asCluster(rst, primary, () =>
            assert.commandWorked(primary.adminCommand({replSetGetStatus: 1})),
        );
        if (replSetStatus["configsvr"]) {
            // Performing dummy replicated writes against a configsvr is hard, especially if auth
            // is also enabled.
            return;
        }

        rst.awaitNodesAgreeOnPrimary();
        primary = rst.getPrimary();

        jsTest.log.info("AwaitLastStableRecoveryTimestamp: ensuring the commit point advances for " + id);
        advanceCommitPoint(this, primary);

        jsTest.log.info("AwaitLastStableRecoveryTimestamp: Waiting for stable recovery timestamps for " + id);

        assert.soonNoExcept(
            function () {
                for (let node of rst.nodes) {
                    // The `lastStableRecoveryTimestamp` field contains a stable timestamp
                    // guaranteed to exist on storage engine recovery to stable timestamp.
                    let res = asCluster(rst, node, () =>
                        assert.commandWorked(node.adminCommand({replSetGetStatus: 1})),
                    );

                    // Continue if we're connected to an arbiter.
                    if (res.myState === ReplSetTest.State.ARBITER) {
                        continue;
                    }

                    // A missing `lastStableRecoveryTimestamp` field indicates that the storage
                    // engine does not support `recover to a stable timestamp`.
                    //
                    // A null `lastStableRecoveryTimestamp` indicates that the storage engine
                    // supports "recover to a stable timestamp", but does not have a stable recovery
                    // timestamp yet.
                    if (
                        res.hasOwnProperty("lastStableRecoveryTimestamp") &&
                        res.lastStableRecoveryTimestamp.getTime() === 0
                    ) {
                        jsTest.log.info(
                            "AwaitLastStableRecoveryTimestamp: " +
                                node.host +
                                " does not have a stable recovery timestamp yet.",
                        );
                        return false;
                    }
                }

                return true;
            },
            "Not all members have a stable recovery timestamp",
            this.timeoutMS,
            retryIntervalMS,
        );

        jsTest.log.info(
            "AwaitLastStableRecoveryTimestamp: A stable recovery timestamp has successfully " + "established on " + id,
        );
    }

    // Wait until the optime of the specified type reaches the primary or the targetNode's last
    // applied optime if provided. Blocks on all secondary nodes or just 'secondaries', if
    // specified. The timeout will reset if any of the secondaries makes progress.
    awaitReplication(timeout, secondaryOpTimeType, secondaries, retryIntervalMS, targetNode) {
        if (secondaries !== undefined && secondaries !== this._secondaries) {
            jsTest.log.info("ReplSetTest awaitReplication: going to check only " + secondaries.map((s) => s.host));
        }

        if (targetNode !== undefined) {
            jsTest.log.info(
                `ReplSetTest awaitReplication: wait against targetNode ${targetNode.host} instead of primary.`,
            );
        }

        timeout = timeout || this.timeoutMS;
        retryIntervalMS = retryIntervalMS || 200;

        secondaryOpTimeType = secondaryOpTimeType || ReplSetTest.OpTimeType.LAST_APPLIED;

        let targetLatestOpTime;

        // Blocking call, which will wait for the last optime written on the target to be available
        let awaitLastOpTimeWrittenFn = function (rst) {
            let target = targetNode || rst.getPrimary();
            assert.soonNoExcept(
                function () {
                    try {
                        targetLatestOpTime = _getLastOpTime(rst, target);
                    } catch (e) {
                        jsTest.log.info("ReplSetTest caught exception", {error: e});
                        return false;
                    }

                    return true;
                },
                "awaiting oplog query",
                timeout,
            );
        };
        awaitLastOpTimeWrittenFn(this);

        // get the latest config version from target (with a few retries in case of error)
        let targetConfigVersion;
        let targetName;
        let target;
        let num_attempts = 3;

        assert.retryNoExcept(
            () => {
                target = targetNode || this.getPrimary();
                targetConfigVersion = asCluster(this, target, () =>
                    this.getReplSetConfigFromNode(this.getNodeId(target)),
                ).version;
                targetName = target.host;
                return true;
            },
            "ReplSetTest awaitReplication: couldnt get repl set config.",
            num_attempts,
            1000,
        );

        jsTest.log.info("ReplSetTest awaitReplication: starting: for target, " + targetName, {
            opTime: targetLatestOpTime,
        });

        let nodesCaughtUp = false;
        let secondariesToCheck = secondaries || this._secondaries;
        let nodeProgress = Array(secondariesToCheck.length);

        const Progress = Object.freeze({
            Skip: "Skip",
            CaughtUp: "CaughtUp",
            InProgress: "InProgress",
            Stuck: "Stuck",
            ConfigMismatch: "ConfigMismatch",
        });

        function checkProgressSingleNode(rst, index, secondaryCount) {
            let secondary = secondariesToCheck[index];
            let secondaryName = secondary.host;

            let secondaryConfigVersion = asCluster(
                rst,
                secondary,
                () => secondary.getDB("local")["system.replset"].find().readConcern("local").limit(1).next().version,
            );

            if (targetConfigVersion != secondaryConfigVersion) {
                jsTest.log.info(
                    "ReplSetTest awaitReplication: secondary #" +
                        secondaryCount +
                        ", " +
                        secondaryName +
                        ", has config version #" +
                        secondaryConfigVersion +
                        ", but expected config version #" +
                        targetConfigVersion,
                );

                if (secondaryConfigVersion > targetConfigVersion) {
                    target = targetNode || rst.getPrimary();
                    targetConfigVersion = target
                        .getDB("local")
                        ["system.replset"].find()
                        .readConcern("local")
                        .limit(1)
                        .next().version;
                    targetName = target.host;

                    jsTest.log.info("ReplSetTest awaitReplication: for target, " + targetName, {
                        opTime: targetLatestOpTime,
                    });
                }

                return Progress.ConfigMismatch;
            }
            // Skip this node if we're connected to an arbiter
            let res = asCluster(rst, secondary, () =>
                assert.commandWorked(secondary.adminCommand({replSetGetStatus: 1})),
            );
            if (res.myState == ReplSetTest.State.ARBITER) {
                return Progress.Skip;
            }

            jsTest.log.info(
                "ReplSetTest awaitReplication: checking secondary #" + secondaryCount + ": " + secondaryName,
            );

            secondary.getDB("admin").getMongo().setSecondaryOk();

            let secondaryOpTime;
            if (secondaryOpTimeType == ReplSetTest.OpTimeType.LAST_DURABLE) {
                secondaryOpTime = _getDurableOpTime(rst, secondary);
            } else {
                secondaryOpTime = _getLastOpTime(rst, secondary);
            }

            // If the node doesn't have a valid opTime, it likely hasn't received any writes from
            // the primary yet.
            if (!globalThis.rs.isValidOpTime(secondaryOpTime)) {
                jsTest.log.info(
                    "ReplSetTest awaitReplication: optime for secondary #" +
                        secondaryCount +
                        ", " +
                        secondaryName +
                        ", is NOT valid.",
                    {opTime: secondaryOpTime},
                );
                return Progress.Stuck;
            }

            // See if the node made progress. We count it as progress even if the node's last optime
            // went backwards because that means the node is in rollback.
            let madeProgress =
                nodeProgress[index] && globalThis.rs.compareOpTimes(nodeProgress[index], secondaryOpTime) != 0;
            nodeProgress[index] = secondaryOpTime;

            if (globalThis.rs.compareOpTimes(targetLatestOpTime, secondaryOpTime) < 0) {
                targetLatestOpTime = _getLastOpTime(rst, target);
                jsTest.log.info(
                    "ReplSetTest awaitReplication: optime for " +
                        secondaryName +
                        " is newer, resetting latest target optime. Also resetting awaitReplication timeout.",
                    {resetOpTime: targetLatestOpTime},
                );
                return Progress.InProgress;
            }

            if (!friendlyEqual(targetLatestOpTime, secondaryOpTime)) {
                jsTest.log.info(
                    "ReplSetTest awaitReplication: optime for secondary #" +
                        secondaryCount +
                        ", " +
                        secondaryName +
                        ", is different than latest optime",
                    {secondaryOpTime, targetLatestOpTime},
                );
                jsTest.log.info(
                    "ReplSetTest awaitReplication: secondary #" +
                        secondaryCount +
                        ", " +
                        secondaryName +
                        ", is NOT synced",
                );

                // Reset the timeout if a node makes progress, but isn't caught up yet.
                if (madeProgress) {
                    jsTest.log.info(
                        "ReplSetTest awaitReplication: secondary #" +
                            secondaryCount +
                            ", " +
                            secondaryName +
                            ", has made progress. Resetting awaitReplication timeout",
                    );
                    return Progress.InProgress;
                }
                return Progress.Stuck;
            }

            jsTest.log.info(
                "ReplSetTest awaitReplication: secondary #" + secondaryCount + ", " + secondaryName + ", is synced",
            );
            return Progress.CaughtUp;
        }

        // We will reset the timeout if a nodes makes progress, but still isn't caught up yet.
        while (!nodesCaughtUp) {
            assert.soonNoExcept(
                () => {
                    try {
                        jsTest.log.info(
                            "ReplSetTest awaitReplication: checking secondaries against latest target optime",
                            {targetLatestOpTime},
                        );
                        let secondaryCount = 0;

                        for (let i = 0; i < secondariesToCheck.length; i++) {
                            const action = checkProgressSingleNode(this, i, secondaryCount);

                            switch (action) {
                                case Progress.CaughtUp:
                                    // We only need to increment the secondaryCount if this node is
                                    // caught up.
                                    secondaryCount++;
                                    continue;
                                case Progress.Skip:
                                    // Don't increment secondaryCount because this node is an arbiter.
                                    continue;
                                case Progress.InProgress:
                                    return true;
                                case Progress.Stuck:
                                case Progress.ConfigMismatch:
                                    return false;
                                default:
                                    throw Error("invalid action: " + tojson(action));
                            }
                        }

                        jsTest.log.info(
                            "ReplSetTest awaitReplication: finished: all " + secondaryCount + " secondaries synced",
                            {opTime: targetLatestOpTime},
                        );
                        nodesCaughtUp = true;
                        return true;
                    } catch (e) {
                        jsTest.log.info("ReplSetTest awaitReplication: caught exception", {error: e});

                        // We might have a new primary now
                        awaitLastOpTimeWrittenFn(this);

                        jsTest.log.info("ReplSetTest awaitReplication: resetting: for target " + target, {
                            opTime: targetLatestOpTime,
                        });

                        return false;
                    }
                },
                "awaitReplication timed out",
                timeout,
                retryIntervalMS,
            );
        }
    }

    getHashesUsingSessions(
        sessions,
        dbName,
        {readAtClusterTime, skipTempCollections = false} = {
            skipTempCollections: false,
        },
    ) {
        return sessions.map((session) => {
            const commandObj = {dbHash: 1};
            const db = session.getDatabase(dbName);
            // Use snapshot read concern for dbhash.
            if (readAtClusterTime !== undefined) {
                commandObj.readConcern = {level: "snapshot", atClusterTime: readAtClusterTime};
            }
            if (skipTempCollections) {
                commandObj.skipTempCollections = 1;
            }
            // If we are running in a multiversion suite, preserve old behavior of checking capped
            // collections in _id order instead of natural order. The
            // 'useIndexScanForCappedCollections' option for dbHash should be ignored in older
            // binaries.
            if (
                typeof TestData !== "undefined" &&
                (TestData.useRandomBinVersionsWithinReplicaSet ||
                    TestData.mongosBinVersion ||
                    TestData.multiversionBinVersion ||
                    TestData.mixedBinVersions)
            ) {
                commandObj.useIndexScanForCappedCollections = 1;
            }

            return assert.commandWorked(db.runCommand(commandObj));
        });
    }

    // Gets the dbhash for the current primary and for all secondaries (or the members of
    // 'secondaries', if specified).
    getHashes(dbName, secondaries, skipTempCollections) {
        assert.neq(dbName, "local", 'Cannot run getHashes() on the "local" database');

        // _determineLiveSecondaries() repopulates both 'self._secondaries' and 'self._primary'. If
        // we're passed an explicit set of secondaries we don't want to do that.
        secondaries = secondaries || _determineLiveSecondaries(this);

        const sessions = [
            this._primary,
            ...secondaries.filter((conn) => {
                return !conn.getDB("admin")._helloOrLegacyHello().arbiterOnly;
            }),
        ].map((conn) => conn.getDB("test").getSession());

        const hashes = this.getHashesUsingSessions(sessions, dbName, {skipTempCollections});
        return {primary: hashes[0], secondaries: hashes.slice(1)};
    }

    findOplog(conn, query, limit) {
        return conn.getDB("local").getCollection(kOplogName).find(query).sort({$natural: -1}).limit(limit);
    }

    dumpOplog(conn, query = {}, limit = 10) {
        let log =
            "Dumping the latest " +
            limit +
            " documents that match " +
            tojson(query) +
            " from the oplog " +
            kOplogName +
            " of " +
            conn.host;
        let entries = [];
        let cursor = this.findOplog(conn, query, limit);
        cursor.forEach(function (entry) {
            log = log + "\n" + tojsononeline(entry);
            entries.push(entry);
        });
        jsTestLog(log);
        return entries;
    }

    // Call the provided checkerFunction, after the replica set has been write locked.
    checkReplicaSet(checkerFunction, secondaries, ...checkerFunctionArgs) {
        assert.eq(typeof checkerFunction, "function", "Expected checkerFunction parameter to be a function");

        assert(secondaries, "must pass list of live nodes to checkReplicaSet");

        // Call getPrimary to populate rst with information about the nodes.
        let primary = this.getPrimary();
        assert(primary, "calling getPrimary() failed");

        // Ensure that the current primary isn't in the secondaries list from a stale
        // determineLiveSecondaries call. Otherwise we will mistakenly freeze the current primary.
        const primIndex = secondaries.indexOf(primary);
        if (primIndex > -1) {
            secondaries.splice(primIndex, 1);
        }
        checkerFunctionArgs.push(secondaries);

        // Prevent an election, which could start, then hang due to the fsyncLock.
        jsTestLog(`Freezing nodes: [${secondaries.map((n) => n.host)}]`);
        secondaries.forEach((secondary) => this.freeze(secondary));

        // Await primary in case freeze() had to step down a node that was unexpectedly primary.
        this.getPrimary();

        // Lock the primary to prevent writes in the background while we are getting the
        // dbhashes of the replica set members. It's not important if the storage engine fails
        // to perform its fsync operation. The only requirement is that writes are locked out.
        assert.commandWorked(
            primary.adminCommand({fsync: 1, lock: 1, allowFsyncFailure: true}),
            "failed to lock the primary",
        );

        function postApplyCheckerFunction() {
            // Unfreeze secondaries and unlock primary.
            try {
                assert.commandWorked(primary.adminCommand({fsyncUnlock: 1}));
            } catch (e) {
                jsTest.log.info("Continuing after fsyncUnlock error", {error: e});
            }

            secondaries.forEach((secondary) => {
                try {
                    assert.commandWorked(secondary.adminCommand({replSetFreeze: 0}));
                } catch (e) {
                    jsTest.log.info("Continuing after replSetFreeze error", {error: e});
                }
            });
        }

        let activeException = false;
        try {
            this.awaitReplication(null, null, secondaries);
            checkerFunction.apply(this, checkerFunctionArgs);
        } catch (e) {
            activeException = true;
            throw e;
        } finally {
            if (activeException) {
                try {
                    postApplyCheckerFunction();
                } catch (e) {
                    // Print the postApplyCheckerFunction error, propagate the original.
                    jsTest.log.info({error: e});
                }
            } else {
                postApplyCheckerFunction();
            }
        }
    }

    // Check the replicated data hashes for all live nodes in the set.
    checkReplicatedDataHashes(msgPrefix = "checkReplicatedDataHashes", excludedDBs = [], ignoreUUIDs = false) {
        // Return items that are in either Array `a` or `b` but not both. Note that this will
        // not work with arrays containing NaN. Array.indexOf(NaN) will always return -1.

        let collectionPrinted = new Set();

        function checkDBHashesForReplSet(rst, dbDenylist = [], msgPrefix, ignoreUUIDs, secondaries) {
            // We don't expect the local database to match because some of its
            // collections are not replicated.
            dbDenylist.push("local");
            secondaries = secondaries || rst._secondaries;

            let success = true;
            let hasDumpedOplog = false;

            // Use '_primary' instead of getPrimary() to avoid the detection of a new primary.
            // '_primary' must have been populated.
            const primary = rst._primary;

            let combinedDBs = new Map();
            primary.getDBs().databases.map((db) => {
                const key = `${db.tenantId}_${db.name}`;
                const obj = {"name": db.name, "tenant": db.tenantId};
                combinedDBs.set(key, obj);
            });

            const replSetConfig = rst.getReplSetConfigFromNode();

            jsTest.log.info("checkDBHashesForReplSet waiting for secondaries to be ready", {secondaries});
            this.awaitSecondaryNodes(rst.timeoutMS, secondaries);

            jsTest.log.info("checkDBHashesForReplSet checking data hashes against primary: " + primary.host);

            secondaries.forEach((node) => {
                // Arbiters have no replicated data.
                if (isNodeArbiter(node)) {
                    jsTest.log.info("checkDBHashesForReplSet skipping data of arbiter: " + node.host);
                    return;
                }
                jsTest.log.info("checkDBHashesForReplSet going to check data hashes on secondary: " + node.host);
                node.getDBs().databases.forEach((db) => {
                    const key = `${db.tenantId}_${db.name}`;
                    const obj = {"name": db.name, "tenant": db.tenantId};
                    combinedDBs.set(key, obj);
                });
            });

            const expectPrefix = typeof TestData !== "undefined" && TestData.multitenancyExpectPrefix ? true : false;

            for (const [key, db] of combinedDBs) {
                const tenant = db.tenant;
                const dbName = expectPrefix && tenant ? tenant + "_" + db.name : db.name;

                if (Array.contains(dbDenylist, dbName)) {
                    continue;
                }

                const token = db.tenant ? _createTenantToken({tenant, expectPrefix}) : undefined;
                try {
                    primary._setSecurityToken(token);
                    secondaries.forEach((node) => node._setSecurityToken(token));

                    const dbHashes = rst.getHashes(dbName, secondaries);
                    const primaryDBHash = dbHashes.primary;
                    const primaryCollections = Object.keys(primaryDBHash.collections);
                    assert.commandWorked(primaryDBHash);

                    // Filter only collections that were retrieved by the dbhash.
                    // listCollections may include non-replicated collections like
                    // system.profile.
                    const primaryCollInfos = new CollInfos(primary, "primary", dbName);
                    primaryCollInfos.filter(primaryCollections);

                    dbHashes.secondaries.forEach((secondaryDBHash) => {
                        assert.commandWorked(secondaryDBHash);

                        const secondary = secondaryDBHash._mongo;
                        const secondaryCollections = Object.keys(secondaryDBHash.collections);
                        // Check that collection information is consistent on the primary and
                        // secondaries.
                        const secondaryCollInfos = new CollInfos(secondary, "secondary", dbName);
                        secondaryCollInfos.filter(secondaryCollections);

                        const hasSecondaryIndexes =
                            replSetConfig.members[rst.getNodeId(secondary)].buildIndexes !== false;

                        jsTest.log.info(
                            `checking db hash between primary: ${primary.host}, and secondary: ${secondary.host}`,
                        );
                        success =
                            DataConsistencyChecker.checkDBHash(
                                primaryDBHash,
                                primaryCollInfos,
                                secondaryDBHash,
                                secondaryCollInfos,
                                msgPrefix,
                                ignoreUUIDs,
                                hasSecondaryIndexes,
                                collectionPrinted,
                            ) && success;

                        if (!success) {
                            if (!hasDumpedOplog) {
                                jsTest.log.info("checkDBHashesForReplSet dumping oplogs from all nodes");
                                this.dumpOplog(primary, {}, 100);
                                rst.getSecondaries().forEach((secondary) => this.dumpOplog(secondary, {}, 100));
                                hasDumpedOplog = true;
                            }
                        }
                    });
                } finally {
                    primary._setSecurityToken(undefined);
                    secondaries.forEach((node) => node._setSecurityToken(undefined));
                }
            }

            assert(success, "dbhash mismatch between primary and secondary");
        }

        const liveSecondaries = _determineLiveSecondaries(this);
        this.checkReplicaSet(checkDBHashesForReplSet, liveSecondaries, this, excludedDBs, msgPrefix, ignoreUUIDs);
    }

    checkOplogs(msgPrefix) {
        let liveSecondaries = _determineLiveSecondaries(this);
        this.checkReplicaSet(checkOplogs, liveSecondaries, this, msgPrefix);
    }

    checkPreImageCollection(msgPrefix) {
        let liveSecondaries = _determineLiveSecondaries(this);
        this.checkReplicaSet(checkPreImageCollection, liveSecondaries, this, msgPrefix);
    }

    checkChangeCollection(msgPrefix) {
        let liveSecondaries = _determineLiveSecondaries(this);
        this.checkReplicaSet(checkChangeCollection, liveSecondaries, this, msgPrefix);
    }

    /**
     * Waits for an initial connection to a given node. Should only be called after the node's
     * process has already been started. Updates the corresponding entry in 'this.nodes' with the
     * newly established connection object.
     *
     * @private
     * @param {int} [n] the node id.
     * @param {boolean} [waitForHealth] If true, wait for the health indicator of the replica set
     *     node after waiting for a connection. Default: false.
     * @returns a new Mongo connection object to the node.
     */
    _waitForInitialConnection(n, waitForHealth) {
        jsTest.log.info("ReplSetTest waiting for an initial connection to node " + n);

        // If we are using a bridge, then we want to get at the underlying mongod node object.
        let node = this._useBridge ? this._unbridgedNodes[n] : this.nodes[n];
        let pid = node.pid;
        let port = node.port;
        let conn = MongoRunner.awaitConnection({pid, port});
        if (!conn) {
            throw new Error("Failed to connect to node " + n);
        }

        // Attach the original node properties to the connection object.
        Object.assign(conn, node);

        // Delete the session since it's linked to the other mongo object.
        delete conn._defaultSession;

        // Authenticate again since this is a new connection.
        if (jsTestOptions().keyFile || this.clusterAuthMode === "x509") {
            // The sslSpecial suite sets up cluster with x509 but the shell was not started with TLS
            // so we need to rely on the test to auth if needed.
            if (!(this.clusterAuthMode === "x509" && !conn.isTLS())) {
                jsTest.authenticate(conn);
            }
        }

        // Save the new connection object. If we are using a bridge, then we need to connect to it.
        if (this._useBridge) {
            this.nodes[n].connectToBridge();
            this.nodes[n].nodeId = n;
            this._unbridgedNodes[n] = conn;
        } else {
            this.nodes[n] = conn;
        }

        jsTest.log.info("ReplSetTest made initial connection to node", {node: this.nodes[n]});

        waitForHealth = waitForHealth || false;
        if (waitForHealth) {
            // Wait for node to start up.
            this._waitForIndicator(this.nodes[n], "health", Health.UP);
        }

        if (this._causalConsistency) {
            this.nodes[n].setCausalConsistency(true);
        }
        return this.nodes[n];
    }

    /**
     * Starts up a server.  Options are saved by default for subsequent starts.
     *
     * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
     * @param {object} [options]
     * @param {boolean} [options.remember] Reapplies the saved options from a prior start.
     * @param {boolean} [options.noRemember] Ignores the current properties.
     * @param {boolean} [options.appendOptions] Appends the current options to those remembered.
     * @param {boolean} [options.startClean] Clears the data directory before starting.
     * @param {boolean} [restart=false] If false, the data directory will be cleared before the
     *     server starts.
     * @param {boolean} [waitForHealth=false] If true, wait for the health indicator of the replica
     *     set node after waiting for a connection.
     */
    start(n, options, restart, waitForHealth) {
        n = resolveToNodeId(this, n);
        jsTest.log.info("ReplSetTest n is : " + n);

        let defaults = {
            useHostName: this.useHostName,
            oplogSize: this.oplogSize,
            keyFile: this.keyFile,
            port: this._useBridge ? this._unbridgedPorts[n] : this.ports[n],
            dbpath: "$set-$node",
        };
        if (jsTestOptions().shellGRPC) {
            defaults.grpcPort = this.grpcPorts[n];
        }

        if (this.useAutoBootstrapProcedure) {
            if (n == 0) {
                // No --replSet for the first node.
            } else {
                defaults.replSet = this.name;
            }
        } else {
            defaults.replSet = this.useSeedList ? this.getURL() : this.name;
        }

        const nodeOptions = this.nodeOptions["n" + n];
        const hasBinVersion = (options && options.binVersion) || (nodeOptions && nodeOptions.binVersion);
        if (hasBinVersion && jsTest.options().useRandomBinVersionsWithinReplicaSet) {
            throw new Error("Can only specify one of binVersion and useRandomBinVersionsWithinReplicaSet, not both.");
        }

        // Note : this replaces the binVersion of the shared startSet() options the first time
        // through, so the full set is guaranteed to have different versions if size > 1.  If using
        // start() independently, independent version choices will be made
        //
        if (options && options.binVersion) {
            options.binVersion = MongoRunner.versionIterator(options.binVersion);
        }

        // Always set log format
        if (options && options.logFormat) {
            options.logFormat = jsTest.options().logFormat;
        }

        // If restarting a node, use its existing options as the defaults unless remember is false.
        let baseOptions;
        if ((options && options.restart) || restart) {
            if (options && options.remember === false) {
                baseOptions = defaults;
            } else {
                baseOptions = this._useBridge ? this._unbridgedNodes[n].fullOptions : this.nodes[n].fullOptions;
            }
        } else {
            baseOptions = defaults;
        }
        baseOptions = Object.merge(baseOptions, nodeOptions);
        options = Object.merge(baseOptions, options);
        if (options.hasOwnProperty("rsConfig")) {
            this.nodeOptions["n" + n] = Object.merge(this.nodeOptions["n" + n], {rsConfig: options.rsConfig});
        }
        delete options.rsConfig;

        if (jsTest.options().useRandomBinVersionsWithinReplicaSet) {
            if (this.isConfigServer) {
                // Our documented upgrade/downgrade paths for a sharded cluster lets us assume that
                // config server nodes will always be fully upgraded before the shard nodes.
                options.binVersion = "latest";
            } else {
                const rand = Random.rand();
                options.binVersion = rand < 0.5 ? "latest" : jsTest.options().useRandomBinVersionsWithinReplicaSet;
            }
            jsTest.log.info("Randomly assigned binary version: " + options.binVersion + " to node: " + n);
        }

        options.restart = options.restart || restart;

        let pathOpts = {node: n, set: this.name};
        options.pathOpts = Object.merge(options.pathOpts || {}, pathOpts);

        // Turn off periodic noop writes for replica sets by default.
        options.setParameter = options.setParameter || {};
        if (typeof options.setParameter === "string") {
            let eqIdx = options.setParameter.indexOf("=");
            if (eqIdx != -1) {
                let param = options.setParameter.substring(0, eqIdx);
                let value = options.setParameter.substring(eqIdx + 1);
                options.setParameter = {};
                options.setParameter[param] = value;
            }
        }
        options.setParameter.writePeriodicNoops = options.setParameter.writePeriodicNoops || false;

        // We raise the number of initial sync connect attempts for tests that disallow chaining.
        // Disabling chaining can cause sync source selection to take longer so we must increase
        // the number of connection attempts.
        options.setParameter.numInitialSyncConnectAttempts = options.setParameter.numInitialSyncConnectAttempts || 60;

        // The default time for stepdown and quiesce mode in response to SIGTERM is 15 seconds.
        // Reduce this to 100ms for faster shutdown.
        options.setParameter.shutdownTimeoutMillisForSignaledShutdown =
            options.setParameter.shutdownTimeoutMillisForSignaledShutdown || 100;

        if (jsTestOptions().enableTestCommands) {
            // This parameter is enabled to allow the default write concern to change while
            // initiating a ReplSetTest. This is due to our testing optimization to initiate
            // with a single node, and reconfig the full membership set in.
            // We need to recalculate the DWC after each reconfig until the full set is included.
            options.setParameter.enableDefaultWriteConcernUpdatesForInitiate = true;
        }

        if (
            baseOptions.hasOwnProperty("setParameter") &&
            baseOptions.setParameter.hasOwnProperty("featureFlagTransitionToCatalogShard") &&
            baseOptions.setParameter.featureFlagTransitionToCatalogShard
        ) {
            options.setParameter.featureFlagTransitionToCatalogShard = true;
        }

        // Disable a check in reconfig that will prevent certain configs with arbiters from
        // spinning up. We will re-enable this check after the replica set has finished initiating.
        if (jsTestOptions().enableTestCommands) {
            options.setParameter.enableReconfigRollbackCommittedWritesCheck = false;
            options.setParameter.disableTransitionFromLatestToLastContinuous =
                options.setParameter.disableTransitionFromLatestToLastContinuous || false;
        }

        if (jsTestOptions().performTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert) {
            options.setParameter.performTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert = true;
        }

        if (this.useAutoBootstrapProcedure) {
            options.setParameter.featureFlagAllMongodsAreSharded = true;
        }

        if (typeof TestData !== "undefined" && TestData.replicaSetEndpointIncompatible) {
            options.setParameter.featureFlagReplicaSetEndpoint = false;
        }

        const olderThan73 =
            MongoRunner.compareBinVersions(
                MongoRunner.getBinVersionFor("7.3"),
                MongoRunner.getBinVersionFor(options.binVersion),
            ) === 1;
        if (olderThan73) {
            delete options.setParameter.featureFlagClusteredConfigTransactions;
            delete options.setParameter.featureFlagReplicaSetEndpoint;
        }

        const olderThan81 =
            MongoRunner.compareBinVersions(
                MongoRunner.getBinVersionFor(options.binVersion),
                MongoRunner.getBinVersionFor("8.1"),
            ) === -1;
        if (olderThan81) {
            delete options.setParameter.performTimeseriesCompressionIntermediateDataIntegrityCheckOnInsert;
        }

        if (tojson(options) != tojson({})) jsTest.log.info({options});

        jsTest.log.info("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");

        if (this._useBridge && (restart === undefined || !restart)) {
            // We leave the mongobridge process running when the mongod process is restarted so we
            // don't need to start a new one.
            let bridgeOptions = Object.merge(this._bridgeOptions, options.bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: this.host,
                port: this.ports[n],
                // The mongod processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: getHostName() + ":" + this._unbridgedPorts[n],
            });

            if (jsTestOptions().networkMessageCompressors) {
                bridgeOptions["networkMessageCompressors"] = jsTestOptions().networkMessageCompressors;
            }

            this.nodes[n] = new MongoBridge(bridgeOptions);
        }

        // Save this property since it may be deleted inside 'runMongod'.
        let waitForConnect = options.waitForConnect;

        // Never wait for a connection inside runMongod. We will do so below if needed.
        options.waitForConnect = false;
        let conn = MongoRunner.runMongod(options);
        if (!conn) {
            throw new Error("Failed to start node " + n);
        }

        // Make sure to call _addPath, otherwise folders won't be cleaned.
        this._addPath(conn.dbpath);

        // We don't want to persist 'waitForConnect' across node restarts.
        delete conn.fullOptions.waitForConnect;

        // Save the node object in the appropriate location.
        if (this._useBridge) {
            this._unbridgedNodes[n] = conn;
        } else {
            this.nodes[n] = conn;
            this.nodes[n].nodeId = n;
        }

        // Clean up after noReplSet to ensure it doesn't effect future restarts.
        if (options.noReplSet) {
            this.nodes[n].fullOptions.replSet = defaults.replSet;
            delete this.nodes[n].fullOptions.noReplSet;
        }

        // Wait for a connection to the node if necessary.
        if (waitForConnect === false) {
            jsTest.log.info("ReplSetTest start skip waiting for a connection to node " + n);
            return this.nodes[n];
        }

        const connection = this._waitForInitialConnection(n, waitForHealth);

        if (n == 0 && this.useAutoBootstrapProcedure && !this._hasAcquiredAutoGeneratedName) {
            const helloReply = connection.getDB("admin")._helloOrLegacyHello();
            jsTest.log.info(
                "ReplSetTest start using auto generated replSet name " +
                    helloReply.setName +
                    " instead of " +
                    this.name,
            );
            this.name = helloReply.setName;
            this._hasAcquiredAutoGeneratedName = true;
        }

        return connection;
    }

    /**
     * Restarts a db without clearing the data directory by default, and using the node(s)'s
     * original startup options by default.
     *
     * When using this method with mongobridge, be aware that mongobridge may not do a good
     * job of detecting that a node was restarted. For example, when mongobridge is being used
     * between some Node A and Node B, on restarting Node B mongobridge will not aggressively
     * close its connection with Node A, leading Node A to think the connection with Node B is
     * still healthy.
     *
     * In order not to use the original startup options, use stop() (or stopSet()) followed by
     * start() (or startSet()) without passing restart: true as part of the options.
     *
     * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
     * @param {Object} [options]
     * @param {boolean} [options.startClean] Forces clearing the data directory.
     * @param {Object} [options.auth] Object that contains the auth details for admin credentials.
     *     Should contain the fields 'user' and 'pwd'.
     */
    restart(n, options, signal, wait) {
        n = resolveToNodeId(this, n);

        // Can specify wait as third parameter, if using default signal
        if (signal == true || signal == false) {
            wait = signal;
            signal = undefined;
        }

        this.stop(n, signal, options, {forRestart: true});

        let started = this.start(n, options, true, wait);

        // We should not attempt to reauthenticate the connection if we did not wait for it
        // to be reestablished in the first place.
        const skipWaitForConnection = options && options.waitForConnect === false;
        if (jsTestOptions().keyFile && !skipWaitForConnection) {
            if (started.length) {
                // if n was an array of conns, start will return an array of connections
                for (let i = 0; i < started.length; i++) {
                    assert(jsTest.authenticate(started[i]), "Failed authentication during restart");
                }
            } else {
                assert(jsTest.authenticate(started), "Failed authentication during restart");
            }
        }
        return started;
    }

    /**
     * Step down and freeze a particular node.
     *
     * @param node A single node you wish to freeze
     */
    freeze(node) {
        node = resolveToConnection(this, node);

        assert.soon(
            () => {
                try {
                    // Ensure node is authenticated.
                    asCluster(this, node, () => {
                        // Ensure node is not primary. Ignore errors, probably means it's already
                        // secondary.
                        node.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true});
                        // Prevent node from running election. Fails if it already started an election.
                        assert.commandWorked(node.adminCommand({replSetFreeze: ReplSetTest.kForeverSecs}));
                    });
                    return true;
                } catch (e) {
                    if (
                        isNetworkError(e) ||
                        e.code === ErrorCodes.NotSecondary ||
                        e.code === ErrorCodes.NotYetInitialized
                    ) {
                        jsTestLog(`Failed to freeze node ${node.host}: ${e}`);
                        return false;
                    }

                    throw e;
                }
            },
            `Failed to run replSetFreeze cmd on ${node.host}`,
            this.timeoutMS,
        );
    }

    /**
     * Unfreeze a particular node or nodes.
     *
     * @param node is a single node or list of nodes, by id or conn
     */
    unfreeze(node) {
        node = resolveToConnection(this, node);

        // Ensure node is authenticated.
        asCluster(this, node, () => assert.commandWorked(node.adminCommand({replSetFreeze: 0})));
    }

    stopPrimary(signal, opts) {
        let primary = this.getPrimary();
        let primary_id = this.getNodeId(primary);
        return this.stop(primary_id, signal, opts);
    }

    /**
     * Stops a particular node or nodes, specified by conn or id. If we expect the node to exit with
     * a nonzero exit code, call this function and pass in allowedExitCode as a field of opts.
     *
     * If _useBridge=true, then the mongobridge process(es) corresponding to the node(s) are also
     * terminated unless forRestart=true. The mongobridge process(es) are left running across
     * restarts to ensure their configuration remains intact.
     *
     * @param {number|Mongo} n the index or connection object of the replica set member to stop.
     * @param {number} signal the signal number to use for killing
     * @param {Object} opts @see MongoRunner.stopMongod
     * @param {Object} [extraOptions={}]
     * @param {boolean} [extraOptions.forRestart=false] indicates whether stop() is being called
     * with the intent to call start() with restart=true for the same node(s) n.
     * @param {boolean} [extraOptions.waitPid=true] if true, we will wait for the process to
     * terminate after stopping it.
     */
    stop(n, signal, opts, {forRestart: forRestart = false, waitpid: waitPid = true} = {}) {
        n = resolveToNodeId(this, n);

        // Can specify wait as second parameter, if using default signal
        if (signal == true || signal == false) {
            signal = undefined;
        }

        let conn = this._useBridge ? this._unbridgedNodes[n] : this.nodes[n];

        jsTest.log.info(
            "ReplSetTest stop *** Shutting down mongod in port " +
                conn.port +
                ", wait for process termination: " +
                waitPid +
                " ***",
        );
        let ret = MongoRunner.stopMongod(conn, signal, opts, waitPid);

        // We only expect the process to have terminated if we actually called 'waitpid'.
        if (waitPid) {
            jsTest.log.info(
                "ReplSetTest stop *** Mongod in port " + conn.port + " shutdown with code (" + ret + ") ***",
            );
        }

        if (this._useBridge && !forRestart) {
            // We leave the mongobridge process running when the mongod process is being restarted.
            const bridge = this.nodes[n];
            jsTest.log.info("ReplSetTest stop *** Shutting down mongobridge on port " + bridge.port + " ***");
            const exitCode = bridge.stop(); // calls MongoBridge#stop()
            jsTest.log.info(
                "ReplSetTest stop *** mongobridge on port " + bridge.port + " exited with code (" + exitCode + ") ***",
            );
        }

        return ret;
    }

    /**
     * Performs collection validation on all nodes in the given 'ports' array in parallel.
     *
     * @private
     * @param {int[]} ports the array of mongo ports to run validation on
     */
    _validateNodes(ports) {
        // Perform collection validation on each node in parallel.
        let validators = [];
        for (let i = 0; i < ports.length; i++) {
            const validator = new Thread(async function (port) {
                const {CommandSequenceWithRetries} = await import("jstests/libs/command_sequence_with_retries.js");
                const {validateCollections} = await import("jstests/hooks/validate_collections.js");
                await import("jstests/libs/override_methods/validate_collections_on_shutdown.js");
                MongoRunner.validateCollectionsCallback(port, {CommandSequenceWithRetries, validateCollections});
            }, ports[i]);
            validators.push(validator);
            validators[i].start();
        }
        // Wait for all validators to finish.
        for (let i = 0; i < ports.length; i++) {
            validators[i].join();
        }
    }

    isReplicaSetEndpointActive() {
        _callHello(this);

        for (let node of this._liveNodes) {
            const helloRes = node.getDB("admin")._helloOrLegacyHello();
            if (!helloRes.configsvr && !this.useAutoBootstrapProcedure) {
                return false;
            }

            let shardDocs;
            try {
                shardDocs = asCluster(this, node, () => node.getCollection("config.shards").find().toArray());
            } catch (e) {
                if (e.code == ErrorCodes.NotPrimaryOrSecondary) {
                    // This node has been removed from the replica set.
                    continue;
                }
                throw e;
            }
            if (shardDocs.length != 1) {
                return false;
            }
            if (shardDocs[0]._id != "config") {
                return false;
            }
            return asCluster(this, node, () => {
                const serverStatusRes = assert.commandWorked(node.adminCommand({serverStatus: 1}));
                const olderThan73 =
                    MongoRunner.compareBinVersions(
                        MongoRunner.getBinVersionFor("7.3"),
                        MongoRunner.getBinVersionFor(serverStatusRes.version),
                    ) === 1;
                if (olderThan73) {
                    return false;
                }
                const getParameterRes = assert.commandWorked(
                    node.adminCommand({getParameter: 1, featureFlagReplicaSetEndpoint: 1}),
                );
                return getParameterRes.featureFlagReplicaSetEndpoint.value;
            });
        }
        return false;
    }

    /**
     * Kill all members of this replica set. When calling this function, we expect all live nodes to
     * exit cleanly. If we expect a node to exit with a nonzero exit code, use the stop function to
     * terminate that node before calling stopSet.
     *
     * @param {number} signal The signal number to use for killing the members
     * @param {boolean} forRestart will not cleanup data directory
     * @param {Object} opts @see MongoRunner.stopMongod
     */
    stopSet(signal, forRestart, opts = {}) {
        if (jsTestOptions().alwaysUseLogFiles) {
            if (opts.noCleanData === false) {
                throw new Error("Always using log files, but received conflicting option.");
            }

            opts.noCleanData = true;
        }

        const primary = _callHello(this);
        // TODO (SERVER-83433): Add back the test coverage for running db hash check and validation
        // on replica set that is fsync locked and has replica set endpoint enabled.
        if (
            (!opts.hasOwnProperty("skipCheckDBHashes") || !opts.hasOwnProperty("skipValidation")) &&
            primary &&
            this._liveNodes.length > 0 &&
            this.isReplicaSetEndpointActive()
        ) {
            opts = Object.assign({}, opts, {skipCheckDBHashes: true, skipValidation: true});
        }

        // Check to make sure data is the same on all nodes.
        const skipChecks = jsTest.options().skipCheckDBHashes || (opts && opts.skipCheckDBHashes);
        if (!skipChecks) {
            let startTime = new Date(); // Measure the execution time of consistency checks.
            jsTest.log.info("ReplSetTest stopSet going to run data consistency checks.");
            // To skip this check add TestData.skipCheckDBHashes = true or pass in {opts:
            // skipCheckDBHashes} Reasons to skip this test include:
            // - the primary goes down and none can be elected (so fsync lock/unlock commands fail)
            // - the replica set is in an unrecoverable inconsistent state. E.g. the replica set
            //   is partitioned.
            if (primary && this._liveNodes.length > 1) {
                // skip for sets with 1 live node
                // Auth only on live nodes because authutil.assertAuthenticate
                // refuses to log in live connections if some secondaries are down.
                jsTest.log.info("ReplSetTest stopSet checking oplogs.");
                asCluster(this, this._liveNodes, () => this.checkOplogs());
                jsTest.log.info("ReplSetTest stopSet checking preimages.");
                asCluster(this, this._liveNodes, () => this.checkPreImageCollection());
                jsTest.log.info("ReplSetTest stopSet checking change_collection(s).");
                asCluster(this, this._liveNodes, () => this.checkChangeCollection());
                jsTest.log.info("ReplSetTest stopSet checking replicated data hashes.");
                asCluster(this, this._liveNodes, () => this.checkReplicatedDataHashes());
            } else {
                jsTest.log.info(
                    "ReplSetTest stopSet skipped data consistency checks. Number of _liveNodes: " +
                        this._liveNodes.length +
                        ", _callHello response: " +
                        primary,
                );
            }
            jsTest.log.info(
                "ReplSetTest stopSet data consistency checks finished, took " +
                    (new Date() - startTime) +
                    "ms for " +
                    this.nodes.length +
                    " nodes.",
            );
        }

        let startTime = new Date(); // Measure the execution time of shutting down nodes.

        if (opts.skipValidation) {
            jsTest.log.info("ReplSetTest stopSet skipping validation before stopping nodes.");
        } else {
            jsTest.log.info("ReplSetTest stopSet validating all replica set nodes before stopping them.");
            this._validateNodes(this.ports);
        }

        // Stop all nodes without waiting for them to terminate. We can skip validation on shutdown
        // since we have already done it above (or validation was explicitly skipped).
        opts = Object.merge(opts, {skipValidation: true});
        for (let i = 0; i < this.ports.length; i++) {
            this.stop(i, signal, opts, {waitpid: false});
        }

        // Wait for all processes to terminate.
        for (let i = 0; i < this.ports.length; i++) {
            let conn = this._useBridge ? this._unbridgedNodes[i] : this.nodes[i];
            let port = parseInt(conn.name.split(":")[1]);
            jsTest.log.info("ReplSetTest stopSet waiting for mongo program on port " + port + " to stop.");
            let exitCode = waitMongoProgram(port);
            if (exitCode !== MongoRunner.EXIT_CLEAN && !opts.skipValidatingExitCode) {
                throw new Error(
                    "ReplSetTest stopSet mongo program on port " +
                        port +
                        " shut down unexpectedly with code " +
                        exitCode +
                        " when code " +
                        MongoRunner.EXIT_CLEAN +
                        " was expected.",
                );
            }
            jsTest.log.info("ReplSetTest stopSet mongo program on port " + port + " shut down with code " + exitCode);
        }

        jsTest.log.info(
            "ReplSetTest stopSet stopped all replica set nodes, took " +
                (new Date() - startTime) +
                "ms for " +
                this.ports.length +
                " nodes.",
        );

        if (forRestart) {
            jsTest.log.info("ReplSetTest stopSet returning since forRestart=true.");
            return;
        }

        if (!opts.noCleanData && this._alldbpaths) {
            jsTest.log.info("ReplSetTest stopSet deleting all dbpaths");
            for (let i = 0; i < this._alldbpaths.length; i++) {
                jsTest.log.info("ReplSetTest stopSet deleting dbpath: " + this._alldbpaths[i]);
                resetDbpath(this._alldbpaths[i]);
            }
            jsTest.log.info("ReplSetTest stopSet deleted all dbpaths");
        }

        _forgetReplSet(this.name);

        jsTest.log.info("ReplSetTest stopSet *** Shut down repl set - test worked ****");
    }

    /**
     * Returns whether or not this ReplSetTest uses mongobridge.
     */
    usesBridge() {
        return this._useBridge;
    }

    /**
     * Wait for a state indicator to go to a particular state or states.
     *
     * Note that this waits for the state as indicated by the primary node, if there is one. If not,
     * it will use the first live node.
     *
     * Cannot be used to wait for a secondary state alone. To wait for a secondary state, use the
     * function 'awaitSecondaryNodes' instead.
     *
     * @param node is a single node, by id or conn
     * @param state is a single state or list of states
     * @param timeout how long to wait for the state to be reached
     * @param reconnectNode indicates that we should reconnect to a node that stepped down
     */
    waitForState(node, state, timeout, reconnectNode) {
        assert(
            state != ReplSetTest.State.SECONDARY,
            "To wait for a secondary state, use the function 'awaitSecondaryNodes' instead.",
        );
        this._waitForIndicator(node, "state", state, timeout, reconnectNode);
    }

    /**
     * Waits until there is a primary node.
     */
    waitForPrimary(timeout) {
        let primary;
        assert.soonNoExcept(
            () => {
                return (primary = this.getPrimary());
            },
            "waiting for primary",
            timeout,
        );

        return primary;
    }

    /**
     * Returns after stable_timestamp has been advanced to at least Timestamp ts. The stable
     * timestamp is not exposed by replSetGetStatus, so we use the readConcernMajorityOpTime
     * instead, because it reflects the time of the current committed snapshot. This time is
     * determined before setting the stable timestamp, and both are done under the same mutex
     * acquisition, so readConcernMajorityOpTime >= ts indicates stable_timestamp >= ts. See
     * ReplicationCoordinatorImpl::_setStableTimestampForStorage for further
     * implementation details.
     * @param node The node to check for stable_timestamp.
     * @param ts The timestamp to compare the stable_timestamp to.
     * @param timeout How long to wait for state, defaults to global value.
     */
    waitForStableTimestampTobeAdvanced(node, ts, timeout = ReplSetTest.kDefaultTimeoutMS) {
        assert.soon(
            function () {
                jsTestLog("Waiting for stable_timestamp >= Timestamp " + ts.toStringIncomparable());
                const replSetStatus = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
                const readConcernMajorityOpTime = replSetStatus.optimes.readConcernMajorityOpTime.ts;
                return timestampCmp(readConcernMajorityOpTime, ts) >= 0;
            },
            "Timed out waiting for stable_timestamp",
            timeout,
        );
    }

    /**
     * Returns after lastStableRecoveryTimestamp has been advanced to at least Timestamp ts. Note
     * that the checkpointer thread should be running for this function to return.
     * @param node The node to check for last checkpoint time.
     * @param ts The timestamp to compare the lastStableRecoveryTimestamp to.
     * @param timeout How long to wait for state, defaults to global value.
     */
    waitForCheckpoint(node, ts, timeout = ReplSetTest.kDefaultTimeoutMS) {
        this.waitForStableTimestampTobeAdvanced(node, ts, timeout);
        assert.soon(
            function () {
                jsTestLog("Waiting for checkpoint >= Timestamp " + ts.toStringIncomparable());
                const replSetStatus = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
                const lastStableRecoveryTimestamp = replSetStatus.lastStableRecoveryTimestamp;
                return timestampCmp(lastStableRecoveryTimestamp, ts) >= 0;
            },
            "Timed out waiting for checkpoint",
            timeout,
        );
    }
}

/**
 *  Global default timeout (10 minutes).
 */
ReplSetTest.kDefaultTimeoutMS = 10 * 60 * 1000;
ReplSetTest.kDefaultRetries = 240;

/**
 *  Global default number that's effectively infinite.
 */
ReplSetTest.kForeverSecs = 24 * 60 * 60;
ReplSetTest.kForeverMillis = ReplSetTest.kForeverSecs * 1000;

/**
 * Set of states that the replica set can be in. Used for the wait functions.
 */
ReplSetTest.State = {
    PRIMARY: 1,
    SECONDARY: 2,
    RECOVERING: 3,
    // Note there is no state 4
    STARTUP_2: 5,
    UNKNOWN: 6,
    ARBITER: 7,
    DOWN: 8,
    ROLLBACK: 9,
    REMOVED: 10,
};

ReplSetTest.OpTimeType = {
    LAST_APPLIED: 1,
    LAST_DURABLE: 2,
};

/**
 * Constructor, which initializes the ReplSetTest object by starting new instances.
 */
function _constructStartNewInstances(rst, opts) {
    rst.name = opts.name || jsTest.name();
    jsTest.log.info("Starting new replica set " + rst.name);

    rst.useHostName = opts.useHostName == undefined ? true : opts.useHostName;
    rst.host = rst.useHostName ? opts.host || getHostName() : "localhost";
    rst.oplogSize = opts.oplogSize || 40;
    rst.useSeedList = opts.useSeedList || false;
    rst.keyFile = opts.keyFile;

    rst.clusterAuthMode = undefined;
    if (opts.clusterAuthMode) {
        rst.clusterAuthMode = opts.clusterAuthMode;
    }

    rst.protocolVersion = opts.protocolVersion;
    rst.waitForKeys = opts.waitForKeys;

    rst.seedRandomNumberGenerator = opts.hasOwnProperty("seedRandomNumberGenerator")
        ? opts.seedRandomNumberGenerator
        : true;
    rst.isConfigServer = opts.isConfigServer;

    rst._useBridge = opts.useBridge || false;
    if (rst._useBridge) {
        assert(
            !jsTestOptions().tlsMode,
            "useBridge cannot be true when using TLS. Add the requires_mongobridge tag to the test to ensure it will be skipped on variants that use TLS.",
        );
    }

    rst._bridgeOptions = opts.bridgeOptions || {};

    rst._causalConsistency = opts.causallyConsistent || false;

    rst._configSettings = opts.settings || false;
    rst.useAutoBootstrapProcedure = opts.useAutoBootstrapProcedure || false;
    rst._hasAcquiredAutoGeneratedName = false;

    rst.nodeOptions = {};

    let numNodes;

    if (isObject(opts.nodes)) {
        let len = 0;
        for (var i in opts.nodes) {
            // opts.nodeOptions and opts.nodes[i] may contain nested objects that have
            // the same key, e.g. setParameter. So we need to recursively merge them.
            // Object.assign and Object.merge do not merge nested objects of the same key.
            let options = (rst.nodeOptions["n" + len] = _deepObjectMerge(opts.nodeOptions, opts.nodes[i]));
            if (i.startsWith("a")) {
                options.arbiter = true;
            }

            len++;
        }

        numNodes = len;
    } else if (Array.isArray(opts.nodes)) {
        for (var i = 0; i < opts.nodes.length; i++) {
            rst.nodeOptions["n" + i] = Object.merge(opts.nodeOptions, opts.nodes[i]);
        }

        numNodes = opts.nodes.length;
    } else {
        for (var i = 0; i < opts.nodes; i++) {
            rst.nodeOptions["n" + i] = opts.nodeOptions;
        }

        numNodes = opts.nodes;
    }

    for (let i = 0; i < numNodes; i++) {
        if (rst.nodeOptions["n" + i] !== undefined && rst.nodeOptions["n" + i].clusterAuthMode == "x509") {
            rst.clusterAuthMode = "x509";
        }
    }

    if (rst._useBridge) {
        let makeAllocatePortFn = (preallocatedPorts) => {
            let idxNextNodePort = 0;

            return function () {
                if (idxNextNodePort >= preallocatedPorts.length) {
                    throw new Error(
                        "Cannot use a replica set larger than " +
                            preallocatedPorts.length +
                            " members with useBridge=true",
                    );
                }

                const nextPort = preallocatedPorts[idxNextNodePort];
                ++idxNextNodePort;
                return nextPort;
            };
        };

        rst._allocatePortForBridge = makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset));
        rst._allocatePortForNode = makeAllocatePortFn(allocatePorts(MongoBridge.kBridgeOffset));
    } else {
        rst._allocatePortForBridge = function () {
            throw new Error("Using mongobridge isn't enabled for this replica set");
        };
        rst._allocatePortForNode = allocatePort;
    }

    rst.nodes = [];

    if (rst._useBridge) {
        rst.ports = Array.from({length: numNodes}, rst._allocatePortForBridge);
        rst._unbridgedPorts = Array.from({length: numNodes}, rst._allocatePortForNode);
        rst._unbridgedNodes = [];
    } else {
        rst.ports = opts.ports || Array.from({length: numNodes}, rst._allocatePortForNode);
    }

    for (let i = 0; i < numNodes; i++) {
        const nodeOpts = rst.nodeOptions["n" + i];
        if (nodeOpts && nodeOpts.hasOwnProperty("port")) {
            if (rst._useBridge) {
                rst._unbridgedPorts[i] = nodeOpts.port;
            } else {
                rst.ports[i] = nodeOpts.port;
            }
        }
    }

    if (jsTestOptions().shellGRPC) {
        rst.grpcPorts = Array.from({length: numNodes}, rst._allocatePortForNode);
    }
}

function _newMongo(host) {
    return new Mongo(host, undefined, {gRPC: false});
}

/**
 * Constructor, which instantiates the ReplSetTest object from an existing set.
 */
function _constructFromExistingSeedNode(rst, seedNode) {
    const conn = _newMongo(seedNode);
    if (jsTest.options().keyFile) {
        rst.keyFile = jsTest.options().keyFile;
    }
    let conf = asCluster(rst, conn, () => _replSetGetConfig(conn));
    jsTest.log.info("Recreating replica set from config", {conf});

    let existingNodes = conf.members.map((member) => member.host);
    rst.ports = existingNodes.map((node) => node.split(":")[1]);
    rst.nodes = existingNodes.map((node) => {
        // Note: the seed node is required to be operational in order for the Mongo
        // shell to connect to it. In this code there is no fallback to other nodes.
        let conn = _newMongo(node);
        conn.name = conn.host;
        return conn;
    });
    rst.waitForKeys = false;
    rst.host = existingNodes[0].split(":")[0];
    rst.name = conf._id;
}

/**
 * Constructor, which instantiates the ReplSetTest object from existing nodes.
 */
function _constructFromExistingNodes(
    rst,
    {name, nodeHosts, nodeOptions, keyFile, host, waitForKeys, useAutoBootstrapProcedure, pidValue = undefined},
) {
    jsTest.log.info("Recreating replica set from existing nodes", {nodeHosts});

    rst.name = name;
    rst.ports = nodeHosts.map((node) => node.split(":")[1]);

    let i = 0;
    rst.nodes = nodeHosts.map((node) => {
        const conn = _newMongo(node);
        conn.name = conn.host;
        conn.port = node.split(":")[1];
        if (pidValue !== undefined && pidValue[i] !== undefined) {
            conn.pid = pidValue[i];
            i++;
        }
        return conn;
    });

    rst.host = host;
    rst.waitForKeys = waitForKeys;
    rst.keyFile = keyFile;
    rst.nodeOptions = nodeOptions;
    rst.useAutoBootstrapProcedure = useAutoBootstrapProcedure || false;
}

/**
 * Returns the config document reported from the specified connection.
 */
function _replSetGetConfig(conn) {
    return assert.commandWorked(conn.adminCommand({replSetGetConfig: 1})).config;
}

/**
 * Invokes the 'hello' command on each individual node and returns the current primary, or false
 * if none is found. Populates the following cached values:
 * '_primary': the current primary
 * '_secondaries': all nodes other than '_primary' (note this includes arbiters)
 * '_liveNodes': all currently reachable nodes
 */
function _callHello(rst) {
    rst._liveNodes = [];
    rst._primary = null;
    rst._secondaries = [];

    let twoPrimaries = false;
    let canAcceptWrites = false;
    // Ensure that only one node is in primary state.
    rst.nodes.forEach(function (node) {
        try {
            node.setSecondaryOk();
            let n = node.getDB("admin")._helloOrLegacyHello();
            rst._liveNodes.push(node);
            // We verify that the node has a valid config by checking if n.me exists. Then, we
            // check to see if the node is in primary state.
            if (n.me && n.me == n.primary) {
                if (rst._primary) {
                    twoPrimaries = true;
                } else {
                    rst._primary = node;
                    canAcceptWrites = n.isWritablePrimary || n.ismaster;
                }
            } else {
                rst._secondaries.push(node);
            }
        } catch (err) {
            jsTest.log.info("ReplSetTest Could not call hello/ismaster on node " + node, {error: err});
            rst._secondaries.push(node);
        }
    });
    if (twoPrimaries || !rst._primary || !canAcceptWrites) {
        return false;
    }

    return rst._primary;
}

/**
 * Attempt to connect to all nodes and returns a list of secondaries in which the connection was
 * successful.
 */
function _determineLiveSecondaries(rst) {
    _callHello(rst);
    return rst._secondaries.filter(function (n) {
        return rst._liveNodes.indexOf(n) !== -1;
    });
}

/**
 * For all unauthenticated connections passed in, authenticates them with the '__system' user.
 * If a connection is already authenticated, we will skip authentication for that connection and
 * assume that it already has the correct privileges. It is up to the caller of this function to
 * ensure that the connection is appropriately authenticated.
 */
function asCluster(rst, conn, fn, keyFileParam = undefined) {
    let connArray = conn;
    if (conn.length == null) connArray = [conn];

    let rst_keyfile = null;
    if (rst !== undefined && rst !== null) {
        rst_keyfile = rst.keyFile;
    }

    const unauthenticatedConns = connArray.filter((connection) => {
        const connStatus = connection.adminCommand({connectionStatus: 1, showPrivileges: true});
        const connIsAuthenticated = connStatus.authInfo.authenticatedUsers.length > 0;
        return !connIsAuthenticated;
    });

    const connOptions = connArray[0].fullOptions || {};
    const authMode = connOptions.clusterAuthMode || connArray[0].clusterAuthMode || jsTest.options().clusterAuthMode;

    keyFileParam = keyFileParam || connOptions.keyFile || rst_keyfile;
    let needsAuth =
        (keyFileParam || authMode === "x509" || authMode === "sendX509" || authMode === "sendKeyFile") &&
        unauthenticatedConns.length > 0;

    // There are few cases where we do not auth
    // 1. When transitioning to auth
    // 2. When cluster is running in x509 but shell was not started with TLS (i.e. sslSpecial
    // suite)
    if (needsAuth && (connOptions.transitionToAuth !== undefined || (authMode === "x509" && !connArray[0].isTLS()))) {
        needsAuth = false;
    }

    if (needsAuth) {
        return authutil.asCluster(unauthenticatedConns, keyFileParam, fn);
    } else {
        return fn();
    }
}

/**
 * Returns 'true' if the "conn" has been configured to run without journaling enabled.
 */
function _isRunningWithoutJournaling(rst, conn) {
    let result = asCluster(rst, conn, function () {
        // Persistent storage engines (WT) can only run with journal enabled.
        let serverStatus = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
        if (serverStatus.storageEngine.hasOwnProperty("persistent")) {
            if (serverStatus.storageEngine.persistent) {
                return false;
            }
        }
        return true;
    });
    return result;
}

/**
 * Helper functions for setting/clearing a failpoint.
 */
function setFailPoint(node, failpoint, data = {}) {
    jsTest.log.info("Setting fail point " + failpoint);
    assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: "alwaysOn", data: data}));
}

function clearFailPoint(node, failpoint) {
    jsTest.log.info("Clearing fail point " + failpoint);
    assert.commandWorked(node.adminCommand({configureFailPoint: failpoint, mode: "off"}));
}

/**
 * Returns true if the OpTime is empty, else false.
 *
 * Empty OpTime Formats:
 *   PV0: Timestamp(0,0)
 *   PV1: {ts: Timestamp(0,0), t: NumberLong(-1)}
 */
function _isEmptyOpTime(opTime) {
    if (!opTime.hasOwnProperty("ts") || !opTime.hasOwnProperty("t")) {
        return opTime.getTime() == 0 && opTime.getInc() == 0;
    }
    return opTime.ts.getTime() == 0 && opTime.ts.getInc() == 0 && opTime.t == -1;
}

/**
 * Returns the OpTime for the specified host by issuing replSetGetStatus.
 */
function _getLastOpTime(rst, conn) {
    let replSetStatus = asCluster(rst, conn, () =>
        assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1})),
    );
    let connStatus = replSetStatus.members.filter((m) => m.self)[0];
    let opTime = connStatus.optime;
    if (_isEmptyOpTime(opTime)) {
        throw new Error("last OpTime is empty -- connection: " + conn);
    }
    return opTime;
}

/**
 * Returns the last durable OpTime for the host if running with journaling.
 * Returns the last applied OpTime otherwise.
 */
function _getDurableOpTime(rst, conn) {
    let replSetStatus = asCluster(rst, conn, () =>
        assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1})),
    );

    let opTimeType = "durableOpTime";
    if (_isRunningWithoutJournaling(rst, conn)) {
        opTimeType = "appliedOpTime";
    }
    let opTime = replSetStatus.optimes[opTimeType];
    if (_isEmptyOpTime(opTime)) {
        throw new Error("last durable OpTime is empty -- connection: " + conn);
    }
    return opTime;
}

/*
 * Returns true if the node can be elected primary of a replica set.
 */
function _isElectable(node) {
    return !node.arbiterOnly && (node.priority === undefined || node.priority != 0);
}

function isNodeArbiter(node) {
    return node.getDB("admin")._helloOrLegacyHello().arbiterOnly;
}

function replSetCommandWithRetry(primary, cmd) {
    jsTest.log.info("Running command with retry", {cmd});
    const cmdName = Object.keys(cmd)[0];
    const errorMsg = `${cmdName} during initiate failed`;
    assert.retry(
        () => {
            const result = assert.commandWorkedOrFailedWithCode(
                primary.runCommand(cmd),
                [
                    ErrorCodes.NodeNotFound,
                    ErrorCodes.NewReplicaSetConfigurationIncompatible,
                    ErrorCodes.InterruptedDueToReplStateChange,
                    ErrorCodes.ConfigurationInProgress,
                    ErrorCodes.CurrentConfigNotCommittedYet,
                    ErrorCodes.NotWritablePrimary,
                ],
                errorMsg,
            );
            return result.ok;
        },
        errorMsg,
        3,
        5 * 1000,
    );
}

// TODO(SERVER-14017): Remove this extra sub-shell in favor of a cleaner authentication
// solution.
function runFnWithAuthOnPrimary(rst, fn, fnName) {
    const primary = rst.getPrimary();
    const primaryId = "n" + rst.getNodeId(primary);
    const primaryOptions = rst.nodeOptions[primaryId] || {};
    const options = Object.keys(primaryOptions).length !== 0 || !rst.startOptions ? primaryOptions : rst.startOptions;
    const authMode = options.clusterAuthMode;
    if (authMode === "x509") {
        jsTest.log.info(fnName + ": authenticating on separate shell with x509 for " + rst.name);
        const caFile = options.sslCAFile ? options.sslCAFile : options.tlsCAFile;
        const keyFile = options.sslPEMKeyFile ? options.sslPEMKeyFile : options.tlsCertificateKeyFile;
        const subShellArgs = [
            "mongo",
            "--ssl",
            "--sslCAFile=" + caFile,
            "--sslPEMKeyFile=" + keyFile,
            "--sslAllowInvalidHostnames",
            "--authenticationDatabase=$external",
            "--authenticationMechanism=MONGODB-X509",
            primary.host,
            "--eval",
            `import {ReplSetTest} from "jstests/libs/replsettest.js"; (${fn.toString()})();`,
        ];

        const retVal = _runMongoProgram(...subShellArgs);
        assert.eq(retVal, 0, "mongo shell did not succeed with exit code 0");
    } else {
        jsTest.log.info(fnName + ": authenticating with authMode '" + authMode + "' for " + rst.name);
        asCluster(rst, primary, fn, primaryOptions.keyFile);
    }
}

const ReverseReader = function (mongo, coll, query) {
    this.kCappedPositionLostSentinel = Object.create(null);

    this._safelyPerformCursorOperation = function (name, operation, onCappedPositionLost) {
        if (!this.cursor) {
            throw new Error("ReverseReader is not open!");
        }

        if (this._cursorExhausted) {
            return onCappedPositionLost;
        }

        try {
            return operation(this.cursor);
        } catch (err) {
            jsTest.log.info("Error: " + name + " threw '" + err.message + "' on " + this.mongo.host);
            // Occasionally, the capped collection will get truncated while we are iterating
            // over it. Since we are iterating over the collection in reverse, getting a
            // truncated item means we've reached the end of the list, so return false.
            if (err.code === ErrorCodes.CappedPositionLost) {
                this.cursor.close();
                this._cursorExhausted = true;
                return onCappedPositionLost;
            }

            throw err;
        }
    };

    this.next = function () {
        return this._safelyPerformCursorOperation(
            "next",
            function (cursor) {
                return cursor.next();
            },
            this.kCappedPositionLostSentinel,
        );
    };

    this.hasNext = function () {
        return this._safelyPerformCursorOperation(
            "hasNext",
            function (cursor) {
                return cursor.hasNext();
            },
            false,
        );
    };

    this.query = function () {
        // Set the cursor to read backwards, from last to first. We also set the cursor not
        // to time out since it may take a while to process each batch and a test may have
        // changed "cursorTimeoutMillis" to a short time period.
        // TODO SERVER-75496 remove the batchSize once the the following issue is fixed: The
        // find{...} will always run with apiStrict:false, however getMore may run with
        // apiStrict: true on specific suites. Use a big batch size to prevent getMore from
        // running.
        this._cursorExhausted = false;
        this.cursor = coll.find(query).sort({$natural: -1}).noCursorTimeout().readConcern("local").batchSize(200);
    };

    this.getFirstDoc = function () {
        return coll.find(query).sort({$natural: 1}).readConcern("local").limit(-1).next();
    };

    this.cursor = null;
    this._cursorExhausted = true;
    this.mongo = mongo;
};

/**
 * Check oplogs on all nodes, by reading from the last time. Since the oplog is a capped
 * collection, each node may not contain the same number of entries and stop if the cursor
 * is exhausted on any node being checked.
 *
 * `secondaries` must be the last argument since in checkReplicaSet we explicitly append the live
 * secondaries to the end of the parameter list after ensuring that the current primary is excluded.
 */
function checkOplogs(rst, msgPrefix = "checkOplogs", secondaries) {
    secondaries = secondaries || rst._secondaries;

    function assertOplogEntriesEq(oplogEntry0, oplogEntry1, reader0, reader1, prevOplogEntry) {
        if (!bsonBinaryEqual(oplogEntry0, oplogEntry1)) {
            const query = prevOplogEntry ? {ts: {$lte: prevOplogEntry.ts}} : {};
            rst.nodes.forEach((node) => rst.dumpOplog(node, query, 100));
            const log =
                msgPrefix +
                ", non-matching oplog entries for the following nodes: \n" +
                reader0.mongo.host +
                ": " +
                tojsononeline(oplogEntry0) +
                "\n" +
                reader1.mongo.host +
                ": " +
                tojsononeline(oplogEntry1);
            assert(false, log);
        }
    }

    jsTest.log.info("checkOplogs starting oplog checks.");
    jsTest.log.info("checkOplogs waiting for secondaries to be ready.");
    rst.awaitSecondaryNodes(rst.timeoutMS, secondaries);
    if (secondaries.length >= 1) {
        let readers = [];
        let smallestTS = new Timestamp(Math.pow(2, 32) - 1, Math.pow(2, 32) - 1);
        const nodes = rst.nodes;
        let firstReaderIndex;
        for (let i = 0; i < nodes.length; i++) {
            const node = nodes[i];

            if (rst._primary !== node && !secondaries.includes(node)) {
                jsTest.log.info("checkOplogs skipping oplog of node: " + node.host);
                continue;
            }

            // Arbiters have no documents in the oplog.
            if (isNodeArbiter(node)) {
                jsTestLog("checkOplogs skipping oplog of arbiter: " + node.host);
                continue;
            }

            jsTest.log.info("checkOplogs going to check oplog of node: " + node.host);
            readers[i] = new ReverseReader(node, node.getDB("local")[kOplogName], {ts: {$gte: new Timestamp()}});
            const currTS = readers[i].getFirstDoc().ts;
            // Find the reader which has the smallestTS. This reader should have the most
            // number of documents in the oplog.
            if (timestampCmp(currTS, smallestTS) < 0) {
                smallestTS = currTS;
                firstReaderIndex = i;
            }
            // Start all oplogReaders at their last document.
            readers[i].query();
        }

        // Read from the reader which has the most oplog entries.
        // Note, we read the oplog backwards from last to first.
        const firstReader = readers[firstReaderIndex];
        let prevOplogEntry;
        assert(firstReader.hasNext(), "oplog is empty while checkOplogs is called");
        // Track the number of bytes we are reading as we check the oplog. We use this to avoid
        // out-of-memory issues by calling to garbage collect whenever the memory footprint is
        // large.
        let bytesSinceGC = 0;
        while (firstReader.hasNext()) {
            const oplogEntry = firstReader.next();
            bytesSinceGC += Object.bsonsize(oplogEntry);
            if (oplogEntry === firstReader.kCappedPositionLostSentinel) {
                // When using legacy OP_QUERY/OP_GET_MORE reads against mongos, it is
                // possible for hasNext() to return true but for next() to throw an exception.
                break;
            }

            for (let i = 0; i < nodes.length; i++) {
                // Skip reading from this reader if the index is the same as firstReader or
                // the cursor is exhausted.
                if (i === firstReaderIndex || !(readers[i] && readers[i].hasNext())) {
                    continue;
                }

                const otherOplogEntry = readers[i].next();
                bytesSinceGC += Object.bsonsize(otherOplogEntry);
                if (otherOplogEntry && otherOplogEntry !== readers[i].kCappedPositionLostSentinel) {
                    assertOplogEntriesEq.call(
                        this,
                        oplogEntry,
                        otherOplogEntry,
                        firstReader,
                        readers[i],
                        prevOplogEntry,
                    );
                }
            }
            // Garbage collect every 10MB.
            if (bytesSinceGC > 10 * 1024 * 1024) {
                gc();
                bytesSinceGC = 0;
            }
            prevOplogEntry = oplogEntry;
        }
    }
    jsTest.log.info("checkOplogs oplog checks complete.");
}

function getPreImageReaders(msgPrefix, rst, secondaries, nsUUID) {
    const readers = [];
    const nodes = rst.nodes;
    for (let i = 0; i < nodes.length; i++) {
        const node = nodes[i];

        if (rst._primary !== node && !secondaries.includes(node)) {
            jsTest.log.info(
                `${msgPrefix} -- skipping preimages of node as it's not in our list of ` + `secondaries: ${node.host}`,
            );
            continue;
        }

        // Arbiters have no documents in the oplog and thus don't have preimages
        // content.
        if (isNodeArbiter(node)) {
            jsTestLog(`${msgPrefix} -- skipping preimages of arbiter node: ${node.host}`);
            continue;
        }

        jsTest.log.info(`${msgPrefix} -- going to check preimages of ${nsUUID} of node: ${node.host}`);
        readers[i] = new ReverseReader(node, node.getDB("config")["system.preimages"], {"_id.nsUUID": nsUUID});
        // Start all reverseReaders at their last document for the collection.
        readers[i].query();
    }

    return readers;
}

function dumpPreImagesCollection(msgPrefix, node, nsUUID, timestamp, limit) {
    const beforeCursor = node
        .getDB("config")
        ["system.preimages"].find({"_id.nsUUID": nsUUID, "_id.ts": {"$lt": timestamp}})
        .sort({$natural: -1})
        .noCursorTimeout()
        .readConcern("local")
        .limit(limit / 2); // We print up to half of the limit in the before part so that
    // the timestamp is centered.
    const beforeEntries = beforeCursor.toArray().reverse();

    let log = `${msgPrefix} -- Dumping a window of ${limit} entries for preimages of collection ${
        nsUUID
    } from host ${node.host} centered around timestamp ${timestamp.toStringIncomparable()}`;

    beforeEntries.forEach((entry) => {
        log += "\n" + tojsononeline(entry);
    });

    const remainingWindow = limit - beforeEntries.length;
    const cursor = node
        .getDB("config")
        ["system.preimages"].find({"_id.nsUUID": nsUUID, "_id.ts": {"$gte": timestamp}})
        .sort({$natural: 1})
        .noCursorTimeout()
        .readConcern("local")
        .limit(remainingWindow);
    cursor.forEach((entry) => {
        log += "\n" + tojsononeline(entry);
    });

    jsTestLog(log);
}

/**
 * Check preimages on all nodes, by reading reading from the last time. Since the preimage may
 * or may not be maintained independently, each node may not contain the same number of entries
 * and stop if the cursor is exhausted on any node being checked.
 *
 * `secondaries` must be the last argument since in checkReplicaSet we explicitly append the live
 * secondaries to the end of the parameter list after ensuring that the current primary is excluded.
 */
function checkPreImageCollection(rst, msgPrefix = "checkPreImageCollection", secondaries) {
    secondaries = secondaries || rst._secondaries;

    const originalPreferences = [];

    jsTest.log.info(`${msgPrefix} -- starting preimage checks.`);
    jsTest.log.info(`${msgPrefix} -- waiting for secondaries to be ready.`);
    rst.awaitSecondaryNodes(rst.timeoutMS, secondaries);
    if (secondaries.length >= 1) {
        let collectionsWithPreimages = {};
        const nodes = rst.nodes;
        for (let i = 0; i < nodes.length; i++) {
            const node = nodes[i];

            if (rst._primary !== node && !secondaries.includes(node)) {
                jsTest.log.info(
                    `${msgPrefix} -- skipping preimages of node as it's not in our list of ` +
                        `secondaries: ${node.host}`,
                );
                continue;
            }

            // Arbiters have no documents in the oplog and thus don't have preimages content.
            if (isNodeArbiter(node)) {
                jsTestLog(`${msgPrefix} -- skipping preimages of arbiter node: ${node.host}`);
                continue;
            }

            const preImageColl = node.getDB("config")["system.preimages"];
            // Reset connection preferences in case the test has modified them. We'll restore
            // them back to what they were originally in the end.
            originalPreferences[i] = {
                secondaryOk: preImageColl.getMongo().getSecondaryOk(),
                readPref: preImageColl.getMongo().getReadPref(),
            };

            preImageColl.getMongo().setSecondaryOk(true);
            preImageColl.getMongo().setReadPref(rst._primary === node ? "primary" : "secondary");

            // Find all collections participating in pre-images.
            const collectionsInPreimages = preImageColl.aggregate([{$group: {_id: "$_id.nsUUID"}}]).toArray();
            for (const collTs of collectionsInPreimages) {
                collectionsWithPreimages[collTs._id] = collTs._id;
            }
        }
        for (const nsUUID of Object.values(collectionsWithPreimages)) {
            const readers = getPreImageReaders(msgPrefix, rst, secondaries, nsUUID);

            while (true) {
                let preImageEntryToCompare = undefined;
                let originNode = undefined;
                for (const reader of readers) {
                    if (reader.hasNext()) {
                        const preImageEntry = reader.next();
                        if (preImageEntryToCompare === undefined) {
                            preImageEntryToCompare = preImageEntry;
                            originNode = reader.mongo;
                        } else {
                            if (!bsonBinaryEqual(preImageEntryToCompare, preImageEntry)) {
                                // TODO SERVER-55756: Investigate if we can remove this since
                                // we'll have the data files present in case this fails with
                                // PeriodicKillSecondaries.
                                jsTest.log.info(`${msgPrefix} -- preimage inconsistency detected.`, {
                                    originNode: {
                                        host: originNode.host,
                                        preImageEntry: preImageEntryToCompare,
                                    },
                                    currentNode: {
                                        host: originNode.host,
                                        preImageEntry: preImageEntryToCompare,
                                    },
                                });
                                jsTest.log.info("Printing previous entries:");
                                dumpPreImagesCollection(
                                    msgPrefix,
                                    originNode,
                                    nsUUID,
                                    preImageEntryToCompare._id.ts,
                                    100,
                                );
                                dumpPreImagesCollection(msgPrefix, reader.mongo, nsUUID, preImageEntry._id.ts, 100);
                                const log =
                                    `${msgPrefix} -- non-matching preimage entries:\n` +
                                    `${originNode.host} -> ${tojsononeline(preImageEntryToCompare)}\n` +
                                    `${reader.mongo.host} -> ${tojsononeline(preImageEntry)}`;
                                assert(false, log);
                            }
                        }
                    }
                }
                if (preImageEntryToCompare === undefined) {
                    break;
                }
            }
        }
    }
    jsTest.log.info(`${msgPrefix} -- preimages check complete.`);

    // Restore original read preferences used by the connection.
    for (const idx in originalPreferences) {
        const node = rst.nodes[idx];
        const conn = node.getDB("config").getMongo();
        conn.setSecondaryOk(originalPreferences[idx].secondaryOk);
        conn.setReadPref(originalPreferences[idx].readPref);
    }
}

function dumpChangeCollection(node, tenantDatabaseName, timestamp, limit, msgPrefix) {
    const beforeCursor = node
        .getDB(tenantDatabaseName)
        ["system.change_collection"].find({"_id": {"$lt": timestamp}})
        .sort({$natural: -1})
        .noCursorTimeout()
        .readConcern("local")
        .limit(limit / 2); // We print up to half of the limit in the before part so that
    // the timestamp is centered.
    const beforeEntries = beforeCursor.toArray().reverse();

    let log = `${msgPrefix} -- Dumping a window of ${limit} entries for ${
        tenantDatabaseName
    }.system.change_collection from host ${node.host} centered around ${timestamp.toStringIncomparable()}`;

    beforeEntries.forEach((entry) => {
        log += "\n" + tojsononeline(entry);
    });

    const remainingWindow = limit - beforeEntries.length;
    const cursor = node
        .getDB(tenantDatabaseName)
        ["system.change_collection"].find({"_id": {"$gte": timestamp}})
        .sort({$natural: 1})
        .noCursorTimeout()
        .readConcern("local")
        .limit(remainingWindow);
    cursor.forEach((entry) => {
        log += "\n" + tojsononeline(entry);
    });

    jsTestLog(log);
}

function checkTenantChangeCollection(rst, secondaries, db, msgPrefix = "checkTenantChangeCollection") {
    const tenantDatabaseName = db.name;
    jsTest.log.info(`${msgPrefix} -- starting check on ${db.tenantId} ${tenantDatabaseName}.system.change_collection`);

    // Prepare reverse read from the primary and specified secondaries.
    const nodes = [rst.getPrimary(), ...secondaries];
    let reverseReaders = nodes.map((node) => {
        let reader = new ReverseReader(node, node.getDB(tenantDatabaseName)["system.change_collection"]);
        // Start all reverseReaders at their last document for the collection.
        reader.query();
        return reader;
    });

    let inspectedEntryCount = 0;
    while (true) {
        const entryAndNodeSet = reverseReaders.map((reader) => {
            if (reader.hasNext()) {
                return {entry: reader.next(), node: reader.mongo};
            }
            return undefined;
        });
        let baselineEntryAndNode = undefined;

        entryAndNodeSet.forEach((entryAndNode) => {
            if (entryAndNode === undefined) {
                return;
            }

            if (baselineEntryAndNode === undefined) {
                inspectedEntryCount++;
                baselineEntryAndNode = entryAndNode;
                return;
            }
            if (!bsonBinaryEqual(baselineEntryAndNode.entry, entryAndNode.entry)) {
                jsTest.log.info(
                    `${msgPrefix} -- inconsistency detected in ${tenantDatabaseName}.system.change_collection`,
                    {
                        baselineNode: {
                            host: baselineEntryAndNode.node.host,
                            entry: baselineEntryAndNode.entry,
                        },
                        currentNode: {host: entryAndNode.node.host, entry: entryAndNode.entry},
                    },
                );

                dumpChangeCollection(
                    baselineEntryAndNode.node,
                    tenantDatabaseName,
                    baselineEntryAndNode.entry._id,
                    100,
                    msgPrefix,
                );
                dumpChangeCollection(entryAndNode.node, tenantDatabaseName, entryAndNode.entry._id, 100, msgPrefix);
                assert(false, `Found inconsistency in '${tenantDatabaseName}.system.change_collection'`);
            }
        });

        if (baselineEntryAndNode === undefined) {
            break;
        }
    }
    jsTest.log.info(
        `${msgPrefix} -- finished check on ${tenantDatabaseName}.system.change_collection, inspected ${
            inspectedEntryCount
        } unique entries`,
    );
}

/**
 * Check change_collection for all tenants on all nodes, by doing a reverse scan. This check
 * accounts for the fact that each node might independently truncate the change collection, and
 * not contain the same number of entries.
 *
 * `secondaries` must be the last argument since in checkReplicaSet we explicitly append the live
 * secondaries to the end of the parameter list after ensuring that the current primary is excluded.
 */
function checkChangeCollection(rst, msgPrefix = "checkChangeCollection", secondaries) {
    secondaries = secondaries || rst._secondaries;
    secondaries = secondaries.filter((node) => !isNodeArbiter(node));

    if (secondaries.length == 0) {
        jsTest.log.info(`${msgPrefix} -- no data bearing secondaries specified, nothing to do.`);
        return;
    }

    jsTest.log.info(`${msgPrefix} -- starting change_collection checks.`);
    jsTest.log.info(`${msgPrefix} -- waiting for secondaries to be ready.`);
    rst.awaitSecondaryNodes(rst.timeoutMS, secondaries);

    // Get all change_collections for all tenants.
    let dbs = rst.getPrimary().getDBs();
    dbs = dbs.databases.filter((db) => db.name.endsWith("_config") || db.name == "config");
    dbs.forEach((db) => {
        if (db.tenantId) {
            try {
                const token = _createTenantToken({tenant: db.tenantId});
                rst.nodes.forEach((node) => node._setSecurityToken(token));
                checkTenantChangeCollection(rst, secondaries, db);
            } finally {
                rst.nodes.forEach((node) => node._setSecurityToken(undefined));
            }
        } else {
            checkTenantChangeCollection(rst, secondaries, db);
        }
    });
    jsTest.log.info(`${msgPrefix} -- change_collection check complete.`);
}

/**
 * Recursively merge the target and source object.
 */
function _deepObjectMerge(target, source) {
    if (!(target instanceof Object)) {
        return source === undefined || source === null ? target : source;
    }

    if (!(source instanceof Object)) {
        return target;
    }

    let res = Object.assign({}, target);
    Object.keys(source).forEach((k) => {
        res[k] = _deepObjectMerge(target[k], source[k]);
    });

    return res;
}

/**
 * Resolves a parameter into a direct connection to a replica set node.
 *
 * @param {ReplSetTest} rst The ReplicaSetTest we are resolving the parameter for.
 * @param {number|Mongo} nodeIdOrConnection The parameter we want to resolve into a connection.
 * @returns {Mongo}
 */
function resolveToConnection(rst, nodeIdOrConnection) {
    if (nodeIdOrConnection.getDB) {
        return nodeIdOrConnection;
    }

    assert(rst.nodes.hasOwnProperty(nodeIdOrConnection), `${nodeIdOrConnection} not found in own nodes`);
    return rst.nodes[nodeIdOrConnection];
}

/**
 * Resolves a parameter into a replica set node id.
 *
 * @param {ReplicaSetTest} rst The ReplicaSetTest we are resolving the parameter for.
 * @param {number|Mongo} nodeIdOrConnection The parameter we want to resolve into a node id.
 * @returns {number}
 */
function resolveToNodeId(rst, nodeIdOrConnection) {
    if (nodeIdOrConnection.getDB) {
        return rst.getNodeId(nodeIdOrConnection);
    }

    assert(Number.isInteger(nodeIdOrConnection), `node must be an integer, not ${nodeIdOrConnection}`);
    return nodeIdOrConnection;
}
