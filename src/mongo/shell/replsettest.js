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
 *     nodeOptions {Object}: Options to apply to all nodes in the replica set.
 *        Format for Object:
 *          { cmdline-param-with-no-arg : "",
 *            param-with-arg : arg }
 *        This turns into "mongod --cmdline-param-with-no-arg --param-with-arg arg" 
 *  
 *     oplogSize {number}: Default: 40
 *     useSeedList {boolean}: Use the connection string format of this set
 *        as the replica set name (overrides the name property). Default: false
 *     keyFile {string}
 *     shardSvr {boolean}: Whether this replica set serves as a shard in a cluster. Default: false.
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
    var Health = { UP: 1, DOWN: 0 };

    var _alldbpaths;
    var _configSettings;

    // mongobridge related variables. Only available if the bridge option is selected.
    var _useBridge;
    var _bridgeOptions;
    var _unbridgedPorts;
    var _unbridgedNodes;

    // Publicly exposed variables

    /**
     * Populates a reference to all reachable nodes.
     */
    function _clearLiveNodes() {
        self.liveNodes = { master: null, slaves: [] };
    }
    
    /**
     * Returns the config document reported from the specified connection.
     */
    function _replSetGetConfig(conn) {
        return assert.commandWorked(conn.adminCommand({ replSetGetConfig: 1 })).config;
    }

    /**
     * Invokes the 'ismaster' command on each individual node and returns whether the node is the
     * current RS master.
     */
    function _callIsMaster() {
        _clearLiveNodes();

        self.nodes.forEach(function(node) {
            try {
                var n = node.getDB('admin').runCommand({ ismaster: 1 });
                if (n.ismaster == true) {
                    self.liveNodes.master = node;
                }
                else {
                    node.setSlaveOk();
                    self.liveNodes.slaves.push(node);
                }
            }
            catch (err) {
                print("ReplSetTest Could not call ismaster on node " + node + ": " + tojson(err));
            }
        });

        return self.liveNodes.master || false;
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
            for(var i = 0; i < nodes.length; i++) {
                if (states.length)
                    _waitForIndicator(nodes[i], states[i], ind, timeout);
                else
                    _waitForIndicator(nodes[i], states, ind, timeout);
            }

            return;
        }

        timeout = timeout || 30000;

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
                if (!conn) return false;

                var getStatusFunc = function() {
                    status = conn.getDB('admin').runCommand({ replSetGetStatus: 1 });
                };

                if (self.keyFile) {
                    // Authenticate connection used for running replSetGetStatus if needed
                    authutil.asCluster(conn, self.keyFile, getStatusFunc);
                } else {
                    getStatusFunc();
                }
            }
            catch (ex) {
                print("ReplSetTest waitForIndicator could not get status: " + tojson(ex));
                return false;
            }

            var printStatus = false;
            if (lastTime == null || (currTime = new Date().getTime()) - (1000 * 5) > lastTime) {
                if (lastTime == null) {
                    print("ReplSetTest waitForIndicator Initial status (timeout : "
                          + timeout + ") :");
                }

                printjson(status);
                lastTime = new Date().getTime();
                printStatus = true;
            }

            if (typeof status.members == 'undefined') {
                return false;
            }

            for(var i = 0; i < status.members.length; i++) {
                if (printStatus) {
                    print("Status for : " + status.members[i].name + ", checking " +
                            node.host + "/" + node.name);
                }

                if (status.members[i].name == node.host || status.members[i].name == node.name) {
                    for(var j = 0; j < states.length; j++) {
                        if (printStatus) {
                            print("Status -- " + " current state: " + status.members[i][ind] +
                                    ",  target state : " + states[j]);
                        }

                        if (typeof(states[j]) != "number") {
                            throw new Error("State was not an number -- type:" +
                                            typeof(states[j]) + ", value:" + states[j]);
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
     * Returns the optime for the specified host by issuing replSetGetStatus.
     */
    function _getLastOpTime(conn) {
        var replSetStatus =
            assert.commandWorked(conn.getDB("admin").runCommand({ replSetGetStatus: 1 }));
        var connStatus = replSetStatus.members.filter(m => m.self)[0];
        if (!connStatus.optime) {
            // Must be an ARBITER
            return undefined;
        }

        var myOpTime = connStatus.optime;
        return myOpTime.ts ? myOpTime.ts : myOpTime;
    }

    /**
     * Returns list of nodes as host:port strings.
     */
    this.nodeList = function() {
        var list = [];
        for(var i=0; i<this.ports.length; i++) {
            list.push(this.host + ":" + this.ports[i]);
        }

        return list;
    };

    this.getNodeId = function(node) {
        if (node.toFixed) {
            return parseInt(node);
        }

        for(var i = 0; i < this.nodes.length; i++) {
            if (this.nodes[i] == node) {
                return i;
            }
        }

        if (node instanceof ObjectId) {
            for(i = 0; i < this.nodes.length; i++) {
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

            var port = this.ports[i];
            member.host = this.host + ":" + port;

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

        if (jsTestOptions().useLegacyReplicationProtocol) {
            cfg.protocolVersion = 0;
        }

        if (_configSettings) {
            cfg.settings = _configSettings;
        }

        return cfg;
    };

    this.getURL = function() {
        var hosts = [];

        for(var i = 0; i < this.ports.length; i++) {
            hosts.push(this.host + ":" + this.ports[i]);
        }

        return this.name + "/" + hosts.join(",");
    };

    /**
     * Starts each node in the replica set with the given options.
     *
     * @param options - The options passed to {@link MongoRunner.runMongod}
     */
    this.startSet = function(options) {
        print("ReplSetTest starting set");

        var nodes = [];
        for(var n = 0 ; n < this.ports.length; n++) {
            nodes.push(this.start(n, options));
        }

        this.nodes = nodes;
        return this.nodes;
    };

    /**
     * Blocks until the secondary nodes have completed recovery and their roles are known.
     */
    this.awaitSecondaryNodes = function(timeout) {
        timeout = timeout || 60000;

        assert.soon(function() {
            // Reload who the current slaves are
            self.getPrimary(timeout);

            var slaves = self.liveNodes.slaves;
            var len = slaves.length;
            var ready = true;

            for(var i = 0; i < len; i++) {
                var isMaster = slaves[i].getDB("admin").runCommand({ ismaster: 1 });
                var arbiter = (isMaster.arbiterOnly == undefined ? false : isMaster.arbiterOnly);
                ready = ready && (isMaster.secondary || arbiter);
            }

            return ready;
        }, "Awaiting secondaries", timeout);
    };

    /**
     * Blocking call, which will wait for a primary to be elected for some pre-defined timeout and
     * if primary is available will return a connection to it. Otherwise throws an exception.
     */
    this.getPrimary = function(timeout) {
        timeout = timeout || 60000;
        var primary = null;

        assert.soon(function() {
            primary = _callIsMaster();
            return primary;
        }, "Finding primary", timeout);

        return primary;
    };

    this.awaitNoPrimary = function(msg, timeout) {
        msg = msg || "Timed out waiting for there to be no primary in replset: " + this.name;
        timeout = timeout || 30000;

        assert.soon(function() {
            return _callIsMaster() == false;
        }, msg, timeout);
    };

    this.getSecondaries = function(timeout) {
        var master = this.getPrimary(timeout);
        var secs = [];
        for(var i = 0; i < this.nodes.length; i++) {
            if (this.nodes[i] != master) {
                secs.push(this.nodes[i]);
            }
        }

        return secs;
    };

    this.getSecondary = function(timeout) {
        return this.getSecondaries(timeout)[0];
    };

    this.status = function(timeout) {
        var master = _callIsMaster();
        if (!master) {
            master = this.liveNodes.slaves[0];
        }

        return master.getDB("admin").runCommand({ replSetGetStatus: 1 });
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

    this.initiate = function(cfg, initCmd, timeout) {
        var master  = this.nodes[0].getDB("admin");
        var config  = cfg || this.getReplSetConfig();
        var cmd     = {};
        var cmdKey  = initCmd || 'replSetInitiate';
        timeout = timeout || 120000;
        if (jsTestOptions().useLegacyReplicationProtocol && !config.hasOwnProperty("protocolVersion")) {
            config.protocolVersion = 0;
        }
        cmd[cmdKey] = config;
        printjson(cmd);

        assert.commandWorked(master.runCommand(cmd), tojson(cmd));
        this.awaitSecondaryNodes(timeout);

        // Setup authentication if running test with authentication
        if ((jsTestOptions().keyFile) && cmdKey == 'replSetInitiate') {
            master = this.getPrimary();
            jsTest.authenticateNodes(this.nodes);
        }
    };

    /**
     * Gets the current replica set config from the primary.
     *
     * throws if any error occurs on the command.
     */
    this.getConfigFromPrimary = function() {
        // Use 90 seconds timeout
        return _replSetGetConfig(this.getPrimary(90 * 1000));
    };

    this.reInitiate = function() {
        var config = this.getReplSetConfig();
        var newVersion = this.getConfigFromPrimary().version + 1;
        config.version = newVersion;

        if (jsTestOptions().useLegacyReplicationProtocol && !config.hasOwnProperty("protocolVersion")) {
            config.protocolVersion = 0;
        }
        try {
            assert.commandWorked(this.getPrimary().adminCommand({replSetReconfig: config}));
        }
        catch (e) {
            if (tojson(e).indexOf("error doing query: failed") < 0) {
                throw e;
            }
        }
    };

    /**
     * Waits for the last oplog entry on the primary to be visible in the committed snapshop view
     * of the oplog on *all* secondaries.
     */
    this.awaitLastOpCommitted = function() {
        var rst = this;
        var master = rst.getPrimary();
        var lastOp = master.getDB('local').oplog.rs.find().sort({ $natural: -1 }).limit(1).next();

        var opTime;
        var filter;
        if (this.getReplSetConfig().protocolVersion === 1) {
            opTime = {ts: lastOp.ts, t: lastOp.t};
            filter = opTime;
        } else {
            opTime = {ts: lastOp.ts, t: -1};
            filter = {ts: lastOp.ts};
        }

        print("Waiting for op with OpTime " + tojson(opTime) + " to be committed on all secondaries");

        assert.soon(function() {
            for (var i = 0; i < rst.nodes.length; i++) {
                var node = rst.nodes[i];

                // Continue if we're connected to an arbiter
                var res = assert.commandWorked(node.adminCommand({ replSetGetStatus: 1 }));
                if (res.myState == ReplSetTest.State.ARBITER) {
                    continue;
                }

                res = node.getDB('local').runCommand({find: 'oplog.rs',
                                                      filter: filter,
                                                      readConcern: {level: "majority",
                                                                    afterOpTime: opTime},
                                                      maxTimeMS: 1000});
                if (!res.ok) {
                    printjson(res);
                    return false;
                }

                var cursor = new DBCommandCursor(node, res);
                if (!cursor.hasNext()) {
                    return false;
                }
            }

            return true;
        }, "Op failed to become committed on all secondaries: " + tojson(lastOp));
    };

    this.awaitReplication = function(timeout) {
        timeout = timeout || 30000;

        var masterLatestOpTime;

        // Blocking call, which will wait for the last optime written on the master to be available
        var awaitLastOpTimeWrittenFn = function() {
            var master = self.getPrimary();
            assert.soon(function() {
                try {
                    masterLatestOpTime = _getLastOpTime(master);
                }
                catch(e) {
                    print("ReplSetTest caught exception " + e);
                    return false;
                }

                return true;
            }, "awaiting oplog query", 30000);
        };

        awaitLastOpTimeWrittenFn();

        // get the latest config version from master. if there is a problem, grab master and try again
        var configVersion;
        var masterOpTime;
        var masterName;
        var master;

        try {
            master = this.getPrimary();
            configVersion = this.getConfigFromPrimary().version;
            masterOpTime = _getLastOpTime(master);
            masterName = master.toString().substr(14); // strip "connection to "
        }
        catch (e) {
            master = this.getPrimary();
            configVersion = this.getConfigFromPrimary().version;
            masterOpTime = _getLastOpTime(master);
            masterName = master.toString().substr(14); // strip "connection to "
        }

        print("ReplSetTest awaitReplication: starting: timestamp for primary, " + masterName +
                ", is " + tojson(masterLatestOpTime) +
                ", last oplog entry is " + tojsononeline(masterOpTime));

        assert.soon(function() {
             try {
                print("ReplSetTest awaitReplication: checking secondaries against timestamp " +
                      tojson(masterLatestOpTime));
                var secondaryCount = 0;
                for (var i = 0; i < self.liveNodes.slaves.length; i++) {
                    var slave = self.liveNodes.slaves[i];
                    var slaveName = slave.toString().substr(14); // strip "connection to "

                    var slaveConfigVersion =
                        slave.getDB("local")['system.replset'].findOne().version;

                    if (configVersion != slaveConfigVersion) {
                        print("ReplSetTest awaitReplication: secondary #" + secondaryCount +
                              ", " + slaveName + ", has config version #" + slaveConfigVersion +
                              ", but expected config version #" + configVersion);

                        if (slaveConfigVersion > configVersion) {
                            master = this.getPrimary();
                            configVersion = master.getDB("local")['system.replset'].findOne().version;
                            masterOpTime = _getLastOpTime(master);
                            masterName = master.toString().substr(14); // strip "connection to "

                            print("ReplSetTest awaitReplication: timestamp for primary, " +
                                  masterName + ", is " + tojson(masterLatestOpTime) +
                                  ", last oplog entry is " + tojsononeline(masterOpTime));
                        }

                        return false;
                    }

                    // Continue if we're connected to an arbiter
                    var res = assert.commandWorked(slave.adminCommand({ replSetGetStatus: 1 }));
                    if (res.myState == ReplSetTest.State.ARBITER) {
                        continue;
                    }

                    ++secondaryCount;
                    print("ReplSetTest awaitReplication: checking secondary #" +
                          secondaryCount + ": " + slaveName);

                    slave.getDB("admin").getMongo().setSlaveOk();

                    var ts = _getLastOpTime(slave);
                    if (masterLatestOpTime.t < ts.t || (masterLatestOpTime.t == ts.t && masterLatestOpTime.i < ts.i)) {
                        masterLatestOpTime = _getLastOpTime(master);
                        print("ReplSetTest awaitReplication: timestamp for " + slaveName +
                              " is newer, resetting latest to " + tojson(masterLatestOpTime));
                        return false;
                    }

                    if (!friendlyEqual(masterLatestOpTime, ts)) {
                        print("ReplSetTest awaitReplication: timestamp for secondary #" +
                              secondaryCount + ", " + slaveName + ", is " + tojson(ts) +
                              " but latest is " + tojson(masterLatestOpTime));
                        print("ReplSetTest awaitReplication: secondary #" +
                              secondaryCount + ", " + slaveName + ", is NOT synced");
                        return false;
                    }

                    print("ReplSetTest awaitReplication: secondary #" +
                          secondaryCount + ", " + slaveName + ", is synced");
                }

                print("ReplSetTest awaitReplication: finished: all " + secondaryCount +
                      " secondaries synced at timestamp " + tojson(masterLatestOpTime));
                return true;
            }
            catch (e) {
                print("ReplSetTest awaitReplication: caught exception " + e + ';\n' + e.stack);

                // We might have a new master now
                awaitLastOpTimeWrittenFn();

                print("ReplSetTest awaitReplication: resetting: timestamp for primary " +
                      self.liveNodes.master + " is " + tojson(masterLatestOpTime));

                return false;
            }
        }, "awaiting replication", timeout);
    };

    this.getHashes = function(db) {
        this.getPrimary();
        var res = {};
        res.master = this.liveNodes.master.getDB(db).runCommand("dbhash");
        res.slaves = this.liveNodes.slaves.map(function(z) { return z.getDB(db).runCommand("dbhash"); });
        return res;
    };

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
            
            for(var i = 0; i < nodes.length; i++) {
                if (this.start(nodes[i], Object.merge({}, options), restart, wait)) {
                    started.push(nodes[i]);
                }
            }
            
            return started;
        }

        // TODO: should we do something special if we don't currently know about this node?
        n = this.getNodeId(n);
        
        print("ReplSetTest n is : " + n);
        
        var defaults = { useHostName : this.useHostName,
                         oplogSize : this.oplogSize, 
                         keyFile : this.keyFile, 
                         port : _useBridge ? _unbridgedPorts[n] : this.ports[n],
                         noprealloc : "",
                         smallfiles : "",
                         replSet : this.useSeedList ? this.getURL() : this.name,
                         dbpath : "$set-$node" };

        //
        // Note : this replaces the binVersion of the shared startSet() options the first time 
        // through, so the full set is guaranteed to have different versions if size > 1.  If using
        // start() independently, independent version choices will be made
        //
        if (options && options.binVersion) {
            options.binVersion = 
                MongoRunner.versionIterator(options.binVersion);
        }

        options = Object.merge(defaults, options);
        options = Object.merge(options, this.nodeOptions["n" + n]);
        delete options.rsConfig;

        options.restart = options.restart || restart;

        var pathOpts = { node : n, set : this.name };
        options.pathOpts = Object.merge(options.pathOpts || {}, pathOpts);
        
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
            
        wait = wait || false;
        if (! wait.toFixed) {
            if (wait) wait = 0;
            else wait = -1;
        }

        if (wait >= 0) {
            // Wait for node to start up.
            _waitForHealth(this.nodes[n], Health.UP, wait);
        }

        return this.nodes[n];
    };

    /**
     * Restarts a db without clearing the data directory by default.  If the server is not
     * stopped first, this function will not work.  
     * 
     * Option { startClean : true } forces clearing the data directory.
     * Option { auth : Object } object that contains the auth details for admin credentials.
     *   Should contain the fields 'user' and 'pwd'
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
                    jsTest.authenticate(started[i]);
                }
            } else {
                jsTest.authenticate(started);
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
            for(var i = 0; i < nodes.length; i++) {
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

        var port = _useBridge ? _unbridgedPorts[n] : this.ports[n];
        print('ReplSetTest stop *** Shutting down mongod in port ' + port + ' ***');
        var ret = MongoRunner.stopMongod(port, signal, opts);

        print('ReplSetTest stop *** Mongod in port ' + port +
              ' shutdown with code (' + ret + ') ***');

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
        for(var i=0; i < this.ports.length; i++) {
            this.stop(i, signal, opts);
        }

        if (forRestart) { return; }

        if (_alldbpaths) {
            print("ReplSetTest stopSet deleting all dbpaths");
            for(var i = 0; i < _alldbpaths.length; i++) {
                resetDbpath(_alldbpaths[i]);
            }
        }

        _forgetReplSet(this.name);

        print('ReplSetTest stopSet *** Shut down repl set - test worked ****');
    };

    /**
     * Walks all oplogs and ensures matching entries.
     */
    this.ensureOplogsMatch = function() {
        var OplogReader = function(mongo) {
                this.next = function() {
                    if (!this.cursor)
                        throw Error("reader is not open!");

                    var nextDoc = this.cursor.next();
                    if (nextDoc)
                        this.lastDoc = nextDoc;
                    return nextDoc;
                };
                
                this.getLastDoc = function() {
                    if (this.lastDoc)
                        return this.lastDoc;
                    return this.next();
                };
                
                this.hasNext = function() {
                    if (!this.cursor)
                        throw Error("reader is not open!");
                    return this.cursor.hasNext();
                };
                
                this.query = function(ts) {
                    var coll = this.getOplogColl();
                    var query = {"ts": {"$gte": ts ? ts : new Timestamp()}};
                    this.cursor = coll.find(query).sort({$natural:1});
                    this.cursor.addOption(DBQuery.Option.oplogReplay);
                };
                
                this.getFirstDoc = function() {
                    return this.getOplogColl().find().sort({$natural:1}).limit(-1).next();
                };
                
                this.getOplogColl = function () {
                    return this.mongo.getDB("local")["oplog.rs"];
                };
                
                this.lastDoc = null;
                this.cursor = null;
                this.mongo = mongo;
        };

        if (this.nodes.length && this.nodes.length > 1) {
            var readers = [];
            var largestTS = null;
            var nodes = this.nodes;
            var rsSize = nodes.length;
            for (var i = 0; i < rsSize; i++) {
                readers[i] = new OplogReader(nodes[i]);
                var currTS = readers[i].getFirstDoc().ts;
                if (currTS.t > largestTS.t || (currTS.t == largestTS.t && currTS.i > largestTS.i)) {
                    largestTS = currTS;
                }
            }

            // start all oplogReaders at the same place. 
            for (i = 0; i < rsSize; i++) {
                readers[i].query(largestTS);
            }

            var firstReader = readers[0];
            while (firstReader.hasNext()) {
                var ts = firstReader.next().ts;
                for(i = 1; i < rsSize; i++) {
                    assert.eq(ts, 
                              readers[i].next().ts,
                              " non-matching ts for node: " + readers[i].mongo);
                }
            }

            // ensure no other node has more oplog
            for (i = 1; i < rsSize; i++) {
                assert.eq(false,
                          readers[i].hasNext(),
                          "" + readers[i] + " shouldn't have more oplog.");
            }
        }
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
        assert.soon(function() {
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
        self.name  = opts.name || "testReplSet";
        print('Starting new replica set ' + self.name);

        self.useHostName = opts.useHostName == undefined ? true : opts.useHostName;
        self.host  = self.useHostName ? (opts.host || getHostName()) : 'localhost';
        self.oplogSize = opts.oplogSize || 40;
        self.useSeedList = opts.useSeedList || false;
        self.keyFile = opts.keyFile;
        self.shardSvr = opts.shardSvr || false;
        self.protocolVersion = opts.protocolVersion;

        _useBridge = opts.useBridge || false;
        _bridgeOptions = opts.bridgeOptions || {};

        _configSettings = opts.settings || false;

        self.nodeOptions = {};

        var numNodes;

        if (isObject(opts.nodes)) {
            var len = 0;
            for(var i in opts.nodes) {
                var options = self.nodeOptions["n" + len] = Object.merge(opts.nodeOptions,
                                                                           opts.nodes[i]);
                if (i.startsWith("a")) {
                    options.arbiter = true;
                }

                len++;
            }

            numNodes = len;
        }
        else if (Array.isArray(opts.nodes)) {
            for(var i = 0; i < opts.nodes.length; i++) {
                self.nodeOptions["n" + i] = Object.merge(opts.nodeOptions, opts.nodes[i]);
            }

            numNodes = opts.nodes.length;
        }
        else {
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
        var conf = _replSetGetConfig(new Mongo(seedNode));
        print('Recreating replica set from config ' + tojson(conf));

        var existingNodes = conf.members.map(member => member.host);
        self.ports = existingNodes.map(node => node.split(':')[1]);
        self.nodes = existingNodes.map(node => new Mongo(node));
    }

    if (typeof opts === 'string' || opts instanceof String) {
        _constructFromExistingSeedNode(opts);
    }
    else {
        _constructStartNewInstances(opts);
    }
};

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

/**
 * Waits for the specified hosts to enter a certain state.
 */
ReplSetTest.awaitRSClientHosts = function(conn, host, hostOk, rs, timeout) {
    var hostCount = host.length;
    if (hostCount) {
        for(var i = 0; i < hostCount; i++) {
            ReplSetTest.awaitRSClientHosts(conn, host[i], hostOk, rs);
        }

        return;
    }

    timeout = timeout || 60000;

    if (hostOk == undefined) hostOk = { ok: true };
    if (host.host) host = host.host;
    if (rs) rs = rs.name;

    print("Awaiting " + host + " to be " + tojson(hostOk) + " for " + conn + " (rs: " + rs + ")");

    var tests = 0;

    assert.soon(function() {
        var rsClientHosts = conn.adminCommand('connPoolStats').replicaSets;
        if (tests++ % 10 == 0) {
            printjson(rsClientHosts);
        }

        for (var rsName in rsClientHosts) {
            if (rs && rs != rsName) continue;

            for (var i = 0; i < rsClientHosts[rsName].hosts.length; i++) {
                var clientHost = rsClientHosts[rsName].hosts[i];
                if (clientHost.addr != host) continue;

                // Check that *all* host properties are set correctly
                var propOk = true;
                for(var prop in hostOk) {
                    if (isObject(hostOk[prop])) {
                        if (!friendlyEqual(hostOk[prop], clientHost[prop])) {
                            propOk = false;
                            break;
                        }
                    }
                    else if (clientHost[prop] != hostOk[prop]) {
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
    },
    'timed out waiting for replica set client to recognize hosts',
    timeout);  
};
