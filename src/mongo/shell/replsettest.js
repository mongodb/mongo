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
 *     bridged {boolean}: Whether to set a mongobridge between replicas.
 *        Default: false
 *     keyFile {string}
 *     shardSvr {boolean}: Default: false
 *     startPort {number}: port offset to be used for each replica. Default: 31000
 *   }
 * 
 * Member variables:
 * numNodes {number} - number of nodes
 * nodes {Array.<Mongo>} - connection to replica set members
 */
ReplSetTest = function( opts ){
    this.name  = opts.name || "testReplSet";
    this.useHostName = opts.useHostName == undefined ? true : opts.useHostName;
    this.host  = this.useHostName ? (opts.host || getHostName()) : 'localhost';
    this.numNodes = opts.nodes || 0;
    this.oplogSize = opts.oplogSize || 40;
    this.useSeedList = opts.useSeedList || false;
    this.bridged = opts.bridged || false;
    this.ports = [];
    this.keyFile = opts.keyFile
    this.shardSvr = opts.shardSvr || false;

    this.startPort = opts.startPort || 31000;

    this.nodeOptions = {}    
    if( isObject( this.numNodes ) ){
        var len = 0
        for( var i in this.numNodes ){
            var options = this.nodeOptions[ "n" + len ] = Object.merge(opts.nodeOptions, 
                                                                       this.numNodes[i]);
            if( i.startsWith( "a" ) ) options.arbiter = true;
            len++
        }
        this.numNodes = len
    }
    else if( Array.isArray( this.numNodes ) ){
        for( var i = 0; i < this.numNodes.length; i++ )
            this.nodeOptions[ "n" + i ] = Object.merge(opts.nodeOptions, this.numNodes[i]);
        this.numNodes = this.numNodes.length
    }
    else {
        for ( var i =0; i < this.numNodes; i++ )
            this.nodeOptions[ "n" + i ] = opts.nodeOptions;
    }
    
    if(this.bridged) {
        this.bridgePorts = [];

        var allPorts = allocatePorts( this.numNodes * 2 , this.startPort );
        for(var i=0; i < this.numNodes; i++) {
            this.ports[i] = allPorts[i*2];
            this.bridgePorts[i] = allPorts[i*2 + 1];
        }

        this.initBridges();
    }
    else {
        this.ports = allocatePorts( this.numNodes , this.startPort );
    }

    this.nodes = []
    this.initLiveNodes()
    
    Object.extend( this, ReplSetTest.Health )
    Object.extend( this, ReplSetTest.State )
    
}

ReplSetTest.prototype.initBridges = function() {
    for(var i=0; i<this.ports.length; i++) {
        startMongoProgram( "mongobridge", "--port", this.bridgePorts[i], "--dest", this.host + ":" + this.ports[i] );
    }
}

// List of nodes as host:port strings.
ReplSetTest.prototype.nodeList = function() {
    var list = [];
    for(var i=0; i<this.ports.length; i++) {
      list.push( this.host + ":" + this.ports[i]);
    }

    return list;
}

// Here we store a reference to all reachable nodes.
ReplSetTest.prototype.initLiveNodes = function() {
    this.liveNodes = { master: null, slaves: [] }
}

ReplSetTest.prototype.getNodeId = function(node) {
    
    if( node.toFixed ) return parseInt( node )
    
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] == node ) return i
    }
    
    if( node instanceof ObjectId ){
        for( var i = 0; i < this.nodes.length; i++ ){
            if( this.nodes[i].runId == node ) return i
        }
    }
    
    if( node.nodeId ) return parseInt( node.nodeId )
    
    return undefined
    
}

ReplSetTest.prototype.getPort = function( n ){
    
    n = this.getNodeId( n )
    
    print( "ReplSetTest n: " + n + " ports: " + tojson( this.ports ) + "\t" + this.ports[n] + " " + typeof(n) );
    return this.ports[ n ];
}

ReplSetTest.prototype.getPath = function( n ){
    
    if( n.host )
        n = this.getNodeId( n )

    var p = "/data/db/" + this.name + "-"+n;
    if ( ! this._alldbpaths )
        this._alldbpaths = [ p ];
    else
        this._alldbpaths.push( p );
    return p;
}

ReplSetTest.prototype.getReplSetConfig = function() {
    var cfg = {};

    cfg['_id']  = this.name;
    cfg.members = [];

    for(i=0; i<this.ports.length; i++) {
        member = {};
        member['_id']  = i;

        if(this.bridged)
          var port = this.bridgePorts[i];
        else
          var port = this.ports[i];

        member['host'] = this.host + ":" + port;
        if( this.nodeOptions[ "n" + i ] && this.nodeOptions[ "n" + i ].arbiter )
            member['arbiterOnly'] = true
            
        cfg.members.push(member);
    }

    return cfg;
}

ReplSetTest.prototype.getURL = function(){
    var hosts = [];
    
    for(i=0; i<this.ports.length; i++) {

        // Don't include this node in the replica set list
        if(this.bridged && this.ports[i] == this.ports[n]) {
            continue;
        }
        
        var port;
        // Connect on the right port
        if(this.bridged) {
            port = this.bridgePorts[i];
        }
        else {
            port = this.ports[i];
        }
        
        var str = this.host + ":" + port;
        hosts.push(str);
    }
    
    return this.name + "/" + hosts.join(",");
}

ReplSetTest.prototype.getOptions = function( n , extra , putBinaryFirst ){

    if ( ! extra )
        extra = {};

    if ( ! extra.oplogSize )
        extra.oplogSize = this.oplogSize;

    var a = []


    if ( putBinaryFirst )
        a.push( "mongod" );

    if ( extra.noReplSet ) {
        delete extra.noReplSet;
    }
    else {
        a.push( "--replSet" );
        
        if( this.useSeedList ) {
            a.push( this.getURL() );
        }
        else {
            a.push( this.name );
        }
    }
    
    a.push( "--noprealloc", "--smallfiles" );

    a.push( "--rest" );

    a.push( "--port" );
    a.push( this.getPort( n ) );

    a.push( "--dbpath" );
    a.push( this.getPath( ( n.host ? this.getNodeId( n ) : n ) ) );
    
    if( this.keyFile ){
        a.push( "--keyFile" )
        a.push( keyFile )
    }        
    
    if( jsTestOptions().noJournal ) a.push( "--nojournal" )
    if( jsTestOptions().noJournalPrealloc ) a.push( "--nopreallocj" )
    if( jsTestOptions().keyFile && !this.keyFile) {
        a.push( "--keyFile" )
        a.push( jsTestOptions().keyFile )
    }
    
    for ( var k in extra ){
        var v = extra[k];
        if( k in MongoRunner.logicalOptions ) continue
        a.push( "--" + k );
        if ( v != null ){
            if( v.replace ){
                v = v.replace(/\$node/g, "" + ( n.host ? this.getNodeId( n ) : n ) )
                v = v.replace(/\$set/g, this.name )
                v = v.replace(/\$path/g, this.getPath( n ) )
            }
            a.push( v );
        }
    }

    return a;
}

ReplSetTest.prototype.startSet = function( options ) {
    
    var nodes = [];
    print( "ReplSetTest Starting Set" );

    for( n = 0 ; n < this.ports.length; n++ ) {
        node = this.start(n, options)
        nodes.push(node);
    }

    this.nodes = nodes;
    return this.nodes;
}

ReplSetTest.prototype.callIsMaster = function() {
  
  var master = null;
  this.initLiveNodes();
    
  for(var i=0; i<this.nodes.length; i++) {

    try {
      var n = this.nodes[i].getDB('admin').runCommand({ismaster:1});
      
      if(n['ismaster'] == true) {
        master = this.nodes[i]
        this.liveNodes.master = master
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
}

ReplSetTest.awaitRSClientHosts = function( conn, host, hostOk, rs, timeout ) {
    var hostCount = host.length;
    if( hostCount ){
        for( var i = 0; i < hostCount; i++ ) {
            ReplSetTest.awaitRSClientHosts( conn, host[i], hostOk, rs );
        }
        return;
    }
    
    timeout = timeout || 60 * 1000;
    
    if( hostOk == undefined ) hostOk = { ok : true }
    if( host.host ) host = host.host
    if( rs && rs.getMaster ) rs = rs.name
    
    print( "Awaiting " + host + " to be " + tojson( hostOk ) + " for " + conn + " (rs: " + rs + ")" )
    
    var tests = 0
    assert.soon( function() {
        var rsClientHosts = conn.getDB( "admin" ).runCommand( "connPoolStats" )[ "replicaSets" ]
        if( tests++ % 10 == 0 ) 
            printjson( rsClientHosts )
        
        for ( rsName in rsClientHosts ){
            if( rs && rs != rsName ) continue
            for ( var i = 0; i < rsClientHosts[rsName].hosts.length; i++ ){
                var clientHost = rsClientHosts[rsName].hosts[ i ];
                if( clientHost.addr != host ) continue
                
                // Check that *all* host properties are set correctly
                var propOk = true
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
    }, "timed out waiting for replica set client to recognize hosts", timeout )
    
}

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
              var arbiter = isMaster['arbiterOnly'] == undefined ? false : isMaster['arbiterOnly'];
              ready = ready && ( isMaster['secondary'] || arbiter );
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
}

ReplSetTest.prototype.getPrimary = ReplSetTest.prototype.getMaster

ReplSetTest.prototype.getSecondaries = function( timeout ){
    var master = this.getMaster( timeout )
    var secs = []
    for( var i = 0; i < this.nodes.length; i++ ){
        if( this.nodes[i] != master ){
            secs.push( this.nodes[i] )
        }
    }
    return secs
}

ReplSetTest.prototype.getSecondary = function( timeout ){
    return this.getSecondaries( timeout )[0];
}

ReplSetTest.prototype.status = function( timeout ){
    var master = this.callIsMaster()
    if( ! master ) master = this.liveNodes.slaves[0]
    return master.getDB("admin").runCommand({replSetGetStatus: 1})
}

// Add a node to the test set
ReplSetTest.prototype.add = function( config ) {
  if(this.ports.length == 0) {
    var nextPort = allocatePorts( 1, this.startPort )[0];
  }
  else {
    var nextPort = this.ports[this.ports.length-1] + 1;
  }
  print("ReplSetTest Next port: " + nextPort);
  this.ports.push(nextPort);
  printjson(this.ports);

  var nextId = this.nodes.length;
  printjson(this.nodes);
  print("ReplSetTest nextId:" + nextId);
  var newNode = this.start( nextId );
  
  return newNode;
}

ReplSetTest.prototype.remove = function( nodeId ) {
    nodeId = this.getNodeId( nodeId )
    this.nodes.splice( nodeId, 1 );
    this.ports.splice( nodeId, 1 );
}

ReplSetTest.prototype.initiate = function( cfg , initCmd , timeout ) {
    var master  = this.nodes[0].getDB("admin");
    var config  = cfg || this.getReplSetConfig();
    var cmd     = {};
    var cmdKey  = initCmd || 'replSetInitiate';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    assert.soon(function() {
        var result = master.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    }, "Initiate replica set", timeout);

    this.awaitSecondaryNodes();

    // Setup authentication if running test with authentication
    if (jsTestOptions().keyFile && !this.keyFile && cmdKey == 'replSetInitiate') {
        master = this.getMaster();
        jsTest.addAuth(master);
        jsTest.authenticateNodes(this.nodes);
    }
}

ReplSetTest.prototype.reInitiate = function() {
    var master  = this.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config  = this.getReplSetConfig();
    config.version = c.version + 1;
    this.initiate( config , 'replSetReconfig' );
}

ReplSetTest.prototype.getLastOpTimeWritten = function() {
    this.getMaster();
    var self = this;
    assert.soon(function() {
        try {
            var cursor = self.liveNodes.master.getDB("local")['oplog.rs'].find({});
            self.latest = cursor.sort({'$natural': -1}).limit(1).next()['ts'];
        }
        catch(e) {
            print("ReplSetTest caught exception " + e);
            return false;
        }

        return true;
    }, "awaiting oplog query", 30000);
};

ReplSetTest.prototype.awaitReplication = function(timeout) {
    timeout = timeout || 30000;

    this.getLastOpTimeWritten();

    var name = this.liveNodes.master.toString().substr(14); // strip "connection to "
    print("ReplSetTest awaitReplication: starting: timestamp for primary, " +
          name + ", is " + tojson(this.latest));

    var configVersion = this.liveNodes.master.getDB("local")['system.replset'].findOne().version;

    var self = this;
    assert.soon( function() {
                     try {
                         print("ReplSetTest awaitReplication: checking secondaries against timestamp " +
                               tojson(self.latest));
                         var secondaryCount = 0;
                         for (var i=0; i < self.liveNodes.slaves.length; i++) {
                             var slave = self.liveNodes.slaves[i];

                             // Continue if we're connected to an arbiter
                             if (res = slave.getDB("admin").runCommand({replSetGetStatus: 1})) {
                                 if (res.myState == 7) {
                                     continue;
                                 }
                             }

                             ++secondaryCount;
                             var name = slave.toString().substr(14); // strip "connection to "
                             print("ReplSetTest awaitReplication: checking secondary #" +
                                   secondaryCount + ": " + name);
                             slave.getDB("admin").getMongo().setSlaveOk();
                             var log = slave.getDB("local")['oplog.rs'];
                             if (log.find({}).sort({'$natural': -1}).limit(1).hasNext()) {
                                 var entry = log.find({}).sort({'$natural': -1}).limit(1).next();
                                 var ts = entry['ts'];
                                 if (self.latest.t < ts.t ||
                                        (self.latest.t == ts.t && self.latest.i < ts.i)) {
                                     self.latest = self.liveNodes.master.getDB("local")['oplog.rs'].
                                                        find({}).
                                                        sort({'$natural': -1}).
                                                        limit(1).
                                                        next()['ts'];
                                     print("ReplSetTest awaitReplication: timestamp for " + name +
                                           " is newer, resetting latest to " + tojson(self.latest));
                                     return false;
                                 }
                                 if (!friendlyEqual(self.latest, ts)) {
                                     print("ReplSetTest awaitReplication: timestamp for secondary #" +
                                           secondaryCount + ", " + name + ", is " + tojson(ts) +
                                           " but latest is " + tojson(self.latest));
                                     print("ReplSetTest awaitReplication: last oplog entry (of " +
                                           log.count() + ") for secondary #" + secondaryCount +
                                           ", " + name + ", is " + tojsononeline(entry));
                                     print("ReplSetTest awaitReplication: secondary #" +
                                           secondaryCount + ", " + name + ", is NOT synced");
                                     return false;
                                 }
                                 print("ReplSetTest awaitReplication: secondary #" +
                                       secondaryCount + ", " + name + ", is synced");
                             }
                             else {
                                 print("ReplSetTest awaitReplication: waiting for secondary #" +
                                       secondaryCount + ", " + name + ", to have an oplog built");
                                 return false;
                             }

                             var slaveConfigVersion = slave.getDB("local")['system.replset'].findOne().version;

                             if (configVersion != slaveConfigVersion) {
                                 print("ReplSetTest awaitReplication: secondary #" + secondaryCount +
                                       ", " + name + ", has config version #" + slaveConfigVersion +
                                       ", but expected config version #" + configVersion);
                                 return false;
                             }
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
}

ReplSetTest.prototype.getHashes = function( db ){
    this.getMaster();
    var res = {};
    res.master = this.liveNodes.master.getDB( db ).runCommand( "dbhash" )
    res.slaves = this.liveNodes.slaves.map( function(z){ return z.getDB( db ).runCommand( "dbhash" ); } )
    return res;
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
ReplSetTest.prototype.start = function( n , options , restart , wait ){
    
    if( n.length ){
        
        var nodes = n
        var started = []
        
        for( var i = 0; i < nodes.length; i++ ){
            if( this.start( nodes[i], Object.merge({}, options), restart, wait ) ){
                started.push( nodes[i] )
            }
        }
        
        return started
        
    }
    
    print( "ReplSetTest n is : " + n )
    
    defaults = { useHostName : this.useHostName,
                 oplogSize : this.oplogSize, 
                 keyFile : this.keyFile, 
                 port : this.getPort( n ),
                 noprealloc : "",
                 smallfiles : "",
                 rest : "",
                 replSet : this.useSeedList ? this.getURL() : this.name,
                 dbpath : "$set-$node" }
    
    defaults = Object.merge( defaults, ReplSetTest.nodeOptions || {} )

    // TODO : should we do something special if we don't currently know about this node?
    n = this.getNodeId( n )
    
    //
    // Note : this replaces the binVersion of the shared startSet() options the first time 
    // through, so the full set is guaranteed to have different versions if size > 1.  If using
    // start() independently, independent version choices will be made
    //
    if( options && options.binVersion ){
        options.binVersion = 
            MongoRunner.versionIterator( options.binVersion )
    }
    
    options = Object.merge( defaults, options )
    options = Object.merge( options, this.nodeOptions[ "n" + n ] )
    
    options.restart = restart
            
    var pathOpts = { node : n, set : this.name }
    options.pathOpts = Object.merge( options.pathOpts || {}, pathOpts )
    
    if( tojson(options) != tojson({}) )
        printjson(options)

    // make sure to call getPath, otherwise folders wont be cleaned
    this.getPath(n);

    print("ReplSetTest " + (restart ? "(Re)" : "") + "Starting....");
    
    var rval = this.nodes[n] = MongoRunner.runMongod( options )
    
    if( ! rval ) return rval
    
    // Add replica set specific attributes
    this.nodes[n].nodeId = n
            
    printjson( this.nodes )
        
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    if( wait < 0 ) return rval
    
    // Wait for startup
    this.waitForHealth( rval, this.UP, wait )
    
    return rval
    
}


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
        wait = signal
        signal = undefined
    }
    
    this.stop( n, signal, wait && wait.toFixed ? wait : true, options )
    started = this.start( n , options , true, wait );

    if (jsTestOptions().keyFile && !this.keyFile) {
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
}

ReplSetTest.prototype.stopMaster = function( signal , wait, opts ) {
    var master = this.getMaster();
    var master_id = this.getNodeId( master );
    return this.stop( master_id , signal , wait, opts );
}

/**
 * Stops a particular node or nodes, specified by conn or id
 *
 * @param {number} n the index of the replica set member to stop
 * @param {number} signal the signal number to use for killing
 * @param {boolean} wait
 * @param {Object} opts @see MongoRunner.stopMongod
 */
ReplSetTest.prototype.stop = function( n , signal, wait /* wait for stop */, opts ){
        
    // Flatten array of nodes to stop
    if( n.length ){
        nodes = n
        
        var stopped = []
        for( var i = 0; i < nodes.length; i++ ){
            if( this.stop( nodes[i], signal, wait, opts ) )
                stopped.push( nodes[i] )
        }
        
        return stopped
    }
    
    // Can specify wait as second parameter, if using default signal
    if( signal == true || signal == false ){
        wait = signal
        signal = undefined
    }
        
    wait = wait || false
    if( ! wait.toFixed ){
        if( wait ) wait = 0
        else wait = -1
    }
    
    var port = this.getPort( n );
    print('ReplSetTest stop *** Shutting down mongod in port ' + port + ' ***');
    var ret = MongoRunner.stopMongod( port , signal, opts );
    
    if( ! ret || wait < 0 ) return ret
    
    // Wait for shutdown
    this.waitForHealth( n, this.DOWN, wait )
    
    return true
}

/**
 * Kill all members of this replica set.
 *
 * @param {number} signal The signal number to use for killing the members
 * @param {boolean} forRestart will not cleanup data directory or teardown
 *   bridges if set to true.
 * @param {Object} opts @see MongoRunner.stopMongod
 */
ReplSetTest.prototype.stopSet = function( signal , forRestart, opts ) {
    for(var i=0; i < this.ports.length; i++) {
        this.stop( i, signal, false, opts );
    }
    if ( forRestart ) { return; }
    if ( this._alldbpaths ){
        print("ReplSetTest stopSet deleting all dbpaths");
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( this._alldbpaths[i] );
        }
    }
    if ( this.bridges ) {
        var mybridgevec;
        while (mybridgevec = this.bridges.pop()) {
            var mybridge;
            while (mybridge = mybridgevec.pop()) {
                mybridge.stop();
            }       
        }
    }
    
    print('ReplSetTest stopSet *** Shut down repl set - test worked ****' )
};


/**
 * Waits until there is a master node
 */
ReplSetTest.prototype.waitForMaster = function( timeout ){
    
    var master = undefined
    
    var self = this;
    assert.soon(function() {
        return ( master = self.getMaster() );
    }, "waiting for master", timeout);
    
    return master
}


/**
 * Wait for a health indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForHealth = function( node, state, timeout ){
    this.waitForIndicator( node, state, "health", timeout )    
}

/**
 * Wait for a state indicator to go to a particular state or states.
 * 
 * @param node is a single node or list of nodes, by id or conn
 * @param state is a single state or list of states
 * 
 */
ReplSetTest.prototype.waitForState = function( node, state, timeout ){
    this.waitForIndicator( node, state, "state", timeout )
}

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
        
        var nodes = node        
        for( var i = 0; i < nodes.length; i++ ){
            if( states.length )
                this.waitForIndicator( nodes[i], states[i], ind, timeout )
            else
                this.waitForIndicator( nodes[i], states, ind, timeout )
        }
        
        return;
    }    
    
    timeout = timeout || 30000;
    
    if( ! node.getDB ){
        node = this.nodes[node]
    }
    
    if( ! states.length ) states = [ states ]
    
    print( "ReplSetTest waitForIndicator " + ind + " on " + node )
    printjson( states )
    print( "ReplSetTest waitForIndicator from node " + node )
    
    var lastTime = null
    var currTime = new Date().getTime()
    var status = undefined

    var self = this;
    assert.soon(function() {
        
        try {
            status = self.status();
        }
        catch ( ex ) {
            print( "ReplSetTest waitForIndicator could not get status: " + tojson( ex ) );
            return false;
        }
        
        var printStatus = false
        if( lastTime == null || ( currTime = new Date().getTime() ) - (1000 * 5) > lastTime ){
            if( lastTime == null ) print( "ReplSetTest waitForIndicator Initial status ( timeout : " + timeout + " ) :" )
            printjson( status )
            lastTime = new Date().getTime()
            printStatus = true
        }

        if (typeof status.members == 'undefined') {
            return false;
        }

        for( var i = 0; i < status.members.length; i++ ){
            if( printStatus ) print( "Status for : " + status.members[i].name + ", checking " + node.host + "/" + node.name )
            if( status.members[i].name == node.host || status.members[i].name == node.name ){
                for( var j = 0; j < states.length; j++ ){
                    if( printStatus ) print( "Status " + " : " + status.members[i][ind] + "  target state : " + states[j] )
                    if( status.members[i][ind] == states[j] ) return true;
                }
            }
        }
        
        return false
        
    }, "waiting for state indicator " + ind + " for " + timeout + "ms", timeout);
    
    print( "ReplSetTest waitForIndicator final status:" )
    printjson( status )
    
}

ReplSetTest.Health = {}
ReplSetTest.Health.UP = 1
ReplSetTest.Health.DOWN = 0

ReplSetTest.State = {}
ReplSetTest.State.PRIMARY = 1
ReplSetTest.State.SECONDARY = 2
ReplSetTest.State.RECOVERING = 3
ReplSetTest.State.ARBITER = 7

/** 
 * Overflows a replica set secondary or secondaries, specified by id or conn.
 */
ReplSetTest.prototype.overflow = function( secondaries ){
    
    // Create a new collection to overflow, allow secondaries to replicate
    var master = this.getMaster()
    var overflowColl = master.getCollection( "_overflow.coll" )
    overflowColl.insert({ replicated : "value" })
    this.awaitReplication()
    
    this.stop( secondaries, undefined, 5 * 60 * 1000 )
        
    var count = master.getDB("local").oplog.rs.count();
    var prevCount = -1;
    
    // Keep inserting till we hit our capped coll limits
    while (count != prevCount) {
      
      print("ReplSetTest overflow inserting 10000");
      
      for (var i = 0; i < 10000; i++) {
          overflowColl.insert({ overflow : "value" });
      }
      prevCount = count;
      this.awaitReplication();
      
      count = master.getDB("local").oplog.rs.count();
      
      print( "ReplSetTest overflow count : " + count + " prev : " + prevCount );
      
    }
    
    // Restart all our secondaries and wait for recovery state
    this.start( secondaries, { remember : true }, true, true )
    this.waitForState( secondaries, this.RECOVERING, 5 * 60 * 1000 )
    
}




/**
 * Bridging allows you to test network partitioning.  For example, you can set
 * up a replica set, run bridge(), then kill the connection between any two
 * nodes x and y with partition(x, y).
 *
 * Once you have called bridging, you cannot reconfigure the replica set.
 */
ReplSetTest.prototype.bridge = function( opts ) {
    if (this.bridges) {
        print("ReplSetTest bridge bridges have already been created!");
        return;
    }
    
    var n = this.nodes.length;

    // create bridges
    this.bridges = [];
    for (var i=0; i<n; i++) {
        var nodeBridges = [];
        for (var j=0; j<n; j++) {
            if (i == j) {
                continue;
            }
            nodeBridges[j] = new ReplSetBridge(this, i, j);
        }
        this.bridges.push(nodeBridges);
    }
    print("ReplSetTest bridge bridges: " + this.bridges);
    
    // restart everyone independently
    this.stopSet(null, true, opts );
    for (var i=0; i<n; i++) {
        this.restart(i, {noReplSet : true});
    }
    
    // create new configs
    for (var i=0; i<n; i++) {
        config = this.nodes[i].getDB("local").system.replset.findOne();
        
        if (!config) {
            print("ReplSetTest bridge couldn't find config for "+this.nodes[i]);
            printjson(this.nodes[i].getDB("local").system.namespaces.find().toArray());
            assert(false);
        }

        var updateMod = {"$set" : {}};
        for (var j = 0; j<config.members.length; j++) {
            if (config.members[j].host == this.host+":"+this.ports[i]) {
                continue;
            }

            updateMod['$set']["members."+j+".host"] = this.bridges[i][j].host;
        }
        print("ReplSetTest bridge for node " + i + ":");
        printjson(updateMod);
        this.nodes[i].getDB("local").system.replset.update({},updateMod);
    }

    this.stopSet( null, true, opts );
    
    // start set
    for (var i=0; i<n; i++) {
        this.restart(i);
    }

    return this.getMaster();
};

/**
 * This kills the bridge between two nodes.  As parameters, specify the from and
 * to node numbers.
 *
 * For example, with a three-member replica set, we'd have nodes 0, 1, and 2,
 * with the following bridges: 0->1, 0->2, 1->0, 1->2, 2->0, 2->1.  We can kill
 * the connection between nodes 0 and 2 by calling replTest.partition(0,2) or
 * replTest.partition(2,0) (either way is identical). Then the replica set would
 * have the following bridges: 0->1, 1->0, 1->2, 2->1.
 *
 * The bidirectional parameter, which defaults to true, determines whether
 * replTest.partition(0,2) will stop the bridges for 0->2 and 2->0 (true), or
 * just 0->2 (false).
 */
ReplSetTest.prototype.partition = function(from, to, bidirectional) {
    bidirectional = typeof bidirectional !== 'undefined' ? bidirectional : true;

    this.bridges[from][to].stop();

    if (bidirectional) {
        this.bridges[to][from].stop();
    }
};

/**
 * This reverses a partition created by partition() above.
 */
ReplSetTest.prototype.unPartition = function(from, to, bidirectional) {
    bidirectional = typeof bidirectional !== 'undefined' ? bidirectional : true;

    this.bridges[from][to].start();

    if (bidirectional) {
        this.bridges[to][from].start();
    }
};
