/**
 * Sets up a replica set. To make the set running, call {@link #startSet},
 * followed by {@link #initiate} (and optionally,
 * {@link #awaitSecondaryNodes} to block till the  set is fully operational).
 * Note that some of the replica start up parameters are not passed here,
 * but to the #startSet method.
 * 
 * @param {Object} opts
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
 *     settings {object}: Setting used in the replica set config document.
 *        Example:
 *              settings: { chainingAllowed: false, ... }
 *   }
 * 
 * Member variables:
 * numNodes {number} - number of nodes
 * nodes {Array.<Mongo>} - connection to replica set members
 */
ReplSetTest = function(opts) {
    this.name  = opts.name || "testReplSet";
    this.useHostName = opts.useHostName == undefined ? true : opts.useHostName;
    this.host  = this.useHostName ? (opts.host || getHostName()) : 'localhost';
    this.oplogSize = opts.oplogSize || 40;
    this.useSeedList = opts.useSeedList || false;
    this.keyFile = opts.keyFile;
    this.shardSvr = opts.shardSvr || false;
    this.protocolVersion = opts.protocolVersion;
    this.useBridge = opts.useBridge || false;
    this.configSettings = opts.settings || false;

    this.nodeOptions = {};

    var i;
    if (isObject(opts.nodes )) {
        var len = 0;

        for(i in opts.nodes) {
            var options = this.nodeOptions[ "n" + len ] = Object.merge(opts.nodeOptions,
                                                                       opts.nodes[i]);
            if( i.startsWith( "a" ) ) {
                options.arbiter = true;
            }
            len++;
        }

        this.numNodes = len;
    }
    else if (Array.isArray(opts.nodes)) {
        for(i = 0; i < opts.nodes.length; i++) {
            this.nodeOptions[ "n" + i ] = Object.merge(opts.nodeOptions, opts.nodes[i]);
        }

        this.numNodes = opts.nodes.length;
    }
    else {
        for (i = 0; i < opts.nodes; i++) {
            this.nodeOptions[ "n" + i ] = opts.nodeOptions;
        }

        this.numNodes = opts.nodes;
    }

    this.ports = allocatePorts(this.numNodes);
    this.nodes = [];

    if (this.useBridge) {
        this._unbridgedPorts = allocatePorts(this.numNodes);
        this._unbridgedNodes = [];
    }

    this.initLiveNodes();

    Object.extend( this, ReplSetTest.Health );
    Object.extend( this, ReplSetTest.State );
};

// List of nodes as host:port strings.
ReplSetTest.prototype.nodeList = function() {
    var list = [];
    for(var i=0; i<this.ports.length; i++) {
        list.push( this.host + ":" + this.ports[i]);
    }

    return list;
};

// Here we store a reference to all reachable nodes.
ReplSetTest.prototype.initLiveNodes = function() {
    this.liveNodes = { master: null, slaves: [] };
};

ReplSetTest.prototype.getNodeId = function(node) {
    
    if( node.toFixed ) {
        return parseInt( node );
    }
    
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] == node ) {
            return i;
        }
    }
    
    if( node instanceof ObjectId ) {
        for(i = 0; i < this.nodes.length; i++){
            if( this.nodes[i].runId == node ) {
                return i;
            }
        }
    }
    
    if( node.nodeId != null ) {
        return parseInt( node.nodeId );
    }
    
    return undefined;
    
};

ReplSetTest.prototype.getPort = function( n ){
    
    n = this.getNodeId( n );
    
    print( "ReplSetTest n: " + n + " ports: " + tojson( this.ports ) + "\t" + this.ports[n] + " " + typeof(n) );
    return this.ports[ n ];
};

ReplSetTest.prototype.getPath = function( n ){
    
    if( n.host )
        n = this.getNodeId( n );

    var p = MongoRunner.dataPath + this.name + "-"+n;
    if ( ! this._alldbpaths )
        this._alldbpaths = [ p ];
    else
        this._alldbpaths.push( p );
    return p;
};

ReplSetTest.prototype.getReplSetConfig = function() {
    var cfg = {};

    cfg._id = this.name;
    if (this.protocolVersion !== undefined && this.protocolVersion !== null) {
        cfg.protocolVersion = this.protocolVersion;
    }

    cfg.members = [];

    for (var i=0; i<this.ports.length; i++) {
        member = {};
        member._id = i;

        var port = this.ports[i];

        member.host = this.host + ":" + port;
        var nodeOpts = this.nodeOptions[ "n" + i ];
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

    if (this.configSettings) {
        cfg.settings = this.configSettings;
    }
    return cfg;
};

ReplSetTest.prototype.getURL = function(){
    var hosts = [];
    
    for(var i=0; i<this.ports.length; i++) {

        var port;
        // Connect on the right port
        port = this.ports[i];
        
        var str = this.host + ":" + port;
        hosts.push(str);
    }
    
    return this.name + "/" + hosts.join(",");
};

ReplSetTest.prototype.startSet = function( options ) {
    var nodes = [];
    print( "ReplSetTest Starting Set" );

    for( var n = 0 ; n < this.ports.length; n++ ) {
        node = this.start(n, options);
        nodes.push(node);
    }

    this.nodes = nodes;
    return this.nodes;
};

ReplSetTest.prototype.callIsMaster = function() {
  
  var master = null;
  this.initLiveNodes();
    
  for(var i=0; i<this.nodes.length; i++) {
    try {
      var n = this.nodes[i].getDB('admin').runCommand({ismaster:1});
      
      if(n.ismaster == true) {
        master = this.nodes[i];
        this.liveNodes.master = master;
      }
      else {
        this.nodes[i].setSlaveOk();
        this.liveNodes.slaves.push(this.nodes[i]);
      }
    }
    catch (err) {
      print("ReplSetTest Could not call ismaster on node " + i + ": " + tojson(err));
    }
  }

  return master || false;
};

ReplSetTest.awaitRSClientHosts = function( conn, host, hostOk, rs, timeout ) {
    var hostCount = host.length;
    if( hostCount ){
        for( var i = 0; i < hostCount; i++ ) {
            ReplSetTest.awaitRSClientHosts( conn, host[i], hostOk, rs );
        }
        return;
    }
    
    timeout = timeout || 60 * 1000;
    
    if( hostOk == undefined ) hostOk = { ok : true };
    if( host.host ) host = host.host;
    if( rs && rs.getMaster ) rs = rs.name;
    
    print( "Awaiting " + host + " to be " + tojson( hostOk ) + " for " + conn + " (rs: " + rs + ")" );
    
    var tests = 0;
    assert.soon( function() {
        var rsClientHosts = conn.getDB( "admin" ).runCommand( "connPoolStats" ).replicaSets;
        if( tests++ % 10 == 0 ) 
            printjson( rsClientHosts );
        
        for ( var rsName in rsClientHosts ){
            if( rs && rs != rsName ) continue;
            for ( var i = 0; i < rsClientHosts[rsName].hosts.length; i++ ){
                var clientHost = rsClientHosts[rsName].hosts[ i ];
                if( clientHost.addr != host ) continue;
                
                // Check that *all* host properties are set correctly
                var propOk = true;
                for( var prop in hostOk ){
                    if ( isObject( hostOk[prop] )) {
                        if ( !friendlyEqual( hostOk[prop], clientHost[prop] )){
                            propOk = false;
                            break;
                        }
                    }
                    else if ( clientHost[prop] != hostOk[prop] ){
                        propOk = false;
                        break;
                    }
                }
                
                if( propOk ) return true;

            }
        }
        return false;
    }, "timed out waiting for replica set client to recognize hosts", timeout );
    
};

ReplSetTest.prototype.awaitSecondaryNodes = function( timeout ) {
  this.getMaster(); // Wait for a primary to be selected.
  var tmo = timeout || 60000;
  var replTest = this;
  assert.soon(
      function() {
          replTest.getMaster(); // Reload who the current slaves are.
          var slaves = replTest.liveNodes.slaves;
          var len = slaves.length;
          var ready = true;
          for(var i=0; i<len; i++) {
              var isMaster = slaves[i].getDB("admin").runCommand({ismaster: 1});
              var arbiter = isMaster.arbiterOnly == undefined ? false : isMaster.arbiterOnly;
              ready = ready && ( isMaster.secondary || arbiter );
          }
          return ready;
      }, "Awaiting secondaries", tmo);
};

ReplSetTest.prototype.getMaster = function( timeout ) {
  var tries = 0;
  var sleepTime = 500;
  var tmo = timeout || 60000;
  var master = null;

  try {
    var self = this;
    assert.soon(function() {
      master = self.callIsMaster();
      return master;
    }, "Finding master", tmo);
  }
  catch (err) {
    print("ReplSetTest getMaster failed: " + tojson(err));
    printStackTrace();
    throw err;
  }
  return master;
};

ReplSetTest.prototype.getPrimary = ReplSetTest.prototype.getMaster;

ReplSetTest.prototype.awaitNoPrimary = function(msg, timeout) {
    msg = msg || "Timed out waiting for there to be no primary in replset: " + this.name;
    timeout = timeout || 30000;
    var self = this;
    assert.soon(function() {
                    return self.callIsMaster() == false;
                }, msg, timeout);

};

ReplSetTest.prototype.getSecondaries = function( timeout ){
    var master = this.getMaster( timeout );
    var secs = [];
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] != master ){
            secs.push( this.nodes[i] );
        }
    }
    return secs;
};

ReplSetTest.prototype.getSecondary = function( timeout ){
    return this.getSecondaries( timeout )[0];
};

ReplSetTest.prototype.status = function( timeout ){
    var master = this.callIsMaster();
    if( ! master ) master = this.liveNodes.slaves[0];
    return master.getDB("admin").runCommand({replSetGetStatus: 1});
};

// Add a node to the test set
ReplSetTest.prototype.add = function(config) {
    var nextPort = allocatePort();
    print("ReplSetTest Next port: " + nextPort);

    this.ports.push(nextPort);
    printjson(this.ports);

    if (this.useBridge) {
        this._unbridgedPorts.push(allocatePort());
    }

    var nextId = this.nodes.length;
    printjson(this.nodes);

    print("ReplSetTest nextId: " + nextId);
    return this.start(nextId, config);
};

ReplSetTest.prototype.remove = function( nodeId ) {
    nodeId = this.getNodeId( nodeId );
    this.nodes.splice( nodeId, 1 );
    this.ports.splice( nodeId, 1 );

    if (this.useBridge) {
        this._unbridgedNodes.splice(nodeId, 1);
        this._unbridgedPorts.splice(nodeId, 1);
    }
};

ReplSetTest.prototype.initiate = function( cfg , initCmd , timeout ) {
    var master  = this.nodes[0].getDB("admin");
    var config  = cfg || this.getReplSetConfig();
    var cmd     = {};
    var cmdKey  = initCmd || 'replSetInitiate';
    timeout = timeout || 60000;
    if (jsTestOptions().useLegacyReplicationProtocol && !config.hasOwnProperty("protocolVersion")) {
        config.protocolVersion = 0;
    }
    cmd[cmdKey] = config;
    printjson(cmd);

    assert.commandWorked(master.runCommand(cmd), tojson(cmd));
    this.awaitSecondaryNodes(timeout);

    // Setup authentication if running test with authentication
    if ((jsTestOptions().keyFile) && cmdKey == 'replSetInitiate') {
        master = this.getMaster();
        jsTest.authenticateNodes(this.nodes);
    }
};

/**
 * Gets the current replica set config from the primary.
 *
 * throws if any error occurs on the command.
 */
ReplSetTest.prototype.getConfigFromPrimary = function() {
    var primary = this.getPrimary(90 * 1000 /* 90 sec timeout */);
    return assert.commandWorked(primary.getDB("admin").adminCommand("replSetGetConfig")).config;
};

// alias to match rs.conf* behavior in the shell.
ReplSetTest.prototype.conf = ReplSetTest.prototype.getConfigFromPrimary;
ReplSetTest.prototype.config = ReplSetTest.prototype.conf;

ReplSetTest.prototype.reInitiate = function() {
    "use strict";

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

ReplSetTest.prototype.getLastOpTime = function(conn) {
    var replStatus = conn.getDB("admin").runCommand("replSetGetStatus");
    var myOpTime = replStatus.members.filter(m=>m.self)[0].optime;
    return myOpTime.ts ? myOpTime.ts : myOpTime;
};

ReplSetTest.prototype.getLastOpTimeWritten = function() {
    var master = this.getMaster();
    var self = this;
    assert.soon(function() {
        try {
            self.latest = self.getLastOpTime(master);
        }
        catch(e) {
            print("ReplSetTest caught exception " + e);
            return false;
        }

        return true;
    }, "awaiting oplog query", 30000);
};

/**
 * Waits for the last oplog entry on the primary to be visible in the committed snapshop view
 * of the oplog on *all* secondaries.
 */
ReplSetTest.prototype.awaitLastOpCommitted = function() {
    var rst = this;
    var master = rst.getMaster();
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

    var isLastOpCommitted = function() {
        for (var i = 0; i < rst.nodes.length; i++) {
            var node = rst.nodes[i];

            // Continue if we're connected to an arbiter
            var res = node.getDB("admin").runCommand({replSetGetStatus: 1});
            assert.commandWorked(res);
            if (res.myState == 7) {
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
    };
    assert.soon(isLastOpCommitted,
                "Op failed to become committed on all secondaries: " + tojson(lastOp));
};

ReplSetTest.prototype.awaitReplication = function(timeout) {
    timeout = timeout || 30000;

    this.getLastOpTimeWritten();

    // get the latest config version from master. if there is a problem, grab master and try again
    var configVersion;
    var masterOpTime;
    var masterName;
    var master;
    try {
        master = this.getMaster();
        configVersion = this.conf().version;
        masterOpTime = this.getLastOpTime(master);
        masterName = master.toString().substr(14); // strip "connection to "
    }
    catch (e) {
        master = this.getMaster();
        configVersion = this.conf().version;
        masterOpTime = this.getLastOpTime(master);
        masterName = master.toString().substr(14); // strip "connection to "
    }

    print("ReplSetTest awaitReplication: starting: timestamp for primary, " + masterName +
            ", is " + tojson(this.latest) +
            ", last oplog entry is " + tojsononeline(masterOpTime));

    var self = this;
    assert.soon(function() {
         try {
             print("ReplSetTest awaitReplication: checking secondaries against timestamp " +
                   tojson(self.latest));
             var secondaryCount = 0;
             for (var i=0; i < self.liveNodes.slaves.length; i++) {
                 var slave = self.liveNodes.slaves[i];
                 var slaveName = slave.toString().substr(14); // strip "connection to "

                 var slaveConfigVersion =
                        slave.getDB("local")['system.replset'].findOne().version;

                 if (configVersion != slaveConfigVersion) {
                    print("ReplSetTest awaitReplication: secondary #" + secondaryCount +
                           ", " + slaveName + ", has config version #" + slaveConfigVersion +
                           ", but expected config version #" + configVersion);

                    if (slaveConfigVersion > configVersion) {
                        master = this.getMaster();
                        configVersion = master.getDB("local")['system.replset'].findOne().version;
                        masterOpTime = self.getLastOpTime(master);
                        masterName = master.toString().substr(14); // strip "connection to "

                        print("ReplSetTest awaitReplication: timestamp for primary, " +
                                masterName + ", is " + tojson(this.latest) +
                                ", last oplog entry is " + tojsononeline(masterOpTime));
                    }

                    return false;
                 }

                 // Continue if we're connected to an arbiter
                 if (res = slave.getDB("admin").runCommand({replSetGetStatus: 1})) {
                     if (res.myState == self.ARBITER) {
                         continue;
                     }
                 }

                 ++secondaryCount;
                 print("ReplSetTest awaitReplication: checking secondary #" +
                       secondaryCount + ": " + slaveName);

                 slave.getDB("admin").getMongo().setSlaveOk();

                 var ts = self.getLastOpTime(slave);
                 if (self.latest.t < ts.t ||
                        (self.latest.t == ts.t && self.latest.i < ts.i)) {
                     self.latest = self.getLastOpTime(master);
                     print("ReplSetTest awaitReplication: timestamp for " + slaveName +
                           " is newer, resetting latest to " + tojson(self.latest));
                     return false;
                 }

                 if (!friendlyEqual(self.latest, ts)) {
                     print("ReplSetTest awaitReplication: timestamp for secondary #" +
                           secondaryCount + ", " + slaveName + ", is " + tojson(ts) +
                           " but latest is " + tojson(self.latest));
                     print("ReplSetTest awaitReplication: secondary #" +
                           secondaryCount + ", " + slaveName + ", is NOT synced");
                     return false;
                 }

                 print("ReplSetTest awaitReplication: secondary #" +
                       secondaryCount + ", " + slaveName + ", is synced");
             }

             print("ReplSetTest awaitReplication: finished: all " + secondaryCount +
                   " secondaries synced at timestamp " + tojson(self.latest));
             return true;
         }
         catch (e) {
             print("ReplSetTest awaitReplication: caught exception: " + e);

             // we might have a new master now
             self.getLastOpTimeWritten();
             print("ReplSetTest awaitReplication: resetting: timestamp for primary " +
                   self.liveNodes.master + " is " + tojson(self.latest));
             return false;
         }
     }, "awaiting replication", timeout);
};

ReplSetTest.prototype.getHashes = function( db ){
    this.getMaster();
    var res = {};
    res.master = this.liveNodes.master.getDB( db ).runCommand( "dbhash" );
    res.slaves = this.liveNodes.slaves.map( function(z){ return z.getDB( db ).runCommand( "dbhash" ); } );
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
ReplSetTest.prototype.start = function( n , options , restart , wait ) {
    if( n.length ) {
        var nodes = n;
        var started = [];
        
        for( var i = 0; i < nodes.length; i++ ){
            if( this.start( nodes[i], Object.merge({}, options), restart, wait ) ){
                started.push( nodes[i] );
            }
        }
        
        return started;
    }

    // TODO: should we do something special if we don't currently know about this node?
    n = this.getNodeId(n);
    
    print( "ReplSetTest n is : " + n );
    
    defaults = { useHostName : this.useHostName,
                 oplogSize : this.oplogSize, 
                 keyFile : this.keyFile, 
                 port : this.useBridge ? this._unbridgedPorts[n] : this.ports[n],
                 noprealloc : "",
                 smallfiles : "",
                 replSet : this.useSeedList ? this.getURL() : this.name,
                 dbpath : "$set-$node" };
    
    defaults = Object.merge( defaults, ReplSetTest.nodeOptions || {} );
    
    //
    // Note : this replaces the binVersion of the shared startSet() options the first time 
    // through, so the full set is guaranteed to have different versions if size > 1.  If using
    // start() independently, independent version choices will be made
    //
    if( options && options.binVersion ){
        options.binVersion = 
            MongoRunner.versionIterator( options.binVersion );
    }
    
    options = Object.merge( defaults, options );
    options = Object.merge( options, this.nodeOptions[ "n" + n ] );
    delete options.rsConfig;

    options.restart = options.restart || restart;

    var pathOpts = { node : n, set : this.name };
    options.pathOpts = Object.merge( options.pathOpts || {}, pathOpts );
    
    if( tojson(options) != tojson({}) )
        printjson(options);

    // make sure to call getPath, otherwise folders wont be cleaned
    this.getPath(n);

    print("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");

    if (this.useBridge) {
        this.nodes[n] = new MongoBridge({
            hostName: this.host,
            port: this.ports[n],
            // The mongod processes identify themselves to mongobridge as host:port, where the host
            // is the actual hostname of the machine and not localhost.
            dest: getHostName() + ":" + this._unbridgedPorts[n],
        });
    }

    var conn = MongoRunner.runMongod(options);
    if (!conn) {
        throw new Error("Failed to start node " + n);
    }

    if (this.useBridge) {
        this.nodes[n].connectToBridge();
        this._unbridgedNodes[n] = conn;
    } else {
        this.nodes[n] = conn;
    }
    
    // Add replica set specific attributes.
    this.nodes[n].nodeId = n;
            
    printjson( this.nodes );
        
    wait = wait || false;
    if( ! wait.toFixed ){
        if( wait ) wait = 0;
        else wait = -1;
    }

    if (wait >= 0) {
        // Wait for node to start up.
        this.waitForHealth(this.nodes[n], this.UP, wait);
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
ReplSetTest.prototype.restart = function( n , options, signal, wait ){
    // Can specify wait as third parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal;
        signal = undefined;
    }
    
    this.stop(n, signal, options);
    started = this.start( n , options , true, wait );

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

ReplSetTest.prototype.stopMaster = function(signal, opts) {
    var master = this.getMaster();
    var master_id = this.getNodeId( master );
    return this.stop(master_id, signal, opts);
};

/**
 * Stops a particular node or nodes, specified by conn or id
 *
 * @param {number|Mongo} n the index or connection object of the replica set member to stop.
 * @param {number} signal the signal number to use for killing
 * @param {Object} opts @see MongoRunner.stopMongod
 */
ReplSetTest.prototype.stop = function(n, signal, opts) {
        
    // Flatten array of nodes to stop
    if( n.length ){
        nodes = n;
        
        var stopped = [];
        for( var i = 0; i < nodes.length; i++ ){
            if (this.stop(nodes[i], signal, opts))
                stopped.push( nodes[i] );
        }
        
        return stopped;
    }
    
    // Can specify wait as second parameter, if using default signal
    if( signal == true || signal == false ){
        signal = undefined;
    }
    
    n = this.getNodeId(n);
    var port = this.useBridge ? this._unbridgedPorts[n] : this.ports[n];
    print('ReplSetTest stop *** Shutting down mongod in port ' + port + ' ***');
    var ret = MongoRunner.stopMongod( port , signal, opts );

    print('ReplSetTest stop *** Mongod in port ' + port +
          ' shutdown with code (' + ret + ') ***');

    if (this.useBridge) {
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
ReplSetTest.prototype.stopSet = function( signal , forRestart, opts ) {
    for(var i=0; i < this.ports.length; i++) {
        this.stop(i, signal, opts);
    }
    if ( forRestart ) { return; }
    if ( this._alldbpaths ){
        print("ReplSetTest stopSet deleting all dbpaths");
        for(i=0; i<this._alldbpaths.length; i++) {
            resetDbpath( this._alldbpaths[i] );
        }
    }
    _forgetReplSet(this.name);

    print('ReplSetTest stopSet *** Shut down repl set - test worked ****' );
};

/**
 * Walks all oplogs and ensures matching entries.
 */
ReplSetTest.prototype.ensureOplogsMatch = function() {
    "use strict";
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
            
            this.getFirstDoc = function(){
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
            if (currTS.t > largestTS.t || (currTS.t == largestTS.t && currTS.i > largestTS.i) ) {
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
 * Waits until there is a master node
 */
ReplSetTest.prototype.waitForMaster = function( timeout ){
    
    var master;
    
    var self = this;
    assert.soon(function() {
        return ( master = self.getMaster() );
    }, "waiting for master", timeout);
    
    return master;
};


/**
 * Wait for a health indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states. ReplSetTest.Health.DOWN can
 *     only be used in cases when there is a primary available or slave[0] can
 *     respond to the isMaster command.
 */
ReplSetTest.prototype.waitForHealth = function( node, state, timeout ){
    this.waitForIndicator( node, state, "health", timeout );
};

/**
 * Wait for a state indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForState = function( node, state, timeout ){
    this.waitForIndicator( node, state, "state", timeout );
};

/**
 * Wait for a rs indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param states is a single state or list of states
 * @param ind is the indicator specified
 * 
 */
ReplSetTest.prototype.waitForIndicator = function( node, states, ind, timeout ){
    
    if( node.length ){
        
        var nodes = node;
        for( var i = 0; i < nodes.length; i++ ){
            if( states.length )
                this.waitForIndicator( nodes[i], states[i], ind, timeout );
            else
                this.waitForIndicator( nodes[i], states, ind, timeout );
        }
        
        return;
    }    
    
    timeout = timeout || 30000;
    
    if( ! node.getDB ){
        node = this.nodes[node];
    }
    
    if( ! states.length ) states = [ states ];
    
    print( "ReplSetTest waitForIndicator " + ind + " on " + node );
    printjson( states );
    print( "ReplSetTest waitForIndicator from node " + node );
    
    var lastTime = null;
    var currTime = new Date().getTime();
    var status;

    var self = this;
    assert.soon(function() {
        try {
            var conn = self.callIsMaster();
            if (!conn) conn = self.liveNodes.slaves[0];
            if (!conn) return false; // Try again to load connection

            var getStatusFunc = function() {
                status = conn.getDB('admin').runCommand({replSetGetStatus: 1});
            };
            if (self.keyFile) {
                // Authenticate connection used for running replSetGetStatus if needed.
                authutil.asCluster(conn, self.keyFile, getStatusFunc);
            } else {
                getStatusFunc();
            }
        }
        catch ( ex ) {
            print( "ReplSetTest waitForIndicator could not get status: " + tojson( ex ) );
            return false;
        }

        var printStatus = false;
        if( lastTime == null || ( currTime = new Date().getTime() ) - (1000 * 5) > lastTime ) {
            if( lastTime == null ) {
                print( "ReplSetTest waitForIndicator Initial status ( timeout : " +
                    timeout + " ) :" );
            }
            printjson( status );
            lastTime = new Date().getTime();
            printStatus = true;
        }

        if (typeof status.members == 'undefined') {
            return false;
        }

        for( var i = 0; i < status.members.length; i++ ) {
            if( printStatus ) {
                print( "Status for : " + status.members[i].name + ", checking " +
                        node.host + "/" + node.name );
            }
            if( status.members[i].name == node.host || status.members[i].name == node.name ) {
                for( var j = 0; j < states.length; j++ ) {
                    if( printStatus ) {
                        print( "Status -- " + " current state: " + status.members[i][ind] +
                                ",  target state : " + states[j] );
                    }

                    if (typeof(states[j]) != "number") {
                        throw new Error("State was not an number -- type:" +
                                        typeof(states[j]) + ", value:" + states[j]);
                    }
                    if( status.members[i][ind] == states[j] ) {
                        return true;
                    }
                }
            }
        }

        return false;

    }, "waiting for state indicator " + ind + " for " + timeout + "ms", timeout);

    print( "ReplSetTest waitForIndicator final status:" );
    printjson( status );
};

ReplSetTest.Health = {};
ReplSetTest.Health.UP = 1;
ReplSetTest.Health.DOWN = 0;

ReplSetTest.State = {};
ReplSetTest.State.PRIMARY = 1;
ReplSetTest.State.SECONDARY = 2;
ReplSetTest.State.RECOVERING = 3;
// Note there is no state 4.
ReplSetTest.State.STARTUP_2 = 5;
ReplSetTest.State.UNKNOWN = 6;
ReplSetTest.State.ARBITER = 7;
ReplSetTest.State.DOWN = 8;
ReplSetTest.State.ROLLBACK = 9;
ReplSetTest.State.REMOVED = 10;

/** 
 * Overflows a replica set secondary or secondaries, specified by id or conn.
 */
ReplSetTest.prototype.overflow = function( secondaries ) {
    // Create a new collection to overflow, allow secondaries to replicate
    var master = this.getMaster();
    var overflowColl = master.getCollection( "_overflow.coll" );
    overflowColl.insert({ replicated : "value" });
    this.awaitReplication();

    this.stop(secondaries);

    var count = master.getDB("local").oplog.rs.count();
    var prevCount = -1;

    // Insert batches of documents until we exceed the capped size for the oplog and truncate it.

    while (count > prevCount) {
      print("ReplSetTest overflow inserting 10000");
      var bulk = overflowColl.initializeUnorderedBulkOp();
      for (var i = 0; i < 10000; i++) {
        bulk.insert({ overflow : "Insert some large overflow value to eat oplog space faster." });
      }
      assert.writeOK(bulk.execute());

      prevCount = count;
      this.awaitReplication();
      
      count = master.getDB("local").oplog.rs.count();

      print( "ReplSetTest overflow count : " + count + " prev : " + prevCount );
    }

    // Do one writeConcern:2 write in order to ensure that all of the oplog gets propagated to the
    // secondary which is online
    assert.writeOK(
        overflowColl.insert({ overflow: "Last overflow value" }, { writeConcern: { w: 2 } }));

    // Restart all our secondaries and wait for recovery state
    this.start( secondaries, { remember : true }, true, true );
    this.waitForState( secondaries, this.RECOVERING, 5 * 60 * 1000 );
};
