/**
 * Starts up a sharded cluster with the given specifications. The cluster
 * will be fully operational after the execution of this constructor function.
 * 
 * @param {Object} testName Contains the key value pair for the cluster
 *   configuration. Accepted keys are:
 * 
 *   {
 *     name {string}: name for this test
 *     verbose {number}: the verbosity for the mongos
 *     keyFile {string}: the location of the keyFile
 *     chunksize {number}:
 *     nopreallocj {boolean|number}:
 * 
 *     mongos {number|Object|Array.<Object>}: number of mongos or mongos
 *       configuration object(s)(*). @see MongoRunner.runMongos
 * 
 *     rs {Object|Array.<Object>}: replica set configuration object. Can
 *       contain:
 *       {
 *         nodes {number}: number of replica members. Defaults to 3.
 *         For other options, @see ReplSetTest#start
 *       }
 * 
 *     shards {number|Object|Array.<Object>}: number of shards or shard
 *       configuration object(s)(*). @see MongoRunner.runMongod
 *     
 *     config {number|Object|Array.<Object>}: number of config server or
 *       config server configuration object(s)(*). If this field has 3 or
 *       more members, it implies other.sync = true. @see MongoRunner.runMongod
 * 
 *     (*) There are two ways For multiple configuration objects.
 *       (1) Using the object format. Example:
 * 
 *           { d0: { verbose: 5 }, d1: { auth: '' }, rs2: { oplogsize: 10 }}
 * 
 *           In this format, d = mongod, s = mongos & c = config servers
 * 
 *       (2) Using the array format. Example:
 * 
 *           [{ verbose: 5 }, { auth: '' }]
 * 
 *       Note: you can only have single server shards for array format.
 * 
 *     other: {
 *       nopreallocj: same as above
 *       rs: same as above
 *       chunksize: same as above
 *
 *       shardOptions {Object}: same as the shards property above.
 *          Can be used to specify options that are common all shards.
 * 
 *       sync {boolean}: Use SyncClusterConnection, and readies
 *          3 config servers.
 *       configOptions {Object}: same as the config property above.
 *          Can be used to specify options that are common all config servers.
 *       mongosOptions {Object}: same as the mongos property above.
 *          Can be used to specify options that are common all mongos.
 *       enableBalancer {boolean} : if true, enable the balancer
 *       manualAddShard {boolean}: shards will not be added if true.
 *
 *       // replica Set only:
 *       rsOptions {Object}: same as the rs property above. Can be used to
 *         specify options that are common all replica members.
 *       useHostname {boolean}: if true, use hostname of machine,
 *         otherwise use localhost
 *       numReplicas {number} 
 *     }
 *   }
 * 
 * Member variables:
 * s {Mongo} - connection to the first mongos
 * s0, s1, ... {Mongo} - connection to different mongos
 * rs0, rs1, ... {ReplSetTest} - test objects to replica sets
 * shard0, shard1, ... {Mongo} - connection to shards (not available for replica sets)
 * d0, d1, ... {Mongo} - same as shard0, shard1, ...
 * config0, config1, ... {Mongo} - connection to config servers
 * c0, c1, ... {Mongo} - same as config0, config1, ...
 * configRS - If the config servers are a replset, this will contain the config ReplSetTest object
 */
ShardingTest = function( testName , numShards , verboseLevel , numMongos , otherParams ){
    
    this._startTime = new Date();

    // Check if testName is an object, if so, pull params from there
    var keyFile = undefined
    var numConfigs = 3;
    otherParams = Object.merge( otherParams || {}, {} )

    if( isObject( testName ) ){
        
        var params = Object.merge( testName, {} )
        
        testName = params.name || "test"
        otherParams = Object.merge(otherParams, params);
        otherParams = Object.merge(params.other || {}, otherParams);

        numShards = otherParams.shards || 2
        verboseLevel = otherParams.verbose || 0
        numMongos = otherParams.mongos || 1
        numConfigs = otherParams.config || numConfigs;

        var tempCount = 0;
        
        // Allow specifying options like :
        // { mongos : [ { noprealloc : "" } ], config : [ { smallfiles : "" } ], shards : { rs : true, d : true } } 
        if( Array.isArray( numShards ) ){
            for( var i = 0; i < numShards.length; i++ ){
                otherParams[ "d" + i ] = numShards[i];
            }

            numShards = numShards.length;
        }
        else if( isObject( numShards ) ){
            tempCount = 0;
            for( var i in numShards ) {
                otherParams[ i ] = numShards[i];
                tempCount++;
            }
            
            numShards = tempCount;
        }
        
        if( Array.isArray( numMongos ) ){
            for( var i = 0; i < numMongos.length; i++ ) {
                otherParams[ "s" + i ] = numMongos[i];
            }
                
            numMongos = numMongos.length;
        }
        else if( isObject( numMongos ) ){
            tempCount = 0;
            for( var i in numMongos ) {
                otherParams[ i ] = numMongos[i];
                tempCount++;
            }
            
            numMongos = tempCount;
        }
        
        if( Array.isArray( numConfigs ) ){
            for( var i = 0; i < numConfigs.length; i++ ){
                otherParams[ "c" + i ] = numConfigs[i];
            }

            numConfigs = numConfigs.length
        }
        else if( isObject( numConfigs ) ){
            tempCount = 0;
            for( var i in numConfigs ) {
                otherParams[ i ] = numConfigs[i];
                tempCount++;
            }
            numConfigs = tempCount;
        }
    }

    otherParams.extraOptions = otherParams.extraOptions || {};
    otherParams.useHostname = otherParams.useHostname == undefined ?
        true : otherParams.useHostname;
    keyFile = otherParams.keyFile || otherParams.extraOptions.keyFile


    this._testName = testName
    this._otherParams = otherParams
    
    var pathOpts = this.pathOpts = { testName : testName }

    var hasRS = false
    for( var k in otherParams ){
        if( k.startsWith( "rs" ) && otherParams[k] != undefined ){
            hasRS = true
            break
        }
    }

    this._alldbpaths = []
    this._connections = []
    this._shardServers = this._connections
    this._rs = []
    this._rsObjects = []

    for ( var i = 0; i < numShards; i++ ) {
        if( otherParams.rs || otherParams["rs" + i] ){
            var setName = testName + "-rs" + i;

            rsDefaults = { useHostname : otherParams.useHostname,
                           noJournalPrealloc : otherParams.nopreallocj, 
                           oplogSize : 40,
                           pathOpts : Object.merge( pathOpts, { shard : i } )}
            
            rsDefaults = Object.merge( rsDefaults, ShardingTest.rsOptions || {} )
            rsDefaults = Object.merge( rsDefaults, otherParams.rs )
            rsDefaults = Object.merge( rsDefaults, otherParams.rsOptions )
            rsDefaults = Object.merge( rsDefaults, otherParams["rs" + i] )
            rsDefaults.nodes = rsDefaults.nodes || otherParams.numReplicas
            
            var numReplicas = rsDefaults.nodes || 3
            delete rsDefaults.nodes
            
            print( "Replica set test!" )
            
            var rs = new ReplSetTest( { name : setName , nodes : numReplicas , startPort : 31100 + ( i * 100 ), useHostName : otherParams.useHostname, keyFile : keyFile, shardSvr : true } );
            this._rs[i] = { setName : setName , test : rs , nodes : rs.startSet( rsDefaults ) , url : rs.getURL() };
            rs.initiate();
            this["rs" + i] = rs
            
            this._rsObjects[i] = rs
            
            this._alldbpaths.push( null )
            this._connections.push( null )
        }
        else {
            var options = { useHostname : otherParams.useHostname,
                            noJournalPrealloc : otherParams.nopreallocj,
                            port : 30000 + i,
                            pathOpts : Object.merge( pathOpts, { shard : i } ),
                            dbpath : "$testName$shard",
                            keyFile : keyFile
                          }
            
            options = Object.merge( options, ShardingTest.shardOptions || {} )
            
            if( otherParams.shardOptions && otherParams.shardOptions.binVersion ){
                otherParams.shardOptions.binVersion = 
                    MongoRunner.versionIterator( otherParams.shardOptions.binVersion )
            }
            
            options = Object.merge( options, otherParams.shardOptions )
            options = Object.merge( options, otherParams["d" + i] )
            
            var conn = MongoRunner.runMongod( options );
            
            this._alldbpaths.push( testName +i )
            this._connections.push( conn );
            this["shard" + i] = conn
            this["d" + i] = conn
            
            this._rs[i] = null
            this._rsObjects[i] = null
        }
    }
        
    // Do replication on replica sets if required
    for ( var i = 0; i < numShards; i++ ){
        if( ! otherParams.rs && ! otherParams["rs" + i] ) continue
        
        var rs = this._rs[i].test;
        
        rs.getMaster().getDB( "admin" ).foo.save( { x : 1 } )
        if (keyFile) {
            authutil.asCluster(rs.nodes, keyFile, function() { rs.awaitReplication(); });
        }
        rs.awaitSecondaryNodes();
        
        var rsConn = new Mongo( rs.getURL() );
        rsConn.name = rs.getURL();
        this._connections[i] = rsConn
        this["shard" + i] = rsConn
        rsConn.rs = rs
    }

    // Default to using 3-node legacy config servers if jsTestOptions().useLegacyOptions is true
    // and the user didn't explicity specify a different config server configuration
    if (jsTestOptions().useLegacyConfigServers &&
            otherParams.sync !== false &&
            (typeof otherParams.config === 'undefined' || numConfigs === 3)) {
        otherParams.sync = true;
    }

    this._configServers = []
    this._configServersAreRS = !otherParams.sync;

    if (otherParams.sync) {
        var configNames = [];
        for ( var i = 0; i < 3 ; i++ ) {

            var options = { useHostname : otherParams.useHostname,
                            noJournalPrealloc : otherParams.nopreallocj,
                            port : 29000 + i,
                            pathOpts : Object.merge( pathOpts, { config : i } ),
                            dbpath : "$testName-config$config",
                            keyFile : keyFile,
                            configsvr : ""
                          }

            options = Object.merge( options, ShardingTest.configOptions || {} )

            if( otherParams.configOptions && otherParams.configOptions.binVersion ){
                otherParams.configOptions.binVersion =
                    MongoRunner.versionIterator( otherParams.configOptions.binVersion )
            }

            options = Object.merge( options, otherParams.configOptions )
            options = Object.merge( options, otherParams["c" + i] )

            var conn = MongoRunner.runMongod( options )

            this._alldbpaths.push( testName + "-config" + i )

            this._configServers.push( conn );
            configNames.push( conn.name )
            this["config" + i] = conn
            this["c" + i] = conn
        }
        this._configDB = configNames.join( "," );
    }
    else {
        // Using replica set for config servers

        var rstOptions = { useHostName : otherParams.useHostname,
                           startPort : 29000,
                           keyFile : keyFile,
                           name: testName + "-configRS"
                      };

        // when using CSRS, always use wiredTiger as the storage engine
        var startOptions = { pathOpts: pathOpts,
                             configsvr : "",
                             noJournalPrealloc : otherParams.nopreallocj,
                             storageEngine : "wiredTiger"
                           };

        startOptions = Object.merge( startOptions, ShardingTest.configOptions || {} )

        if( otherParams.configOptions && otherParams.configOptions.binVersion ){
            otherParams.configOptions.binVersion =
                MongoRunner.versionIterator( otherParams.configOptions.binVersion )
        }

        startOptions = Object.merge( startOptions, otherParams.configOptions )
        var nodeOptions = [];
        for (var i = 0; i < numConfigs; ++i) {
            nodeOptions.push(otherParams["c" + i] || {});
        }
        rstOptions["nodes"] = nodeOptions;

        this.configRS = new ReplSetTest(rstOptions);
        this.configRS.startSet(startOptions);

        var config = this.configRS.getReplSetConfig();
        config.configsvr = true;
        this.configRS.initiate(config);

        this.configRS.getMaster(); // Wait for master to be elected before starting mongos

        this._configDB = this.configRS.getURL();
        this._configServers = this.configRS.nodes;
        for (var i = 0; i < numConfigs; ++i) {
            var conn = this._configServers[i];
            this["config" + i] = conn;
            this["c" + i] = conn;
        }
    }

    printjson("config servers: " + this._configDB);
    var connectWithRetry = function(url) {
        var conn = null;
        assert.soon( function() {
                         try {
                             conn = new Mongo(url);
                             return true;
                         } catch (e) {
                             print("Error connecting to " + url + ": " + e);
                             return false;
                         }
                     });
        return conn;
    }
    this._configConnection = connectWithRetry(this._configDB);

    print( "ShardingTest " + this._testName + " :\n" + tojson( { config : this._configDB, shards : this._connections } ) );

    if ( numMongos == 0 && !otherParams.noChunkSize ) {
        if ( keyFile ) {
            throw Error("Cannot set chunk size without any mongos when using auth");
        } else {
            this._configConnection.getDB( "config" ).settings.insert(
                { _id : "chunksize" , value : otherParams.chunksize || otherParams.chunkSize || 50 } );
        }
    }

    this._mongos = []
    this._mongoses = this._mongos
    for ( var i = 0; i < ( ( numMongos == 0 ? -1 : numMongos ) || 1 ); i++ ){
        
        var options = { useHostname : otherParams.useHostname, 
                        port : 31000 - i - 1,
                        pathOpts : Object.merge( pathOpts, { mongos : i } ),
                        configdb : this._configDB,
                        verbose : verboseLevel || 0,
                        keyFile : keyFile
                      }
        if ( ! otherParams.noChunkSize ) {
            options.chunkSize = otherParams.chunksize || otherParams.chunkSize || 50;
        }

        options = Object.merge( options, ShardingTest.mongosOptions || {} )
        
        if( otherParams.mongosOptions && otherParams.mongosOptions.binVersion ){
            otherParams.mongosOptions.binVersion = 
                MongoRunner.versionIterator( otherParams.mongosOptions.binVersion )
        }
        
        options = Object.merge( options, otherParams.mongosOptions )
        options = Object.merge( options, otherParams.extraOptions )
        options = Object.merge( options, otherParams["s" + i] )
        
        var conn = MongoRunner.runMongos( options )

        this._mongos.push( conn );
        if ( i == 0 ) this.s = conn
        this["s" + i] = conn
    }

    var admin = this.admin = this.s.getDB( "admin" );
    this.config = this.s.getDB( "config" );

    // Disable the balancer unless it is explicitly turned on
    if ( !otherParams.enableBalancer ) {
        if (keyFile) {
            authutil.assertAuthenticate(this._mongos, 'admin', {
                user: '__system',
                mechanism: 'MONGODB-CR',
                pwd: cat(keyFile).replace(/[\011-\015\040]/g, '')
            });

            try {
                this.stopBalancer();
            }
            finally {
                authutil.logout(this._mongos, 'admin');
            }
        }
        else {
            this.stopBalancer();
        }
    }

    if ( ! otherParams.manualAddShard ){
        this._shardNames = []
        var shardNames = this._shardNames
        this._connections.forEach(
            function(z){
                var n = z.name;
                if ( ! n ){
                    n = z.host;
                    if ( ! n )
                        n = z;
                }
                print( "ShardingTest " + this._testName + " going to add shard : " + n )
                x = admin.runCommand( { addshard : n } );
                printjson( x )
                assert( x.ok );
                shardNames.push( x.shardAdded )
                z.shardName = x.shardAdded
            }
        );
    }

    if (jsTestOptions().keyFile) {
        jsTest.authenticate( this._configConnection );
        jsTest.authenticateNodes( this._configServers );
        jsTest.authenticateNodes( this._mongos );
    }
}

ShardingTest.prototype.getRSEntry = function( setName ){
    for ( var i=0; i<this._rs.length; i++ )
        if ( this._rs[i].setName == setName )
            return this._rs[i];
    throw Error( "can't find rs: " + setName );
}

ShardingTest.prototype.getConfigIndex = function( config ){
    
    // Assume config is a # if not a conn object
    if( ! isObject( config ) ) config = getHostName() + ":" + config
    
    for( var i = 0; i < this._configServers.length; i++ ){
        if( connectionURLTheSame( this._configServers[i], config ) ) return i
    }
    
    return -1
}

ShardingTest.prototype.getDB = function( name ){
    return this.s.getDB( name );
}

ShardingTest.prototype.getServerName = function( dbname ){
    var x = this.config.databases.findOne( { _id : "" + dbname } );
    if ( x )
        return x.primary;
    this.config.databases.find().forEach( printjson );
    throw Error( "couldn't find dbname: " + dbname + " total: " + this.config.databases.count() );
}


ShardingTest.prototype.getNonPrimaries = function( dbname ){
    var x = this.config.databases.findOne( { _id : dbname } );
    if ( ! x ){
        this.config.databases.find().forEach( printjson );
        throw Error( "couldn't find dbname: " + dbname + " total: " + this.config.databases.count() );
    }
    
    return this.config.shards.find( { _id : { $ne : x.primary } } ).map( function(z){ return z._id; } )
}


ShardingTest.prototype.getConnNames = function(){
    var names = [];
    for ( var i=0; i<this._connections.length; i++ ){
        names.push( this._connections[i].name );
    }
    return names; 
}

ShardingTest.prototype.getServer = function( dbname ){
    var name = this.getServerName( dbname );

    var x = this.config.shards.findOne( { _id : name } );
    if ( x )
        name = x.host;

    var rsName = null;
    if ( name.indexOf( "/" ) > 0 )
	rsName = name.substring( 0 , name.indexOf( "/" ) );
    
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        if ( connectionURLTheSame( name , c.name ) || 
             connectionURLTheSame( rsName , c.name ) )
            return c;
    }
    
    throw Error( "can't find server for: " + dbname + " name:" + name );

}

ShardingTest.prototype.normalize = function( x ){
    var z = this.config.shards.findOne( { host : x } );
    if ( z )
        return z._id;
    return x;
}

ShardingTest.prototype.getOther = function( one ){
    if ( this._connections.length < 2 )
        throw Error("getOther only works with 2 servers");

    if ( one._mongo )
        one = one._mongo
    
    for( var i = 0; i < this._connections.length; i++ ){
        if( this._connections[i] != one ) return this._connections[i]
    }
    
    return null
}

ShardingTest.prototype.getAnother = function( one ){
    if(this._connections.length < 2)
        throw Error("getAnother() only works with multiple servers");
	
	if ( one._mongo )
        one = one._mongo
    
    for(var i = 0; i < this._connections.length; i++){
    	if(this._connections[i] == one)
    		return this._connections[(i + 1) % this._connections.length];
    }
}

ShardingTest.prototype.getFirstOther = function( one ){
    for ( var i=0; i<this._connections.length; i++ ){
        if ( this._connections[i] != one )
        return this._connections[i];
    }
    throw Error("impossible");
}

ShardingTest.prototype.stop = function(){
    for ( var i=0; i<this._mongos.length; i++ ){
        _stopMongoProgram( 31000 - i - 1 );
    }
    for ( var i=0; i<this._connections.length; i++){
        _stopMongoProgram( 30000 + i );
    }
    if ( this._rs ){
        for ( var i=0; i<this._rs.length; i++ ){
            if( this._rs[i] ) this._rs[i].test.stopSet( 15 );
        }
    }
    if (this._configServersAreRS) {
        this.configRS.stopSet();
    }
    else {
        // Old style config triplet
        for ( var i=0; i<this._configServers.length; i++ ){
            MongoRunner.stopMongod( this._configServers[i] )
        }
    }
    if ( this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( MongoRunner.dataPath + this._alldbpaths[i] );
        }
    }

    var timeMillis = new Date().getTime() - this._startTime.getTime();

    print('*** ShardingTest ' + this._testName + " completed successfully in " + ( timeMillis / 1000 ) + " seconds ***");
}

ShardingTest.prototype.adminCommand = function(cmd){
    var res = this.admin.runCommand( cmd );
    if ( res && res.ok == 1 )
        return true;

    throw _getErrorWithCode(res, "command " + tojson(cmd) + " failed: " + tojson(res));
}

ShardingTest.prototype._rangeToString = function(r){
    return tojsononeline( r.min ) + " -> " + tojsononeline( r.max );
}

ShardingTest.prototype.printChangeLog = function(){
    var s = this;
    this.config.changelog.find().forEach( 
        function(z){
            var msg = z.server + "\t" + z.time + "\t" + z.what;
            for ( i=z.what.length; i<15; i++ )
                msg += " ";
            msg += " " + z.ns + "\t";
            if ( z.what == "split" ){
                msg += s._rangeToString( z.details.before ) + " -->> (" + s._rangeToString( z.details.left ) + "),(" + s._rangeToString( z.details.right ) + ")";
            }
            else if (z.what == "multi-split" ){
                msg += s._rangeToString( z.details.before ) + "  -->> (" + z.details.number + "/" + z.details.of + " " + s._rangeToString( z.details.chunk ) + ")"; 
            }
            else {
                msg += tojsononeline( z.details );
            }

            print( "ShardingTest " + msg )
        }
    );

}

ShardingTest.prototype.getChunksString = function( ns ){
    var q = {}
    if ( ns )
        q.ns = ns;

    var s = "";
    this.config.chunks.find( q ).sort( { ns : 1 , min : 1 } ).forEach( 
        function(z){
            s +=  "  " + z._id + "\t" + z.lastmod.t + "|" + z.lastmod.i + "\t" + tojson(z.min) + " -> " + tojson(z.max) + " " + z.shard + "  " + z.ns + "\n";
        }
    );
    
    return s;
}

ShardingTest.prototype.printChunks = function( ns ){
    print( "ShardingTest " + this.getChunksString( ns ) );
}

ShardingTest.prototype.printShardingStatus = function(){
    printShardingStatus( this.config );
}

ShardingTest.prototype.printCollectionInfo = function( ns , msg ){
    var out = "";
    if ( msg )
        out += msg + "\n";
    out += "sharding collection info: " + ns + "\n";
    for ( var i=0; i<this._connections.length; i++ ){
        var c = this._connections[i];
        out += "  mongod " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    for ( var i=0; i<this._mongos.length; i++ ){
        var c = this._mongos[i];
        out += "  mongos " + c + " " + tojson( c.getCollection( ns ).getShardVersion() , " " , true ) + "\n";
    }
    
    out += this.getChunksString( ns );

    print( "ShardingTest " + out );
}

printShardingStatus = function( configDB , verbose ){
    // configDB is a DB object that contains the sharding metadata of interest.
    // Defaults to the db named "config" on the current connection.
    if (configDB === undefined)
        configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingStatus: this db does not have sharding enabled. be sure you are connecting to a mongos from the shell and not to a mongod." );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    configDB.shards.find().sort( { _id : 1 } ).forEach( 
        function(z){
            output( "\t" + tojsononeline( z ) );
        }
    );

    output( "  balancer:" );

    //Is the balancer currently enabled
    output( "\tCurrently enabled:  " + ( sh.getBalancerState(configDB) ? "yes" : "no" ) );

    //Is the balancer currently active
    output( "\tCurrently running:  " + ( sh.isBalancerRunning(configDB) ? "yes" : "no" ) );

    //Output details of the current balancer round
    var balLock = sh.getBalancerLockDetails(configDB)
    if ( balLock ) {
        output( "\t\tBalancer lock taken at " + balLock.when + " by " + balLock.who );
    }

    //Output the balancer window
    var balSettings = sh.getBalancerWindow(configDB)
    if ( balSettings ) {
        output( "\t\tBalancer active window is set between " +
            balSettings.start + " and " + balSettings.stop + " server local time");
    }

    //Output the list of active migrations
    var activeMigrations = sh.getActiveMigrations(configDB)
    if (activeMigrations.length > 0 ){
        output("\tCollections with active migrations: ");
        activeMigrations.forEach( function(migration){
            output("\t\t"+migration._id+ " started at " + migration.when );
        });
    }

    // Actionlog and version checking only works on 2.7 and greater
    var versionHasActionlog = false;
    var metaDataVersion = configDB.getCollection("version").findOne().currentVersion
    if ( metaDataVersion > 5 ) {
        versionHasActionlog = true;
    }
    if ( metaDataVersion == 5 ) {
        var verArray = db.serverBuildInfo().versionArray
        if (verArray[0] == 2 && verArray[1] > 6){
            versionHasActionlog = true;
        }
    }

    if ( versionHasActionlog ) {
        //Review config.actionlog for errors
        var actionReport = sh.getRecentFailedRounds(configDB);
        //Always print the number of failed rounds
        output( "\tFailed balancer rounds in last 5 attempts:  " + actionReport.count )

        //Only print the errors if there are any
        if ( actionReport.count > 0 ){
            output( "\tLast reported error:  " + actionReport.lastErr )
            output( "\tTime of Reported error:  " + actionReport.lastTime )
        }

        output("\tMigration Results for the last 24 hours: ");
        var migrations = sh.getRecentMigrations(configDB)
        if(migrations.length > 0) {
            migrations.forEach( function(x) {
                if (x._id === "Success"){
                    output( "\t\t" + x.count + " : " + x._id)
                } else {
                    output( "\t\t" + x.count + " : Failed with error '" +  x._id
                    + "', from " + x.from + " to " + x.to )
                }
            });
        } else {
                output( "\t\tNo recent migrations");
        }
    }

    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojsononeline(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" +
                    RegExp.escape(db._id) + "\\." ) } ).
                    sort( { _id : 1 } ).forEach( function( coll ){
                        if ( ! coll.dropped ){
                            output( "\t\t" + coll._id );
                            output( "\t\t\tshard key: " + tojson(coll.key) );
                            output( "\t\t\tchunks:" );

                            res = configDB.chunks.aggregate( { $match : { ns : coll._id } } ,
                                                             { $group : { _id : "$shard" ,
                                                                          cnt : { $sum : 1 } } } ,
                                                             { $project : { _id : 0 ,
                                                                            shard : "$_id" ,
                                                                            nChunks : "$cnt" } } ,
                                                             { $sort : { shard : 1 } } ).toArray();
                            var totalChunks = 0;
                            res.forEach( function(z){
                                totalChunks += z.nChunks;
                                output( "\t\t\t\t" + z.shard + "\t" + z.nChunks );
                            } )
                            
                            if ( totalChunks < 20 || verbose ){
                                configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                                    function(chunk){
                                        output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                                " on : " + chunk.shard + " " + tojson( chunk.lastmod ) + " " +
                                                ( chunk.jumbo ? "jumbo " : "" ) );
                                    }
                                );
                            }
                            else {
                                output( "\t\t\ttoo many chunks to print, use verbose if you want to force print" );
                            }

                            configDB.tags.find( { ns : coll._id } ).sort( { min : 1 } ).forEach( 
                                function( tag ) {
                                    output( "\t\t\t tag: " + tag.tag + "  " + tojson( tag.min ) + " -->> " + tojson( tag.max ) );
                                }
                            )
                        }
                    }
                )
            }
        }
    );
    
    print( raw );
}

printShardingSizes = function(){
    configDB = db.getSisterDB('config')
    
    var version = configDB.getCollection( "version" ).findOne();
    if ( version == null ){
        print( "printShardingSizes : not a shard db!" );
        return;
    }
    
    var raw = "";
    var output = function(s){
        raw += s + "\n";
    }
    output( "--- Sharding Status --- " );
    output( "  sharding version: " + tojson( configDB.getCollection( "version" ).findOne() ) );
    
    output( "  shards:" );
    var shards = {};
    configDB.shards.find().forEach( 
        function(z){
            shards[z._id] = new Mongo(z.host);
            output( "      " + tojson(z) );
        }
    );

    var saveDB = db;
    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojson(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" +
                    RegExp.escape(db._id) + "\." ) } ).
                    sort( { _id : 1 } ).forEach( function( coll ){
                        output("\t\t" + coll._id + " chunks:");
                        configDB.chunks.find( { "ns" : coll._id } ).sort( { min : 1 } ).forEach( 
                            function(chunk){
                                var mydb = shards[chunk.shard].getDB(db._id)
                                var out = mydb.runCommand({dataSize: coll._id,
                                                           keyPattern: coll.key, 
                                                           min: chunk.min,
                                                           max: chunk.max });
                                delete out.millis;
                                delete out.ok;

                                output( "\t\t\t" + tojson( chunk.min ) + " -->> " + tojson( chunk.max ) + 
                                        " on : " + chunk.shard + " " + tojson( out ) );

                            }
                        );
                    }
                )
            }
        }
    );
    
    print( raw );
}

ShardingTest.prototype.sync = function(){
    this.adminCommand( "connpoolsync" );
}

ShardingTest.prototype.onNumShards = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var num=0;
    for ( var i=0; i<this._connections.length; i++ )
        if ( this._connections[i].getDB( dbName ).getCollection( collName ).count() > 0 )
            num++;
    return num;
}


ShardingTest.prototype.shardCounts = function( collName , dbName ){
    this.sync(); // we should sync since we're going directly to mongod here
    dbName = dbName || "test";
    var counts = {}
    for ( var i=0; i<this._connections.length; i++ )
        counts[i] = this._connections[i].getDB( dbName ).getCollection( collName ).count();
    return counts;
}

ShardingTest.prototype.chunkCounts = function( collName , dbName ){
    dbName = dbName || "test";
    var x = {}

    this.config.shards.find().forEach(
        function(z){
            x[z._id] = 0;
        }
    );
    
    this.config.chunks.find( { ns : dbName + "." + collName } ).forEach(
        function(z){
            if ( x[z.shard] )
                x[z.shard]++
            else
                x[z.shard] = 1;
        }
    );
    return x;

}

ShardingTest.prototype.chunkDiff = function( collName , dbName ){
    var c = this.chunkCounts( collName , dbName );
    var min = 100000000;
    var max = 0;
    for ( var s in c ){
        if ( c[s] < min )
            min = c[s];
        if ( c[s] > max )
            max = c[s];
    }
    print( "ShardingTest input: " + tojson( c ) + " min: " + min + " max: " + max  );
    return max - min;
}

// Waits up to one minute for the difference in chunks between the most loaded shard and least
// loaded shard to be 0 or 1, indicating that the collection is well balanced.
// This should only be called after creating a big enough chunk difference to trigger balancing.
ShardingTest.prototype.awaitBalance = function( collName , dbName , timeToWait ) {
    timeToWait = timeToWait || 60000;
    var shardingTest = this;
    assert.soon( function() {
        var x = shardingTest.chunkDiff( collName , dbName );
        print( "chunk diff: " + x );
        return x < 2;
    } , "no balance happened", 60000 );

}

ShardingTest.prototype.getShard = function( coll, query, includeEmpty ){
    var shards = this.getShards( coll, query, includeEmpty )
    assert.eq( shards.length, 1 )
    return shards[0]
}

// Returns the shards on which documents matching a particular query reside
ShardingTest.prototype.getShards = function( coll, query, includeEmpty ){
    if( ! coll.getDB )
        coll = this.s.getCollection( coll )

    var explain = coll.find( query ).explain("executionStats")
    var shards = []

    var execStages = explain.executionStats.executionStages;
    var plannerShards = explain.queryPlanner.winningPlan.shards;

    if( execStages.shards ){
        for( var i = 0; i < execStages.shards.length; i++ ){
            var hasResults = execStages.shards[i].executionStages.nReturned &&
                             execStages.shards[i].executionStages.nReturned > 0;
            if( includeEmpty || hasResults ){
                shards.push(plannerShards[i].connectionString);
            }
        }
    }

    for( var i = 0; i < shards.length; i++ ){
        for( var j = 0; j < this._connections.length; j++ ){
            if ( connectionURLTheSame(  this._connections[j] , shards[i] ) ){
                shards[i] = this._connections[j]
                break;
            }
        }
    }

    return shards
}

ShardingTest.prototype.isSharded = function( collName ){
    
    var collName = "" + collName
    var dbName = undefined
    
    if( typeof collName.getCollectionNames == 'function' ){
        dbName = "" + collName
        collName = undefined
    }
    
    if( dbName ){
        var x = this.config.databases.findOne( { _id : dbname } )
        if( x ) return x.partitioned
        else return false
    }
    
    if( collName ){
        var x = this.config.collections.findOne( { _id : collName } )
        if( x ) return true
        else return false
    }
    
}

ShardingTest.prototype.shardGo = function( collName , key , split , move , dbName, waitForDelete ){

    split = ( split != false ? ( split || key ) : split )
    move = ( split != false && move != false ? ( move || split ) : false )
    
    if( collName.getDB )
        dbName = "" + collName.getDB()
    else dbName = dbName || "test";

    var c = dbName + "." + collName;
    if( collName.getDB )
        c = "" + collName

    var isEmpty = this.s.getCollection( c ).count() == 0
        
    if( ! this.isSharded( dbName ) )
        this.s.adminCommand( { enableSharding : dbName } )
    
    var result = this.s.adminCommand( { shardcollection : c , key : key } )
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
    
    if( split == false ) return
    
    result = this.s.adminCommand( { split : c , middle : split } );
    if( ! result.ok ){
        printjson( result )
        assert( false )
    }
        
    if( move == false ) return
    
    var result = null
    for( var i = 0; i < 5; i++ ){
        result = this.s.adminCommand( { movechunk : c , find : move , to : this.getOther( this.getServer( dbName ) ).name, _waitForDelete: waitForDelete } );
        if( result.ok ) break;
        sleep( 5 * 1000 );
    }
    printjson( result )
    assert( result.ok )
    
};

ShardingTest.prototype.shardColl = ShardingTest.prototype.shardGo

ShardingTest.prototype.setBalancer = function( balancer ){
    if( balancer || balancer == undefined ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: false } } , true )
    }
    else if( balancer == false ){
        this.config.settings.update( { _id: "balancer" }, { $set : { stopped: true } } , true )
    }
}

ShardingTest.prototype.stopBalancer = function( timeout, interval ) {
    this.setBalancer( false )
    
    if( typeof db == "undefined" ) db = undefined
    var oldDB = db
    
    db = this.config
    sh.waitForBalancer( false, timeout, interval )
    db = oldDB
}

ShardingTest.prototype.startBalancer = function( timeout, interval ) {
    this.setBalancer( true )
    
    if( typeof db == "undefined" ) db = undefined
    var oldDB = db
    
    db = this.config
    sh.waitForBalancer( true, timeout, interval )
    db = oldDB
}

ShardingTest.prototype.isAnyBalanceInFlight = function() {
    if ( this.config.locks.find({ _id : { $ne : "balancer" }, state : 2 }).count() > 0 )
        return true;

    var allCurrent = this.s.getDB( "admin" ).currentOp().inprog;
    for ( var i = 0; i < allCurrent.length; i++ ) {
        if ( allCurrent[i].desc &&
             allCurrent[i].desc.indexOf( "cleanupOldData" ) == 0 )
            return true;
    }
    return false;
}

/**
 * Kills the mongos with index n.
 */
ShardingTest.prototype.stopMongos = function(n) {
    MongoRunner.stopMongos(this['s' + n].port);
};

/**
 * Kills the mongod with index n.
 */
ShardingTest.prototype.stopMongod = function(n) {
    MongoRunner.stopMongod(this['d' + n].port);
};

/**
 * Restarts a previously stopped mongos using the same parameters as before.
 *
 * Warning: Overwrites the old s (if n = 0) and sn member variables.
 */
ShardingTest.prototype.restartMongos = function(n) {
    var mongos = this['s' + n];
    MongoRunner.stopMongos(mongos);
    mongos.restart = true;

    var newConn = MongoRunner.runMongos(mongos);

    this['s' + n] = newConn;
    if (n == 0) {
        this.s = newConn;
    }
};

/**
 * Restarts a previously stopped mongod using the same parameters as before.
 *
 * Warning: Overwrites the old dn member variables.
 */
ShardingTest.prototype.restartMongod = function(n) {
    var mongod = this['d' + n];
    MongoRunner.stopMongod(mongod);
    mongod.restart = true;

    var newConn = MongoRunner.runMongod(mongod);

    this['d' + n] = newConn;
};

/**
 * Helper method for setting primary shard of a database and making sure that it was successful.
 * Note: first mongos needs to be up.
 */
ShardingTest.prototype.ensurePrimaryShard = function(dbName, shardName) {
    var db = this.s0.getDB('admin');
    var res = db.adminCommand({ movePrimary: dbName, to: shardName });
    assert(res.ok || res.errmsg == "it is already the primary", tojson(res));
};
