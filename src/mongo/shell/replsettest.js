/**
 * Sets up a replica set. To make the set running, call {@link #startSet},
 * followed by {@link #initiate} (and optionally,
 * {@link #awaitSecondaryNodes} to block till the  set is fully operational).
 * Note that some of the replica start up parameters are not passed here,
 * but to the #startSet method.
 *
 * @param {Object|string} opts If this value is a string, it specifies the connection string for
 *      a MongoD host to be used for recreating a ReplSetTest from. Otherwise, if it is an object,
 *      it must have the following contents:
 *
 *   {
 *     name {string}: name of this replica set. Default: 'testReplSet'
 *     host {string}: name of the host machine. Hostname will be used
 *        if not specified.
 *     useHostName {boolean}: if true, use hostname of machine,
 *        otherwise use localhost
 *     nodes {number|Object|Array.<Object>}: number of replicas. Default: 0.
 *        Can also be an Object (or Array).
 *        Format for Object:
 *          {
 *            <any string>: replica member option Object. @see MongoRunner.runMongod
 *            <any string2>: and so on...
 *          }
 *          If object has a special "rsConfig" field then those options will be used for each
 *          replica set member config options when used to initialize the replica set, or
 *          building the config with getReplSetConfig()
 *
 *        Format for Array:
 *           An array of replica member option Object. @see MongoRunner.runMongod
 *
 *        Note: For both formats, a special boolean property 'arbiter' can be
 *          specified to denote a member is an arbiter.
 *
 *        Note: A special "bridgeOptions" property can be specified in both the object and array
 *           formats to configure the options for the mongobridge corresponding to that node. These
 *           options are merged with the opts.bridgeOptions options, where the node-specific options
 *           take precedence.
 *
 *     nodeOptions {Object}: Command-line options to apply to all nodes in the replica set.
 *        Format for Object:
 *          { cmdline-param-with-no-arg : "",
 *            param-with-arg : arg }
 *        This turns into "mongod --cmdline-param-with-no-arg --param-with-arg arg"
 *
 *     causallyConsistent {boolean}: Specifies whether the connections to the replica set nodes
 *        should be created with the 'causal consistency' flag enabled, which means they will gossip
 *        the cluster time and add readConcern afterClusterTime where applicable. Defaults to false.
 *
 *     oplogSize {number}: Default: 40
 *     useSeedList {boolean}: Use the connection string format of this set
 *        as the replica set name (overrides the name property). Default: false
 *     keyFile {string}
 *     protocolVersion {number}: protocol version of replset used by the replset initiation.
 *
 *     useBridge {boolean}: If true, then a mongobridge process is started for each node in the
 *        replica set. Both the replica set configuration and the connections returned by startSet()
 *        will be references to the proxied connections. Defaults to false.
 *
 *     bridgeOptions {Object}: Options to apply to all mongobridge processes. Defaults to {}.
 *
 *     settings {object}: Setting used in the replica set config document.
 *        Example:
 *              settings: { chainingAllowed: false, ... }
 *   }
 *
 * Member variables:
 *  nodes {Array.<Mongo>} - connection to replica set members
 */

var ReplSetTest = function(opts) {
    'use strict';

    if (!(this instanceof ReplSetTest)) {
        return new ReplSetTest(opts);
    }

    // Capture the 'this' reference
    var self = this;

    // Replica set health states
    var Health = {UP: 1, DOWN: 0};

    var _alldbpaths;
    var _configSettings;

    // mongobridge related variables. Only available if the bridge option is selected.
    var _useBridge;
    var _bridgeOptions;
    var _unbridgedPorts;
    var _unbridgedNodes;

    var _causalConsistency;

    // Some code still references kDefaultTimeoutMS as a (non-static) member variable, so make sure
    // it's still accessible that way.
    this.kDefaultTimeoutMS = ReplSetTest.kDefaultTimeoutMS;
    var oplogName = 'oplog.rs';

    // Publicly exposed variables

    /**
     * Populates a reference to all reachable nodes.
     */
    function _clearLiveNodes() {
        self.liveNodes = {master: null, slaves: []};
    }

    /**
     * Returns the config document reported from the specified connection.
     */
    function _replSetGetConfig(conn) {
        return assert.commandWorked(conn.adminCommand({replSetGetConfig: 1})).config;
    }

    /**
     * Invokes the 'ismaster' command on each individual node and returns whether the node is the
     * current RS master.
     */
    function _callIsMaster() {
        _clearLiveNodes();

        var twoPrimaries = false;
        self.nodes.forEach(function(node) {
            try {
                node.setSlaveOk();
                var n = node.getDB('admin').runCommand({ismaster: 1});
                if (n.ismaster == true) {
                    if (self.liveNodes.master) {
                        twoPrimaries = true;
                    } else {
                        self.liveNodes.master = node;
                    }
                } else {
                    self.liveNodes.slaves.push(node);
                }
            } catch (err) {
                print("ReplSetTest Could not call ismaster on node " + node + ": " + tojson(err));
            }
        });
        if (twoPrimaries) {
            return false;
        }

        return self.liveNodes.master || false;
    }

    function asCluster(conn, fn) {
        if (self.keyFile) {
            return authutil.asCluster(conn, self.keyFile, fn);
        } else {
            return fn();
        }
    }

    /**
     * Returns 'true' if the "conn" has been configured to run without journaling enabled.
     */
    function _isRunningWithoutJournaling(conn) {
        var result = asCluster(conn, function() {
            var serverStatus = assert.commandWorked(conn.adminCommand({serverStatus: 1}));
            if (serverStatus.storageEngine.hasOwnProperty('persistent')) {
                if (!serverStatus.storageEngine.persistent) {
                    return true;
                }
            } else if (serverStatus.storageEngine.name == 'inMemory' ||
                       serverStatus.storageEngine.name == 'ephemeralForTest') {
                return true;
            }
            var cmdLineOpts = assert.commandWorked(conn.adminCommand({getCmdLineOpts: 1}));
            var getWithDefault = function(dict, key, dflt) {
                if (dict[key] === undefined)
                    return dflt;
                return dict[key];
            };
            return !getWithDefault(
                getWithDefault(getWithDefault(cmdLineOpts.parsed, "storage", {}), "journal", {}),
                "enabled",
                true);
        });
        return result;
    }

    /**
     * Wait for a rs indicator to go to a particular state or states.
     *
     * @param node is a single node or list of nodes, by id or conn
     * @param states is a single state or list of states
     * @param ind is the indicator specified
     * @param timeout how long to wait for the state to be reached
     */
    function _waitForIndicator(node, states, ind, timeout) {
        if (node.length) {
            var nodes = node;
            for (var i = 0; i < nodes.length; i++) {
                if (states.length)
                    _waitForIndicator(nodes[i], states[i], ind, timeout);
                else
                    _waitForIndicator(nodes[i], states, ind, timeout);
            }

            return;
        }

        timeout = timeout || self.kDefaultTimeoutMS;

        if (!node.getDB) {
            node = self.nodes[node];
        }

        if (!states.length) {
            states = [states];
        }

        print("ReplSetTest waitForIndicator " + ind + " on " + node);
        printjson(states);
        print("ReplSetTest waitForIndicator from node " + node);

        var lastTime = null;
        var currTime = new Date().getTime();
        var status;

        assert.soon(function() {
            try {
                var conn = _callIsMaster();
                if (!conn) {
                    conn = self.liveNodes.slaves[0];
                }

                // Try again to load connection
                if (!conn)
                    return false;

                asCluster(conn, function() {
                    status = conn.getDB('admin').runCommand({replSetGetStatus: 1});
                });
            } catch (ex) {
                print("ReplSetTest waitForIndicator could not get status: " + tojson(ex));
                return false;
            }

            var printStatus = false;
            if (lastTime == null || (currTime = new Date().getTime()) - (1000 * 5) > lastTime) {
                if (lastTime == null) {
                    print("ReplSetTest waitForIndicator Initial status (timeout : " + timeout +
                          ") :");
                }

                printjson(status);
                lastTime = new Date().getTime();
                printStatus = true;
            }

            if (typeof status.members == 'undefined') {
                return false;
            }

            for (var i = 0; i < status.members.length; i++) {
                if (printStatus) {
                    print("Status for : " + status.members[i].name + ", checking " + node.host +
                          "/" + node.name);
                }

                if (status.members[i].name == node.host || status.members[i].name == node.name) {
                    for (var j = 0; j < states.length; j++) {
                        if (printStatus) {
                            print("Status -- " + " current state: " + status.members[i][ind] +
                                  ",  target state : " + states[j]);
                        }

                        if (typeof(states[j]) != "number") {
                            throw new Error("State was not an number -- type:" + typeof(states[j]) +
                                            ", value:" + states[j]);
                        }
                        if (status.members[i][ind] == states[j]) {
                            return true;
                        }
                    }
                }
            }

            return false;

        }, "waiting for state indicator " + ind + " for " + timeout + "ms", timeout);

        print("ReplSetTest waitForIndicator final status:");
        printjson(status);
    }

    /**
     * Wait for a health indicator to go to a particular state or states.
     *
     * @param node is a single node or list of nodes, by id or conn
     * @param state is a single state or list of states. ReplSetTest.Health.DOWN can
     *     only be used in cases when there is a primary available or slave[0] can
     *     respond to the isMaster command.
     */
    function _waitForHealth(node, state, timeout) {
        _waitForIndicator(node, state, "health", timeout);
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
            return (opTime.getTime() == 0 && opTime.getInc() == 0);
        }
        return (opTime.ts.getTime() == 0 && opTime.ts.getInc() == 0 && opTime.t == -1);
    }

    /**
     * Returns the OpTime for the specified host by issuing replSetGetStatus.
     */
    function _getLastOpTime(conn) {
        var replSetStatus =
            assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1}));
        var connStatus = replSetStatus.members.filter(m => m.self)[0];
        var opTime = connStatus.optime;
        if (_isEmptyOpTime(opTime)) {
            throw new Error("last OpTime is empty -- connection: " + conn);
        }
        return opTime;
    }

    /**
     * Returns the {readConcern: majority} OpTime for the host.
     * This is the OpTime of the host's "majority committed" snapshot.
     * This function may return an OpTime with Timestamp(0,0) and Term(0) if read concern majority
     * is not enabled, or if there has not been a committed snapshot yet.
     */
    function _getReadConcernMajorityOpTime(conn) {
        var replSetStatus =
            assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1}));
        return (replSetStatus.OpTimes || replSetStatus.optimes).readConcernMajorityOpTime ||
            {ts: Timestamp(0, 0), t: NumberLong(0)};
    }

    /**
     * Returns the last durable OpTime for the host if running with journaling.
     * Returns the last applied OpTime otherwise.
     */
    function _getDurableOpTime(conn) {
        var replSetStatus =
            assert.commandWorked(conn.getDB("admin").runCommand({replSetGetStatus: 1}));

        var opTimeType = "durableOpTime";
        if (_isRunningWithoutJournaling(conn)) {
            opTimeType = "appliedOpTime";
        }
        var opTime = replSetStatus.optimes[opTimeType];
        if (_isEmptyOpTime(opTime)) {
            throw new Error("last durable OpTime is empty -- connection: " + conn);
        }
        return opTime;
    }

    /*
     * Compares Timestamp objects. Returns true if ts1 is 'earlier' than ts2, else false.
     */
    function _isEarlierTimestamp(ts1, ts2) {
        if (ts1.getTime() == ts2.getTime()) {
            return ts1.getInc() < ts2.getInc();
        }
        return ts1.getTime() < ts2.getTime();
    }

    /*
     * Returns true if the node can be elected primary of a replica set.
     */
    function _isElectable(node) {
        return !node.arbiterOnly && (node.priority === undefined || node.priority != 0);
    }

    /**
     * Returns list of nodes as host:port strings.
     */
    this.nodeList = function() {
        var list = [];
        for (var i = 0; i < this.ports.length; i++) {
            list.push(this.host + ":" + this.ports[i]);
        }

        return list;
    };

    this.getNodeId = function(node) {
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
    };

    this.getPort = function(n) {
        var n = this.getNodeId(n);
        return this.ports[n];
    };

    this._addPath = function(p) {
        if (!_alldbpaths)
            _alldbpaths = [p];
        else
            _alldbpaths.push(p);

        return p;
    };

    this.getReplSetConfig = function() {
        var cfg = {};
        cfg._id = this.name;

        if (this.protocolVersion !== undefined && this.protocolVersion !== null) {
            cfg.protocolVersion = this.protocolVersion;
        }

        cfg.members = [];

        for (var i = 0; i < this.ports.length; i++) {
            var member = {};
            member._id = i;

            member.host = this.host;
            if (!member.host.contains('/')) {
                member.host += ":" + this.ports[i];
            }

            var nodeOpts = this.nodeOptions["n" + i];
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

        if (jsTestOptions().forceReplicationProtocolVersion !== undefined) {
            cfg.protocolVersion = jsTestOptions().forceReplicationProtocolVersion;
        }

        if (_configSettings) {
            cfg.settings = _configSettings;
        }

        return cfg;
    };

    this.getURL = function() {
        var hosts = [];

        for (var i = 0; i < this.ports.length; i++) {
            hosts.push(this.host + ":" + this.ports[i]);
        }

        return this.name + "/" + hosts.join(",");
    };

    /**
     * Starts each node in the replica set with the given options.
     *
     * @param options - The options passed to {@link MongoRunner.runMongod}
     */
    this.startSet = function(options, restart) {
        print("ReplSetTest starting set");

        if (options && options.keyFile) {
            self.keyFile = options.keyFile;
        }

        if (options) {
            self.startOptions = options;
        }

        var nodes = [];
        for (var n = 0; n < this.ports.length; n++) {
            nodes.push(this.start(n, options, restart));
        }

        this.nodes = nodes;
        return this.nodes;
    };

    /**
     * Blocks until the secondary nodes have completed recovery and their roles are known.
     */
    this.awaitSecondaryNodes = function(timeout) {
        timeout = timeout || self.kDefaultTimeoutMS;

        assert.soonNoExcept(function() {
            // Reload who the current slaves are
            self.getPrimary(timeout);

            var slaves = self.liveNodes.slaves;
            var len = slaves.length;
            var ready = true;

            for (var i = 0; i < len; i++) {
                var isMaster = slaves[i].getDB("admin").runCommand({ismaster: 1});
                var arbiter = (isMaster.arbiterOnly == undefined ? false : isMaster.arbiterOnly);
                ready = ready && (isMaster.secondary || arbiter);
            }

            return ready;
        }, "Awaiting secondaries", timeout);
    };

    /**
     * Blocks until the specified node says it's syncing from the given upstream node.
     */
    this.awaitSyncSource = function(node, upstreamNode, timeout) {
        print("Waiting for node " + node.name + " to start syncing from " + upstreamNode.name);
        var status = null;
        assert.soonNoExcept(
            function() {
                status = node.getDB("admin").runCommand({replSetGetStatus: 1});
                for (var j = 0; j < status.members.length; j++) {
                    if (status.members[j].self) {
                        return status.members[j].syncingTo === upstreamNode.host;
                    }
                }
                return false;
            },
            "Awaiting node " + node + " syncing from " + upstreamNode + ": " + tojson(status),
            timeout);
    };

    /**
     * Blocks until each node agrees that all other nodes have applied the most recent oplog entry.
     */
    this.awaitNodesAgreeOnAppliedOpTime = function(timeout, nodes) {
        timeout = timeout || self.kDefaultTimeoutMS;
        nodes = nodes || self.nodes;

        assert.soon(function() {
            let appliedOpTimeConsensus = undefined;
            for (let i = 0; i < nodes.length; i++) {
                let replSetGetStatus;
                try {
                    replSetGetStatus = nodes[i].adminCommand({replSetGetStatus: 1});
                } catch (e) {
                    print("AwaitNodesAgreeOnAppliedOpTime: Retrying because node " + nodes[i].name +
                          " failed to execute replSetGetStatus: " + tojson(e));
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
                        let optimeMembers = replSetGetStatus.members.filter(m => m.optime);
                        assert(optimeMembers.length > 0,
                               "AwaitNodesAgreeOnAppliedOpTime: replSetGetStatus did not " +
                                   "include optimes for any members: " + tojson(replSetGetStatus));
                        appliedOpTimeConsensus = optimeMembers[0].optime;
                    }

                    assert(appliedOpTimeConsensus,
                           "AwaitNodesAgreeOnAppliedOpTime: missing appliedOpTime in " +
                               "replSetGetStatus: " + tojson(replSetGetStatus));
                }

                if (replSetGetStatus.optimes &&
                    !friendlyEqual(replSetGetStatus.optimes.appliedOpTime,
                                   appliedOpTimeConsensus)) {
                    print("AwaitNodesAgreeOnAppliedOpTime: Retrying because node " + nodes[i].name +
                          " has appliedOpTime " + tojson(replSetGetStatus.optimes.appliedOpTime) +
                          " that does not match the previously observed appliedOpTime " +
                          tojson(appliedOpTimeConsensus));
                    return false;
                }

                for (let j = 0; j < replSetGetStatus.members.length; j++) {
                    if (replSetGetStatus.members[j].state == ReplSetTest.State.ARBITER) {
                        // ARBITER nodes do not apply oplog entries and do not have an 'optime'
                        // field.
                        continue;
                    }

                    if (!friendlyEqual(replSetGetStatus.members[j].optime,
                                       appliedOpTimeConsensus)) {
                        print("AwaitNodesAgreeOnAppliedOpTime: Retrying because node " +
                              nodes[i].name + " sees optime " +
                              tojson(replSetGetStatus.members[j].optime) + " on node " +
                              replSetGetStatus.members[j].name + " but expects to see optime " +
                              tojson(appliedOpTimeConsensus));
                        return false;
                    }
                }
            }

            print(
                "AwaitNodesAgreeOnAppliedOpTime: All nodes agree that all ops are applied up to " +
                tojson(appliedOpTimeConsensus));
            return true;
        }, "Awaiting nodes to agree that all ops are applied across replica set", timeout);
    };

    /**
     * Blocks until all nodes agree on who the primary is.
     * If 'expectedPrimaryNodeId' is provided, ensure that every node is seeing this node as the
     * primary. Otherwise, ensure that all the nodes in the set agree with the first node on the
     * identity of the primary.
     */
    this.awaitNodesAgreeOnPrimary = function(timeout, nodes, expectedPrimaryNodeId) {
        timeout = timeout || self.kDefaultTimeoutMS;
        nodes = nodes || self.nodes;
        expectedPrimaryNodeId = expectedPrimaryNodeId || -1;
        if (expectedPrimaryNodeId === -1) {
            print("AwaitNodesAgreeOnPrimary: Waiting for nodes to agree on any primary.");
        } else {
            print("AwaitNodesAgreeOnPrimary: Waiting for nodes to agree on " +
                  nodes[expectedPrimaryNodeId].name + " as primary.");
        }

        assert.soonNoExcept(function() {
            var primary = expectedPrimaryNodeId;

            for (var i = 0; i < nodes.length; i++) {
                var replSetGetStatus = nodes[i].getDB("admin").runCommand({replSetGetStatus: 1});
                var nodesPrimary = -1;
                for (var j = 0; j < replSetGetStatus.members.length; j++) {
                    if (replSetGetStatus.members[j].state === ReplSetTest.State.PRIMARY) {
                        // Node sees two primaries.
                        if (nodesPrimary !== -1) {
                            print("AwaitNodesAgreeOnPrimary: Retrying because " + nodes[i].name +
                                  " thinks both " + nodes[nodesPrimary].name + " and " +
                                  nodes[j].name + " are primary.");

                            return false;
                        }
                        nodesPrimary = j;
                    }
                }
                // Node doesn't see a primary.
                if (nodesPrimary < 0) {
                    print("AwaitNodesAgreeOnPrimary: Retrying because " + nodes[i].name +
                          " does not see a primary.");
                    return false;
                }

                if (primary < 0) {
                    // If we haven't seen a primary yet, set it to this.
                    primary = nodesPrimary;
                } else if (primary !== nodesPrimary) {
                    print("AwaitNodesAgreeOnPrimary: Retrying because " + nodes[i].name +
                          " thinks the primary is " + nodes[nodesPrimary].name + " instead of " +
                          nodes[primary].name);
                    return false;
                }
            }

            print("AwaitNodesAgreeOnPrimary: Nodes agreed on primary " + nodes[primary].name);
            return true;
        }, "Awaiting nodes to agree on primary", timeout);
    };

    /**
     * Blocking call, which will wait for a primary to be elected and become master for some
     * pre-defined timeout. If a primary is available it will return a connection to it.
     * Otherwise throws an exception.
     */
    this.getPrimary = function(timeout) {
        timeout = timeout || self.kDefaultTimeoutMS;
        var primary = null;

        assert.soonNoExcept(function() {
            primary = _callIsMaster();
            return primary;
        }, "Finding primary", timeout);

        return primary;
    };

    this.awaitNoPrimary = function(msg, timeout) {
        msg = msg || "Timed out waiting for there to be no primary in replset: " + this.name;
        timeout = timeout || self.kDefaultTimeoutMS;

        assert.soonNoExcept(function() {
            return _callIsMaster() == false;
        }, msg, timeout);
    };

    this.getSecondaries = function(timeout) {
        var master = this.getPrimary(timeout);
        var secs = [];
        for (var i = 0; i < this.nodes.length; i++) {
            if (this.nodes[i] != master) {
                secs.push(this.nodes[i]);
            }
        }

        return secs;
    };

    this.getSecondary = function(timeout) {
        return this.getSecondaries(timeout)[0];
    };

    this.getArbiters = function() {
        var arbiters = [];
        for (var i = 0; i < this.nodes.length; i++) {
            var node = this.nodes[i];

            let isArbiter = false;

            assert.retryNoExcept(() => {
                isArbiter = node.getDB('admin').isMaster('admin').arbiterOnly;
                return true;
            }, `Could not call 'isMaster' on ${node}.`, 3, 1000);

            if (isArbiter) {
                arbiters.push(node);
            }
        }
        return arbiters;
    };

    this.getArbiter = function() {
        return this.getArbiters()[0];
    };

    this.status = function(timeout) {
        var master = _callIsMaster();
        if (!master) {
            master = this.liveNodes.slaves[0];
        }

        return master.getDB("admin").runCommand({replSetGetStatus: 1});
    };

    /**
     * Adds a node to the replica set managed by this instance.
     */
    this.add = function(config) {
        var nextPort = allocatePort();
        print("ReplSetTest Next port: " + nextPort);

        this.ports.push(nextPort);
        printjson(this.ports);

        if (_useBridge) {
            _unbridgedPorts.push(allocatePort());
        }

        var nextId = this.nodes.length;
        printjson(this.nodes);

        print("ReplSetTest nextId: " + nextId);
        return this.start(nextId, config);
    };

    this.remove = function(nodeId) {
        nodeId = this.getNodeId(nodeId);
        this.nodes.splice(nodeId, 1);
        this.ports.splice(nodeId, 1);

        if (_useBridge) {
            _unbridgedPorts.splice(nodeId, 1);
            _unbridgedNodes.splice(nodeId, 1);
        }
    };

    /*
     * If journaling is disabled or we are using an ephemeral storage engine, set
     * 'writeConcernMajorityJournalDefault' to false for the given 'config' object. If the
     * 'writeConcernMajorityJournalDefault' field is already set, it does not override it,
     * and returns the 'config' object unchanged. Does not affect 'config' when running CSRS.
     */
    this._updateConfigIfNotDurable = function(config) {

        // Get a replica set node (check for use of bridge).
        var replNode = _useBridge ? _unbridgedNodes[0] : this.nodes[0];

        // Don't update replset config for sharding config servers since config servers always
        // require durable storage.
        if (replNode.hasOwnProperty("fullOptions") &&
            replNode.fullOptions.hasOwnProperty("configsvr")) {
            return config;
        }

        // Don't override existing value.
        var wcMajorityJournalField = "writeConcernMajorityJournalDefault";
        if (config.hasOwnProperty(wcMajorityJournalField)) {
            return config;
        }

        // Check journaling by sending commands through the bridge if it's used.
        if (_isRunningWithoutJournaling(this.nodes[0])) {
            config[wcMajorityJournalField] = false;
        }

        return config;
    };

    this._setDefaultConfigOptions = function(config) {
        if (jsTestOptions().forceReplicationProtocolVersion !== undefined &&
            !config.hasOwnProperty("protocolVersion")) {
            config.protocolVersion = jsTestOptions().forceReplicationProtocolVersion;
        }
        // Update config for non journaling test variants
        this._updateConfigIfNotDurable(config);
    };

    /**
     * Runs replSetInitiate on the first node of the replica set.
     * Ensures that a primary is elected (not necessarily node 0).
     * initiate() should be preferred instead of this, but this is useful when the connections
     * aren't authorized to run replSetGetStatus.
     * TODO(SERVER-14017): remove this in favor of using initiate() everywhere.
     */
    this.initiateWithAnyNodeAsPrimary = function(cfg, initCmd) {
        var master = this.nodes[0].getDB("admin");
        var config = cfg || this.getReplSetConfig();
        var cmd = {};
        var cmdKey = initCmd || 'replSetInitiate';

        // Throw an exception if nodes[0] is unelectable in the given config.
        if (!_isElectable(config.members[0])) {
            throw Error("The node at index 0 must be electable");
        }

        // Start up a single node replica set then reconfigure to the correct size (if the config
        // contains more than 1 node), so the primary is elected more quickly.
        var originalMembers, originalSettings;
        if (config.members && config.members.length > 1) {
            originalMembers = config.members.slice();
            config.members = config.members.slice(0, 1);
            originalSettings = config.settings;
            delete config.settings;  // Clear settings to avoid tags referencing sliced nodes.
        }
        this._setDefaultConfigOptions(config);

        cmd[cmdKey] = config;
        printjson(cmd);

        assert.commandWorked(master.runCommand(cmd), tojson(cmd));
        this.getPrimary();  // Blocks until there is a primary.

        // Reconfigure the set to contain the correct number of nodes (if necessary).
        if (originalMembers) {
            config.members = originalMembers;
            if (originalSettings) {
                config.settings = originalSettings;
            }
            config.version = 2;

            // Nodes started with the --configsvr flag must have configsvr = true in their config.
            if (this.nodes[0].hasOwnProperty("fullOptions") &&
                this.nodes[0].fullOptions.hasOwnProperty("configsvr")) {
                config.configsvr = true;
            }

            cmd = {replSetReconfig: config};
            print("Reconfiguring replica set to add in other nodes");
            printjson(cmd);

            // replSetInitiate and replSetReconfig commands can fail with a NodeNotFound error
            // if a heartbeat times out during the quorum check.
            // They may also fail with NewReplicaSetConfigurationIncompatible on similar timeout
            // during the config validation stage while deducing isSelf().
            // We retry three times to reduce the chance of failing this way.
            assert.retry(() => {
                var res;
                try {
                    res = master.runCommand(cmd);
                    if (res.ok === 1) {
                        return true;
                    }
                } catch (e) {
                    // reconfig can lead to a stepdown if the primary looks for a majority before
                    // a majority of nodes have successfully joined the set. If there is a stepdown
                    // then the reconfig request will be killed and respond with a network error.
                    if (isNetworkError(e)) {
                        return true;
                    }
                    throw e;
                }

                assert.commandFailedWithCode(
                    res,
                    [ErrorCodes.NodeNotFound, ErrorCodes.NewReplicaSetConfigurationIncompatible],
                    "replSetReconfig during initiate failed");
                return false;
            }, "replSetReconfig during initiate failed", 3, 5 * 1000);
        }

        // Setup authentication if running test with authentication
        if ((jsTestOptions().keyFile) && cmdKey == 'replSetInitiate') {
            master = this.getPrimary();
            jsTest.authenticateNodes(this.nodes);
        }

        this.awaitSecondaryNodes();

        let shouldWaitForKeys = true;
        if (self.waitForKeys != undefined) {
            shouldWaitForKeys = self.waitForKeys;
            print("Set shouldWaitForKeys from RS options: " + shouldWaitForKeys);
        } else {
            Object.keys(self.nodeOptions).forEach(function(key, index) {
                let val = self.nodeOptions[key];
                if (typeof(val) === "object" &&
                    (val.hasOwnProperty("shardsvr") ||
                     // TODO: SERVER-31376
                     val.hasOwnProperty("binVersion") && val.binVersion != "latest")) {
                    shouldWaitForKeys = false;
                    print("Set shouldWaitForKeys from node options: " + shouldWaitForKeys);
                }
            });
            if (self.startOptions != undefined) {
                let val = self.startOptions;
                if (typeof(val) === "object" &&
                    (val.hasOwnProperty("shardsvr") ||
                     val.hasOwnProperty("binVersion") && val.binVersion != "latest")) {
                    shouldWaitForKeys = false;
                    print("Set shouldWaitForKeys from start options: " + shouldWaitForKeys);
                }
            }
        }
        /**
         * Blocks until the primary node generates cluster time sign keys.
         */
        if (shouldWaitForKeys) {
            var timeout = self.kDefaultTimeoutMS;
            asCluster(this.nodes, function(timeout) {
                print("Waiting for keys to sign $clusterTime to be generated");
                assert.soonNoExcept(function(timeout) {
                    var keyCnt = self.getPrimary(timeout)
                                     .getCollection('admin.system.keys')
                                     .find({purpose: 'HMAC'})
                                     .itcount();
                    return keyCnt >= 2;
                }, "Awaiting keys", timeout);
            });
        }

        // Set 'featureCompatibilityVersion' for the entire replica set, if specified.
        if (jsTest.options().replSetFeatureCompatibilityVersion) {
            // Authenticate before running the command.
            asCluster(self.nodes, function setFCV() {
                let fcv = jsTest.options().replSetFeatureCompatibilityVersion;
                print("Setting feature compatibility version for replica set to '" + fcv + "'");
                assert.commandWorked(
                    self.getPrimary().adminCommand({setFeatureCompatibilityVersion: fcv}));

                // Wait for the new 'featureCompatibilityVersion' to propagate to all nodes in the
                // replica set. The 'setFeatureCompatibilityVersion' command only waits for
                // replication to a majority of nodes by default.
                self.awaitReplication();
            });
        }

    };

    /**
     * Runs replSetInitiate on the replica set and requests the first node to step up as primary.
     * This version should be prefered where possible but requires all connections in the
     * ReplSetTest to be authorized to run replSetGetStatus.
     */
    this.initiateWithNodeZeroAsPrimary = function(cfg, initCmd) {
        this.initiateWithAnyNodeAsPrimary(cfg, initCmd);

        // stepUp() calls awaitReplication() which requires all nodes to be authorized to run
        // replSetGetStatus.
        asCluster(this.nodes, function() {
            self.stepUp(self.nodes[0]);
        });
    };

    /**
     * Runs replSetInitiate on the replica set and requests the first node to step up as
     * primary.
     */
    this.initiate = function(cfg, initCmd) {
        this.initiateWithNodeZeroAsPrimary(cfg, initCmd);
    };

    /**
     * Steps up 'node' as primary.
     * Waits for all nodes to reach the same optime before sending the replSetStepUp command
     * to 'node'.
     * Calls awaitReplication() which requires all connections in 'nodes' to be authenticated.
     */
    this.stepUp = function(node) {
        this.awaitReplication();
        this.awaitNodesAgreeOnAppliedOpTime();
        this.awaitNodesAgreeOnPrimary();
        if (this.getPrimary() === node) {
            return;
        }

        if (this.protocolVersion === 0 || jsTestOptions().forceReplicationProtocolVersion === 0) {
            // Ensure the specified node is primary.
            for (let i = 0; i < this.nodes.length; i++) {
                let primary = this.getPrimary();
                if (primary === node) {
                    break;
                }
                try {
                    // Make sure the nodes do not step back up for 10 minutes.
                    assert.commandWorked(
                        primary.adminCommand({replSetStepDown: 10 * 60, force: true}));
                } catch (ex) {
                    print("Caught exception while stepping down node '" + tojson(node.host) +
                          "': " + tojson(ex));
                }
                this.awaitReplication();
                this.awaitNodesAgreeOnAppliedOpTime();
                this.awaitNodesAgreeOnPrimary();
            }

            // Reset the rest of the nodes so they can run for election during the test.
            for (let i = 0; i < this.nodes.length; i++) {
                // Cannot call replSetFreeze on the primary.
                if (this.nodes[i] === node) {
                    continue;
                }
                assert.commandWorked(this.nodes[i].adminCommand({replSetFreeze: 0}));
            }
        } else {
            assert.commandWorked(node.adminCommand({replSetStepUp: 1}));
            this.awaitNodesAgreeOnPrimary();
        }
        assert.eq(this.getPrimary(), node, node.host + " was not primary after stepUp");
    };

    /**
     * Gets the current replica set config from the specified node index. If no nodeId is specified,
     * uses the primary node.
     */
    this.getReplSetConfigFromNode = function(nodeId) {
        if (nodeId == undefined) {
            // Use 90 seconds timeout for finding a primary
            return _replSetGetConfig(self.getPrimary(90 * 1000));
        }

        if (!isNumber(nodeId)) {
            throw Error(nodeId + ' is not a number');
        }

        return _replSetGetConfig(self.nodes[nodeId]);
    };

    this.reInitiate = function() {
        var config = this.getReplSetConfigFromNode();
        var newConfig = this.getReplSetConfig();
        // Only reset members.
        config.members = newConfig.members;
        config.version += 1;

        this._setDefaultConfigOptions(config);

        assert.adminCommandWorkedAllowingNetworkError(this.getPrimary(), {replSetReconfig: config});
    };

    /**
     * Blocks until all nodes in the replica set have the same config version as the primary.
     **/
    this.awaitNodesAgreeOnConfigVersion = function(timeout) {
        timeout = timeout || this.kDefaultTimeoutMS;

        assert.soonNoExcept(function() {
            var primaryVersion = self.getPrimary().adminCommand({ismaster: 1}).setVersion;

            for (var i = 0; i < self.nodes.length; i++) {
                var version = self.nodes[i].adminCommand({ismaster: 1}).setVersion;
                assert.eq(version,
                          primaryVersion,
                          "waiting for secondary node " + self.nodes[i].host +
                              " with config version of " + version +
                              " to match the version of the primary " + primaryVersion);
            }

            return true;
        }, "Awaiting nodes to agree on config version", timeout);
    };

    /**
     * Waits for the last oplog entry on the primary to be visible in the committed snapshop view
     * of the oplog on *all* secondaries.
     * Returns last oplog entry.
     */
    this.awaitLastOpCommitted = function() {
        var rst = this;
        var master = rst.getPrimary();
        var masterOpTime = _getLastOpTime(master);

        print("Waiting for op with OpTime " + tojson(masterOpTime) +
              " to be committed on all secondaries");

        assert.soonNoExcept(function() {
            for (var i = 0; i < rst.nodes.length; i++) {
                var node = rst.nodes[i];

                // Continue if we're connected to an arbiter
                var res = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
                if (res.myState == ReplSetTest.State.ARBITER) {
                    continue;
                }
                var rcmOpTime = _getReadConcernMajorityOpTime(node);
                if (friendlyEqual(rcmOpTime, {ts: Timestamp(0, 0), t: NumberLong(0)})) {
                    return false;
                }
                if (rs.compareOpTimes(rcmOpTime, masterOpTime) < 0) {
                    return false;
                }
            }

            return true;
        }, "Op with OpTime " + tojson(masterOpTime) + " failed to be committed on all secondaries");

        return masterOpTime;
    };

    // Wait until the optime of the specified type reaches the primary's last applied optime.
    this.awaitReplication = function(timeout, secondaryOpTimeType) {
        timeout = timeout || self.kDefaultTimeoutMS;
        secondaryOpTimeType = secondaryOpTimeType || ReplSetTest.OpTimeType.LAST_APPLIED;

        var masterLatestOpTime;

        // Blocking call, which will wait for the last optime written on the master to be available
        var awaitLastOpTimeWrittenFn = function() {
            var master = self.getPrimary();
            assert.soonNoExcept(function() {
                try {
                    masterLatestOpTime = _getLastOpTime(master);
                } catch (e) {
                    print("ReplSetTest caught exception " + e);
                    return false;
                }

                return true;
            }, "awaiting oplog query", timeout);
        };

        awaitLastOpTimeWrittenFn();

        // get the latest config version from master (with a few retries in case of error)
        var masterConfigVersion;
        var masterName;
        var master;
        var num_attempts = 3;

        assert.retryNoExcept(() => {
            master = this.getPrimary();
            masterConfigVersion = this.getReplSetConfigFromNode().version;
            masterName = master.toString().substr(14);  // strip "connection to "
            return true;
        }, "ReplSetTest awaitReplication: couldnt get repl set config.", num_attempts, 1000);

        print("ReplSetTest awaitReplication: starting: optime for primary, " + masterName +
              ", is " + tojson(masterLatestOpTime));

        assert.soonNoExcept(function() {
            try {
                print("ReplSetTest awaitReplication: checking secondaries " +
                      "against latest primary optime " + tojson(masterLatestOpTime));
                var secondaryCount = 0;
                for (var i = 0; i < self.liveNodes.slaves.length; i++) {
                    var slave = self.liveNodes.slaves[i];
                    var slaveName = slave.toString().substr(14);  // strip "connection to "

                    var slaveConfigVersion =
                        slave.getDB("local")['system.replset'].findOne().version;

                    if (masterConfigVersion != slaveConfigVersion) {
                        print("ReplSetTest awaitReplication: secondary #" + secondaryCount + ", " +
                              slaveName + ", has config version #" + slaveConfigVersion +
                              ", but expected config version #" + masterConfigVersion);

                        if (slaveConfigVersion > masterConfigVersion) {
                            master = self.getPrimary();
                            masterConfigVersion =
                                master.getDB("local")['system.replset'].findOne().version;
                            masterName = master.toString().substr(14);  // strip "connection to "

                            print("ReplSetTest awaitReplication: optime for primary, " +
                                  masterName + ", is " + tojson(masterLatestOpTime));
                        }

                        return false;
                    }

                    // Continue if we're connected to an arbiter
                    var res = assert.commandWorked(slave.adminCommand({replSetGetStatus: 1}));
                    if (res.myState == ReplSetTest.State.ARBITER) {
                        continue;
                    }

                    ++secondaryCount;
                    print("ReplSetTest awaitReplication: checking secondary #" + secondaryCount +
                          ": " + slaveName);

                    slave.getDB("admin").getMongo().setSlaveOk();

                    var slaveOpTime;
                    if (secondaryOpTimeType == ReplSetTest.OpTimeType.LAST_DURABLE) {
                        slaveOpTime = _getDurableOpTime(slave);
                    } else {
                        slaveOpTime = _getLastOpTime(slave);
                    }

                    if (rs.compareOpTimes(masterLatestOpTime, slaveOpTime) < 0) {
                        masterLatestOpTime = _getLastOpTime(master);
                        print("ReplSetTest awaitReplication: optime for " + slaveName +
                              " is newer, resetting latest primary optime to " +
                              tojson(masterLatestOpTime));
                        return false;
                    }

                    if (!friendlyEqual(masterLatestOpTime, slaveOpTime)) {
                        print("ReplSetTest awaitReplication: optime for secondary #" +
                              secondaryCount + ", " + slaveName + ", is " + tojson(slaveOpTime) +
                              " but latest is " + tojson(masterLatestOpTime));
                        print("ReplSetTest awaitReplication: secondary #" + secondaryCount + ", " +
                              slaveName + ", is NOT synced");
                        return false;
                    }

                    print("ReplSetTest awaitReplication: secondary #" + secondaryCount + ", " +
                          slaveName + ", is synced");
                }

                print("ReplSetTest awaitReplication: finished: all " + secondaryCount +
                      " secondaries synced at optime " + tojson(masterLatestOpTime));
                return true;
            } catch (e) {
                print("ReplSetTest awaitReplication: caught exception " + e);

                // We might have a new master now
                awaitLastOpTimeWrittenFn();

                print("ReplSetTest awaitReplication: resetting: optime for primary " +
                      self.liveNodes.master + " is " + tojson(masterLatestOpTime));

                return false;
            }
        }, "awaiting replication", timeout);
    };

    this.getHashes = function(db) {
        assert.neq(db, 'local', 'Cannot run getHashes() on the "local" database');

        this.getPrimary();
        var res = {};

        // If MapReduce is interrupted by a stepdown, it could still have 'tmp.mr' collections that
        // it will not be able to delete. Excluding them from dbhash will prevent a mismatch.
        // TODO SERVER-27147: no need to exclude 'tmp.mr' collections
        var collections = this.liveNodes.master.getDB(db).getCollectionNames();
        var colls_excluding_tmp_mr = collections.filter(coll => !coll.startsWith("tmp.mr."));
        res.master = this.liveNodes.master.getDB(db).runCommand(
            {dbhash: 1, collections: colls_excluding_tmp_mr});
        res.slaves = [];
        this.liveNodes.slaves.forEach(function(node) {
            var isArbiter = node.getDB('admin').isMaster('admin').arbiterOnly;
            if (!isArbiter) {
                collections = node.getDB(db).getCollectionNames();
                colls_excluding_tmp_mr = collections.filter(coll => {
                    return !coll.startsWith("tmp.mr.");
                });
                var slaveRes =
                    node.getDB(db).runCommand({dbhash: 1, collections: colls_excluding_tmp_mr});
                res.slaves.push(slaveRes);
            }
        });
        return res;
    };

    this.dumpOplog = function(conn, query = {}, limit = 10) {
        var log = 'Dumping the latest ' + limit + ' documents that match ' + tojson(query) +
            ' from the oplog ' + oplogName + ' of ' + conn.host;
        var cursor = conn.getDB('local')
                         .getCollection(oplogName)
                         .find(query)
                         .sort({$natural: -1})
                         .limit(limit);
        cursor.forEach(function(entry) {
            log = log + '\n' + tojsononeline(entry);
        });
        jsTestLog(log);
    };

    // Call the provided checkerFunction, after the replica set has been write locked.
    this.checkReplicaSet = function(checkerFunction, ...checkerFunctionArgs) {
        assert.eq(typeof checkerFunction,
                  "function",
                  "Expected checkerFunction parameter to be a function");

        // Call getPrimary to populate rst with information about the nodes.
        var primary = this.getPrimary();
        assert(primary, 'calling getPrimary() failed');

        // Since we cannot determine if there is a background index in progress (SERVER-26624), we
        // use the "collMod" command to wait for any index builds that may be in progress on the
        // primary or on one of the secondaries to complete.
        for (let dbName of primary.getDBNames()) {
            if (dbName === "local") {
                continue;
            }

            let dbHandle = primary.getDB(dbName);
            dbHandle.getCollectionInfos({$or: [{type: "collection"}, {type: {$exists: false}}]})
                .forEach(function(collInfo) {
                    // Skip system collections. We handle here rather than in the getCollectionInfos
                    // filter to take advantage of the fact that a simple 'collection' filter will
                    // skip view evaluation, and therefore won't fail on an invalid view.
                    if (!collInfo.name.startsWith('system.')) {
                        // 'usePowerOf2Sizes' is ignored by the server so no actual collection
                        // modification takes place. We intentionally await replication without
                        // doing any I/O to avoid any overhead from allocating or deleting data
                        // files when using the MMAPv1 storage engine. We call awaitReplication()
                        // later on to ensure the collMod is replicated to all nodes.
                        try {
                            assert.commandWorked(dbHandle.runCommand({
                                collMod: collInfo.name,
                                usePowerOf2Sizes: true,
                            }));
                        } catch (e) {
                            // Ignore NamespaceNotFound errors because a background thread could
                            // have dropped the collection after getCollectionInfos but before
                            // running collMod.
                            if (e.code != ErrorCodes.NamespaceNotFound) {
                                throw e;
                            }
                        }
                    }
                });
        }

        var activeException = false;

        // Lock the primary to prevent the TTL monitor from deleting expired documents in
        // the background while we are getting the dbhashes of the replica set members.
        assert.commandWorked(primary.adminCommand({fsync: 1, lock: 1}),
                             'failed to lock the primary');
        try {
            this.awaitReplication();
            checkerFunction.apply(this, checkerFunctionArgs);
        } catch (e) {
            activeException = true;
            throw e;
        } finally {
            // Allow writes on the primary.
            var res = primary.adminCommand({fsyncUnlock: 1});

            if (!res.ok) {
                var msg = 'failed to unlock the primary, which may cause this' +
                    ' test to hang: ' + tojson(res);
                if (activeException) {
                    print(msg);
                } else {
                    throw new Error(msg);
                }
            }
        }
    };

    this.checkReplicatedDataHashes = function(
        msgPrefix = 'checkReplicatedDataHashes', excludedDBs = [], ignoreUUIDs = false) {
        // Return items that are in either Array `a` or `b` but not both. Note that this will
        // not work with arrays containing NaN. Array.indexOf(NaN) will always return -1.

        var collectionPrinted = new Set();

        function arraySymmetricDifference(a, b) {
            var inAOnly = a.filter(function(elem) {
                return b.indexOf(elem) < 0;
            });

            var inBOnly = b.filter(function(elem) {
                return a.indexOf(elem) < 0;
            });

            return inAOnly.concat(inBOnly);
        }

        function printCollectionInfo(connName, conn, dbName, collName) {
            var ns = dbName + '.' + collName;
            var hostColl = `${conn.host}--${ns}`;
            var alreadyPrinted = collectionPrinted.has(hostColl);

            // Extract basic collection info.
            var coll = conn.getDB(dbName).getCollection(collName);
            var res = conn.getDB(dbName).runCommand({listCollections: 1, filter: {name: collName}});
            var collInfo = null;
            if (res.ok === 1 && res.cursor.firstBatch.length !== 0) {
                collInfo = {
                    ns: ns,
                    host: conn.host,
                    UUID: res.cursor.firstBatch[0].info.uuid,
                    count: coll.find().itcount()
                };
            }
            var infoPrefix = `${connName}(${conn.host}) info for ${ns} : `;
            if (collInfo !== null) {
                if (alreadyPrinted) {
                    print(`${connName} info for ${ns} already printed. Search for ` +
                          `'${infoPrefix}'`);
                } else {
                    print(infoPrefix + tojsononeline(collInfo));
                }
            } else {
                print(infoPrefix + 'collection does not exist');
            }

            var collStats = conn.getDB(dbName).runCommand({collStats: collName});
            var statsPrefix = `${connName}(${conn.host}) collStats for ${ns}: `;
            if (collStats.ok === 1) {
                if (alreadyPrinted) {
                    print(`${connName} collStats for ${ns} already printed. Search for ` +
                          `'${statsPrefix}'`);
                } else {
                    print(statsPrefix + tojsononeline(collStats));
                }
            } else {
                print(`${statsPrefix}  error: ${tojsononeline(collStats)}`);
            }

            collectionPrinted.add(hostColl);

            // Return true if collInfo & collStats can be retrieved for conn.
            return collInfo !== null && collStats.ok === 1;
        }

        function dumpCollectionDiff(primary, secondary, dbName, collName) {
            var ns = dbName + '.' + collName;
            print('Dumping collection: ' + ns);

            var primaryExists = printCollectionInfo('primary', primary, dbName, collName);
            var secondaryExists = printCollectionInfo('secondary', secondary, dbName, collName);

            if (!primaryExists || !secondaryExists) {
                print(`Skipping checking collection differences for ${ns} since it does not ` +
                      'exist on primary and secondary');
                return;
            }

            var primaryColl = primary.getDB(dbName).getCollection(collName);
            var secondaryColl = secondary.getDB(dbName).getCollection(collName);

            var primaryDocs = primaryColl.find().sort({_id: 1}).toArray();
            var secondaryDocs = secondaryColl.find().sort({_id: 1}).toArray();

            var primaryIndex = primaryDocs.length - 1;
            var secondaryIndex = secondaryDocs.length - 1;

            var missingOnPrimary = [];
            var missingOnSecondary = [];

            while (primaryIndex >= 0 || secondaryIndex >= 0) {
                var primaryDoc = primaryDocs[primaryIndex];
                var secondaryDoc = secondaryDocs[secondaryIndex];

                if (primaryIndex < 0) {
                    missingOnPrimary.push(tojsononeline(secondaryDoc));
                    secondaryIndex--;
                } else if (secondaryIndex < 0) {
                    missingOnSecondary.push(tojsononeline(primaryDoc));
                    primaryIndex--;
                } else {
                    if (!bsonBinaryEqual(primaryDoc, secondaryDoc)) {
                        print('Mismatching documents:');
                        print('    primary: ' + tojsononeline(primaryDoc));
                        print('    secondary: ' + tojsononeline(secondaryDoc));
                        var ordering =
                            bsonWoCompare({wrapper: primaryDoc._id}, {wrapper: secondaryDoc._id});
                        if (ordering === 0) {
                            primaryIndex--;
                            secondaryIndex--;
                        } else if (ordering < 0) {
                            missingOnPrimary.push(tojsononeline(secondaryDoc));
                            secondaryIndex--;
                        } else if (ordering > 0) {
                            missingOnSecondary.push(tojsononeline(primaryDoc));
                            primaryIndex--;
                        }
                    } else {
                        // Latest document matched.
                        primaryIndex--;
                        secondaryIndex--;
                    }
                }
            }

            if (missingOnPrimary.length) {
                print('The following documents are missing on the primary:');
                print(missingOnPrimary.join('\n'));
            }
            if (missingOnSecondary.length) {
                print('The following documents are missing on the secondary:');
                print(missingOnSecondary.join('\n'));
            }
        }

        function checkDBHashesForReplSet(rst, dbBlacklist = [], msgPrefix, ignoreUUIDs) {
            // We don't expect the local database to match because some of its
            // collections are not replicated.
            dbBlacklist.push('local');

            var success = true;
            var hasDumpedOplog = false;

            // Use liveNodes.master instead of getPrimary() to avoid the detection
            // of a new primary.
            // liveNodes must have been populated.
            var primary = rst.liveNodes.master;
            var combinedDBs = new Set(primary.getDBNames());
            // replSetConfig will be undefined for master/slave passthrough.
            const replSetConfig =
                rst.getReplSetConfigFromNode ? rst.getReplSetConfigFromNode() : undefined;

            rst.liveNodes.slaves.forEach(secondary => {
                secondary.getDBNames().forEach(dbName => combinedDBs.add(dbName));
            });

            for (var dbName of combinedDBs) {
                if (Array.contains(dbBlacklist, dbName)) {
                    continue;
                }

                try {
                    var dbHashes = rst.getHashes(dbName);
                    var primaryDBHash = dbHashes.master;
                    var primaryCollections = Object.keys(primaryDBHash.collections);
                    assert.commandWorked(primaryDBHash);

                    // Filter only collections that were retrieved by the dbhash. listCollections
                    // may include non-replicated collections like system.profile.
                    var primaryCollInfo =
                        primary.getDB(dbName).getCollectionInfos({name: {$in: primaryCollections}});

                } catch (e) {
                    if (jsTest.options().skipValidationOnInvalidViewDefinitions) {
                        assert.commandFailedWithCode(e, ErrorCodes.InvalidViewDefinition);
                        print('Skipping dbhash check on ' + dbName +
                              ' because of invalid views in system.views');
                        continue;
                    } else {
                        throw e;
                    }
                }

                dbHashes.slaves.forEach(secondaryDBHash => {
                    assert.commandWorked(secondaryDBHash);

                    var secondary = secondaryDBHash._mongo;
                    var secondaryCollections = Object.keys(secondaryDBHash.collections);

                    if (primaryCollections.length !== secondaryCollections.length) {
                        print(
                            msgPrefix +
                            ', the primary and secondary have a different number of collections: ' +
                            tojson(dbHashes));
                        for (var diffColl of arraySymmetricDifference(primaryCollections,
                                                                      secondaryCollections)) {
                            dumpCollectionDiff(primary, secondary, dbName, diffColl);
                        }
                        success = false;
                    }

                    var nonCappedCollNames = primaryCollections.filter(
                        collName => !primary.getDB(dbName).getCollection(collName).isCapped());
                    // Only compare the dbhashes of non-capped collections because capped
                    // collections are not necessarily truncated at the same points
                    // across replica set members.
                    nonCappedCollNames.forEach(collName => {
                        if (primaryDBHash.collections[collName] !==
                            secondaryDBHash.collections[collName]) {
                            print(msgPrefix +
                                  ', the primary and secondary have a different hash for the' +
                                  ' collection ' + dbName + '.' + collName + ': ' +
                                  tojson(dbHashes));
                            dumpCollectionDiff(primary, secondary, dbName, collName);
                            success = false;
                        }

                    });

                    // Check that collection information is consistent on the primary and
                    // secondaries.
                    var secondaryCollInfo = secondary.getDB(dbName).getCollectionInfos(
                        {name: {$in: secondaryCollections}});

                    secondaryCollInfo.forEach(secondaryInfo => {
                        primaryCollInfo.forEach(primaryInfo => {
                            if (secondaryInfo.name === primaryInfo.name &&
                                secondaryInfo.type === primaryInfo.type) {
                                if (ignoreUUIDs) {
                                    print(msgPrefix + ", skipping UUID check for " +
                                          primaryInfo.name);
                                    primaryInfo.info.uuid = null;
                                    secondaryInfo.info.uuid = null;
                                }
                                if (!bsonBinaryEqual(secondaryInfo, primaryInfo)) {
                                    print(msgPrefix +
                                          ', the primary and secondary have different ' +
                                          'attributes for the collection or view' + dbName + '.' +
                                          secondaryInfo.name);
                                    dumpCollectionDiff(
                                        primary, secondary, dbName, secondaryInfo.name);
                                    success = false;
                                }
                            }
                        });
                    });

                    // Check that the following collection stats are the same across replica set
                    // members:
                    //  capped
                    //  nindexes, except on nodes with buildIndexes: false
                    //  ns
                    const hasSecondaryIndexes = !replSetConfig ||
                        replSetConfig.members[rst.getNodeId(secondary)].buildIndexes !== false;
                    primaryCollections.forEach(collName => {
                        var primaryCollStats =
                            primary.getDB(dbName).runCommand({collStats: collName});
                        var secondaryCollStats =
                            secondary.getDB(dbName).runCommand({collStats: collName});

                        if (primaryCollStats.ok !== 1 || secondaryCollStats.ok !== 1) {
                            printCollectionInfo('primary', primary, dbName, collName);
                            printCollectionInfo('secondary', secondary, dbName, collName);
                            success = false;
                        } else if (primaryCollStats.capped !== secondaryCollStats.capped ||
                                   (hasSecondaryIndexes &&
                                    primaryCollStats.nindexes !== secondaryCollStats.nindexes) ||
                                   primaryCollStats.ns !== secondaryCollStats.ns) {
                            print(msgPrefix +
                                  ', the primary and secondary have different stats for the ' +
                                  'collection ' + dbName + '.' + collName);
                            dumpCollectionDiff(primary, secondary, dbName, collName);
                            success = false;
                        }
                    });

                    if (nonCappedCollNames.length === primaryCollections.length) {
                        // If the primary and secondary have the same hashes for all the
                        // collections in the database and there aren't any capped collections,
                        // then the hashes for the whole database should match.
                        if (primaryDBHash.md5 !== secondaryDBHash.md5) {
                            print(msgPrefix +
                                  ', the primary and secondary have a different hash for ' +
                                  'the ' + dbName + ' database: ' + tojson(dbHashes));
                            success = false;
                        }
                    }

                    if (!success) {
                        if (!hasDumpedOplog) {
                            this.dumpOplog(primary, {}, 100);
                            rst.getSecondaries().forEach(secondary =>
                                                             this.dumpOplog(secondary, {}, 100));
                            hasDumpedOplog = true;
                        }
                    }
                });
            }

            assert(success, 'dbhash mismatch between primary and secondary');
        }

        this.checkReplicaSet(checkDBHashesForReplSet, this, excludedDBs, msgPrefix, ignoreUUIDs);
    };

    this.checkOplogs = function(msgPrefix) {
        this.checkReplicaSet(checkOplogs, this, msgPrefix);
    };

    /**
     * Check oplogs on all nodes, by reading from the last time. Since the oplog is a capped
     * collection, each node may not contain the same number of entries and stop if the cursor
     * is exhausted on any node being checked.
     */
    function checkOplogs(rst, msgPrefix = 'checkOplogs') {
        var OplogReader = function(mongo) {
            this.next = function() {
                if (!this.cursor)
                    throw new Error("OplogReader is not open!");

                var nextDoc = this.cursor.next();
                if (nextDoc)
                    this.lastDoc = nextDoc;
                return nextDoc;
            };

            this.hasNext = function() {
                if (!this.cursor)
                    throw new Error("OplogReader is not open!");
                return this.cursor.hasNext();
            };

            this.query = function(ts) {
                var coll = this.getOplogColl();
                var query = {ts: {$gte: ts ? ts : new Timestamp()}};
                // Set the cursor to read backwards, from last to first. We also set the cursor not
                // to time out since it may take a while to process each batch and a test may have
                // changed "cursorTimeoutMillis" to a short time period.
                this.cursor = coll.find(query).sort({$natural: -1}).noCursorTimeout();
            };

            this.getFirstDoc = function() {
                return this.getOplogColl().find().sort({$natural: 1}).limit(-1).next();
            };

            this.getOplogColl = function() {
                return this.mongo.getDB("local")[oplogName];
            };

            this.lastDoc = null;
            this.cursor = null;
            this.mongo = mongo;
        };

        if (rst.nodes.length && rst.nodes.length > 1) {
            var readers = [];
            var smallestTS = new Timestamp(Math.pow(2, 32) - 1, Math.pow(2, 32) - 1);
            var nodes = rst.nodes;
            var rsSize = nodes.length;
            var firstReaderIndex;
            for (var i = 0; i < rsSize; i++) {
                // Arbiters have no documents in the oplog.
                const isArbiter = nodes[i].getDB('admin').isMaster('admin').arbiterOnly;
                if (isArbiter) {
                    continue;
                }

                readers[i] = new OplogReader(nodes[i]);
                var currTS = readers[i].getFirstDoc().ts;
                // Find the reader which has the smallestTS. This reader should have the most
                // number of documents in the oplog.
                if (currTS.t < smallestTS.t ||
                    (currTS.t == smallestTS.t && currTS.i < smallestTS.i)) {
                    smallestTS = currTS;
                    firstReaderIndex = i;
                }
                // Start all oplogReaders at their last document.
                readers[i].query();
            }

            // Read from the reader which has the most oplog entries.
            // Note, we read the oplog backwards from last to first.
            var firstReader = readers[firstReaderIndex];
            var prevOplogEntry;
            while (firstReader.hasNext()) {
                var oplogEntry = firstReader.next();
                for (i = 0; i < rsSize; i++) {
                    // Skip reading from this reader if the index is the same as firstReader or
                    // the cursor is exhausted.
                    if (i === firstReaderIndex || !(readers[i] && readers[i].hasNext())) {
                        continue;
                    }
                    var otherOplogEntry = readers[i].next();
                    if (!bsonBinaryEqual(oplogEntry, otherOplogEntry)) {
                        var query = prevOplogEntry ? {ts: {$lte: prevOplogEntry.ts}} : {};
                        rst.nodes.forEach(node => this.dumpOplog(node, query, 100));
                        var log = msgPrefix +
                            ", non-matching oplog entries for the following nodes: \n" +
                            firstReader.mongo.host + ": " + tojsononeline(oplogEntry) + "\n" +
                            readers[i].mongo.host + ": " + tojsononeline(otherOplogEntry);
                        assert(false, log);
                    }
                }
                prevOplogEntry = oplogEntry;
            }
        }
    }

    /**
     * Starts up a server.  Options are saved by default for subsequent starts.
     *
     *
     * Options { remember : true } re-applies the saved options from a prior start.
     * Options { noRemember : true } ignores the current properties.
     * Options { appendOptions : true } appends the current options to those remembered.
     * Options { startClean : true } clears the data directory before starting.
     *
     * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
     * @param {object} [options]
     * @param {boolean} [restart] If false, the data directory will be cleared
     *   before the server starts.  Default: false.
     *
     */
    this.start = function(n, options, restart, wait) {
        if (n.length) {
            var nodes = n;
            var started = [];

            for (var i = 0; i < nodes.length; i++) {
                if (this.start(nodes[i], Object.merge({}, options), restart, wait)) {
                    started.push(nodes[i]);
                }
            }

            return started;
        }

        // TODO: should we do something special if we don't currently know about this node?
        n = this.getNodeId(n);

        print("ReplSetTest n is : " + n);

        var defaults = {
            useHostName: this.useHostName,
            oplogSize: this.oplogSize,
            keyFile: this.keyFile,
            port: _useBridge ? _unbridgedPorts[n] : this.ports[n],
            noprealloc: "",
            smallfiles: "",
            replSet: this.useSeedList ? this.getURL() : this.name,
            dbpath: "$set-$node"
        };

        //
        // Note : this replaces the binVersion of the shared startSet() options the first time
        // through, so the full set is guaranteed to have different versions if size > 1.  If using
        // start() independently, independent version choices will be made
        //
        if (options && options.binVersion) {
            options.binVersion = MongoRunner.versionIterator(options.binVersion);
        }

        // If restarting a node, use its existing options as the defaults.
        if ((options && options.restart) || restart) {
            options = Object.merge(this.nodes[n].fullOptions, options);
        } else {
            options = Object.merge(defaults, options);
        }
        options = Object.merge(options, this.nodeOptions["n" + n]);
        delete options.rsConfig;

        options.restart = options.restart || restart;

        var pathOpts = {node: n, set: this.name};
        options.pathOpts = Object.merge(options.pathOpts || {}, pathOpts);

        // Turn off periodic noop writes for replica sets by default.
        options.setParameter = options.setParameter || {};
        options.setParameter.writePeriodicNoops = options.setParameter.writePeriodicNoops || false;
        options.setParameter.numInitialSyncAttempts =
            options.setParameter.numInitialSyncAttempts || 1;
        // We raise the number of initial sync connect attempts for tests that disallow chaining.
        // Disabling chaining can cause sync source selection to take longer so we must increase
        // the number of connection attempts.
        options.setParameter.numInitialSyncConnectAttempts =
            options.setParameter.numInitialSyncConnectAttempts || 60;

        if (tojson(options) != tojson({}))
            printjson(options);

        print("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");

        if (_useBridge) {
            var bridgeOptions = Object.merge(_bridgeOptions, options.bridgeOptions || {});
            bridgeOptions = Object.merge(bridgeOptions, {
                hostName: this.host,
                port: this.ports[n],
                // The mongod processes identify themselves to mongobridge as host:port, where the
                // host is the actual hostname of the machine and not localhost.
                dest: getHostName() + ":" + _unbridgedPorts[n],
            });

            if (jsTestOptions().networkMessageCompressors) {
                bridgeOptions["networkMessageCompressors"] =
                    jsTestOptions().networkMessageCompressors;
            }

            this.nodes[n] = new MongoBridge(bridgeOptions);
        }

        var conn = MongoRunner.runMongod(options);
        if (!conn) {
            throw new Error("Failed to start node " + n);
        }

        // Make sure to call _addPath, otherwise folders won't be cleaned.
        this._addPath(conn.dbpath);

        if (_useBridge) {
            this.nodes[n].connectToBridge();
            _unbridgedNodes[n] = conn;
        } else {
            this.nodes[n] = conn;
        }

        // Add replica set specific attributes.
        this.nodes[n].nodeId = n;

        printjson(this.nodes);

        // Clean up after noReplSet to ensure it doesn't effect future restarts.
        if (options.noReplSet) {
            this.nodes[n].fullOptions.replSet = defaults.replSet;
            delete this.nodes[n].fullOptions.noReplSet;
        }

        wait = wait || false;
        if (!wait.toFixed) {
            if (wait)
                wait = 0;
            else
                wait = -1;
        }

        if (wait >= 0) {
            // Wait for node to start up.
            _waitForHealth(this.nodes[n], Health.UP, wait);
        }

        if (_causalConsistency) {
            this.nodes[n].setCausalConsistency(true);
        }

        return this.nodes[n];
    };

    /**
     * Restarts a db without clearing the data directory by default, and using the node(s)'s
     * original startup options by default.
     *
     * Option { startClean : true } forces clearing the data directory.
     * Option { auth : Object } object that contains the auth details for admin credentials.
     *   Should contain the fields 'user' and 'pwd'
     *
     * In order not to use the original startup options, use stop() (or stopSet()) followed by
     * start() (or startSet()) without passing restart: true as part of the options.
     *
     * @param {int|conn|[int|conn]} n array or single server number (0, 1, 2, ...) or conn
     */
    this.restart = function(n, options, signal, wait) {
        // Can specify wait as third parameter, if using default signal
        if (signal == true || signal == false) {
            wait = signal;
            signal = undefined;
        }

        this.stop(n, signal, options);

        var started = this.start(n, options, true, wait);

        if (jsTestOptions().keyFile) {
            if (started.length) {
                // if n was an array of conns, start will return an array of connections
                for (var i = 0; i < started.length; i++) {
                    assert(jsTest.authenticate(started[i]), "Failed authentication during restart");
                }
            } else {
                assert(jsTest.authenticate(started), "Failed authentication during restart");
            }
        }
        return started;
    };

    this.stopMaster = function(signal, opts) {
        var master = this.getPrimary();
        var master_id = this.getNodeId(master);
        return this.stop(master_id, signal, opts);
    };

    /**
     * Stops a particular node or nodes, specified by conn or id
     *
     * @param {number|Mongo} n the index or connection object of the replica set member to stop.
     * @param {number} signal the signal number to use for killing
     * @param {Object} opts @see MongoRunner.stopMongod
     */
    this.stop = function(n, signal, opts) {
        // Flatten array of nodes to stop
        if (n.length) {
            var nodes = n;

            var stopped = [];
            for (var i = 0; i < nodes.length; i++) {
                if (this.stop(nodes[i], signal, opts))
                    stopped.push(nodes[i]);
            }

            return stopped;
        }

        // Can specify wait as second parameter, if using default signal
        if (signal == true || signal == false) {
            signal = undefined;
        }

        n = this.getNodeId(n);

        var conn = _useBridge ? _unbridgedNodes[n] : this.nodes[n];
        print('ReplSetTest stop *** Shutting down mongod in port ' + conn.port + ' ***');
        var ret = MongoRunner.stopMongod(conn, signal, opts);

        print('ReplSetTest stop *** Mongod in port ' + conn.port + ' shutdown with code (' + ret +
              ') ***');

        if (_useBridge) {
            this.nodes[n].stop();
        }

        return ret;
    };

    /**
     * Kill all members of this replica set.
     *
     * @param {number} signal The signal number to use for killing the members
     * @param {boolean} forRestart will not cleanup data directory
     * @param {Object} opts @see MongoRunner.stopMongod
     */
    this.stopSet = function(signal, forRestart, opts) {
        // Check to make sure data is the same on all nodes.
        if (!jsTest.options().skipCheckDBHashes) {
            // To skip this check add TestData.skipCheckDBHashes = true;
            // Reasons to skip this test include:
            // - the primary goes down and none can be elected (so fsync lock/unlock commands fail)
            // - the replica set is in an unrecoverable inconsistent state. E.g. the replica set
            //   is partitioned.
            //
            if (_callIsMaster() &&
                this.liveNodes.slaves.length > 0) {  // skip for single node replsets.
                // Auth only on live nodes because authutil.assertAuthenticate
                // refuses to log in live connections if some secondaries are down.
                asCluster([this.liveNodes.master, ...this.liveNodes.slaves],
                          () => this.checkReplicatedDataHashes());
            }
        }

        for (var i = 0; i < this.ports.length; i++) {
            this.stop(i, signal, opts);
        }

        if (forRestart) {
            return;
        }

        if ((!opts || !opts.noCleanData) && _alldbpaths) {
            print("ReplSetTest stopSet deleting all dbpaths");
            for (var i = 0; i < _alldbpaths.length; i++) {
                resetDbpath(_alldbpaths[i]);
            }
        }

        _forgetReplSet(this.name);

        print('ReplSetTest stopSet *** Shut down repl set - test worked ****');
    };

    /**
     * Returns whether or not this ReplSetTest uses mongobridge.
     */
    this.usesBridge = function() {
        return _useBridge;
    };

    /**
     * Wait for a state indicator to go to a particular state or states.
     *
     * @param node is a single node or list of nodes, by id or conn
     * @param state is a single state or list of states
     *
     */
    this.waitForState = function(node, state, timeout) {
        _waitForIndicator(node, state, "state", timeout);
    };

    /**
     * Waits until there is a master node.
     */
    this.waitForMaster = function(timeout) {
        var master;
        assert.soonNoExcept(function() {
            return (master = self.getPrimary());
        }, "waiting for master", timeout);

        return master;
    };

    //
    // ReplSetTest constructors
    //

    /**
     * Constructor, which initializes the ReplSetTest object by starting new instances.
     */
    function _constructStartNewInstances(opts) {
        self.name = opts.name || jsTest.name();
        print('Starting new replica set ' + self.name);

        self.useHostName = opts.useHostName == undefined ? true : opts.useHostName;
        self.host = self.useHostName ? (opts.host || getHostName()) : 'localhost';
        self.oplogSize = opts.oplogSize || 40;
        self.useSeedList = opts.useSeedList || false;
        self.keyFile = opts.keyFile;
        self.protocolVersion = opts.protocolVersion;
        self.waitForKeys = opts.waitForKeys;

        _useBridge = opts.useBridge || false;
        _bridgeOptions = opts.bridgeOptions || {};

        _causalConsistency = opts.causallyConsistent || false;

        _configSettings = opts.settings || false;

        self.nodeOptions = {};

        var numNodes;

        if (isObject(opts.nodes)) {
            var len = 0;
            for (var i in opts.nodes) {
                var options = self.nodeOptions["n" + len] =
                    Object.merge(opts.nodeOptions, opts.nodes[i]);
                if (i.startsWith("a")) {
                    options.arbiter = true;
                }

                len++;
            }

            numNodes = len;
        } else if (Array.isArray(opts.nodes)) {
            for (var i = 0; i < opts.nodes.length; i++) {
                self.nodeOptions["n" + i] = Object.merge(opts.nodeOptions, opts.nodes[i]);
            }

            numNodes = opts.nodes.length;
        } else {
            for (var i = 0; i < opts.nodes; i++) {
                self.nodeOptions["n" + i] = opts.nodeOptions;
            }

            numNodes = opts.nodes;
        }

        self.ports = allocatePorts(numNodes);
        self.nodes = [];

        if (_useBridge) {
            _unbridgedPorts = allocatePorts(numNodes);
            _unbridgedNodes = [];
        }
    }

    /**
     * Constructor, which instantiates the ReplSetTest object from an existing set.
     */
    function _constructFromExistingSeedNode(seedNode) {
        const conn = new Mongo(seedNode);
        if (jsTest.options().keyFile) {
            self.keyFile = jsTest.options().keyFile;
        }
        var conf = asCluster(conn, () => _replSetGetConfig(conn));
        print('Recreating replica set from config ' + tojson(conf));

        var existingNodes = conf.members.map(member => member.host);
        self.ports = existingNodes.map(node => node.split(':')[1]);
        self.nodes = existingNodes.map(node => new Mongo(node));
        self.waitForKeys = false;
    }

    if (typeof opts === 'string' || opts instanceof String) {
        retryOnNetworkError(function() {
            // The primary may unexpectedly step down during startup if under heavy load
            // and too slowly processing heartbeats. When it steps down, it closes all of
            // its connections.
            _constructFromExistingSeedNode(opts);
        }, 10);
    } else {
        _constructStartNewInstances(opts);
    }
};

/**
 *  Global default timeout (10 minutes).
 */
ReplSetTest.kDefaultTimeoutMS = 10 * 60 * 1000;

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
