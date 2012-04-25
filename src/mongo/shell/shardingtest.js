/**
 * Starts up a sharded cluster with the given specifications. The cluster
 * will be fully operational after the execution of this constructor function.
 * 
 * @param {Object} testName Contains the key value pair for the cluster
 *   configuration. Accpeted keys are:
 * 
 *   {
 *     name {string}: name for this test
 *     verbose {number}: the verbosity for the mongos
 *     keyFile {string}: the location of the keyFile
 *     chunksize {number}:
 *     nopreallocj {boolean|number}:
 *
 *     mongos {number|Object|Array.<Object>}: number of mongos or mongos
 *       configuration object(s). @see MongoRunner.runMongos
 * 
 *     rs {Object|Array.<Object>}: replica set configuration object. Can
 *       contain:
 *       {
 *         nodes {number}: number of replica members. Defaults to 3.
 *         For other options, @see ReplSetTest#start
 *       }
 * 
 *     shards {number|Object|Array.<Object>}: number of shards or shard
 *       configuration object(s). @see MongoRunner.runMongod
 *     
 *     config {number|Object|Array.<Object>}: number of config server or
 *       config server configuration object(s). the presence of this field implies
 *       other.separateConfig = true, and if has 3 or more members, implies
 *       other.sync = true. @see MongoRunner.runMongod
 * 
 *     WARNING: use Array format for shards/config/rs/mongos when used
 *       together as they can overwrite each other's settings.
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
 *       separateConfig {boolean}: if false, recycle one of the running mongod
 *          as a config server. The config property can override this.
 *       configOptions {Object}: same as the config property above.
 *          Can be used to specify options that are common all config servers.
 *       mongosOptions {Object}: same as the mongos property above.
 *          Can be used to specify options that are common all mongos.
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
 */
ShardingTest = function( testName , numShards , verboseLevel , numMongos , otherParams ){
    
    this._startTime = new Date();

    // Check if testName is an object, if so, pull params from there
    var keyFile = undefined
    otherParams = Object.merge( otherParams || {}, {} )
    otherParams.extraOptions = otherParams.extraOptions || {}
    
    if( isObject( testName ) ){
        
        var params = Object.merge( testName, {} )
        
        testName = params.name || "test"
        
        otherParams = Object.merge( params.other || {}, {} )
        otherParams.extraOptions = otherParams.extraOptions || {}
        
        numShards = params.shards || 2
        verboseLevel = params.verbose || 0
        numMongos = params.mongos || 1
        
        keyFile = params.keyFile || otherParams.keyFile || otherParams.extraOptions.keyFile
        otherParams.nopreallocj = params.nopreallocj || otherParams.nopreallocj
        otherParams.rs = params.rs || ( params.other ? params.other.rs : undefined )
        otherParams.chunksize = params.chunksize || ( params.other ? params.other.chunksize : undefined )
        
        // Allow specifying options like :
        // { mongos : [ { noprealloc : "" } ], config : [ { smallfiles : "" } ], shards : { rs : true, d : true } } 
        if( isObject( numShards ) ){
            var len = 0
            for( var i in numShards ){
                otherParams[ "" + i ] = numShards[i]
                len++
            }
            numShards = len
        }
        
        if( isObject( numMongos ) ){
            var len = 0
            for( var i in numMongos ){
                otherParams[ "" + i ] = numMongos[i]
                len++
            }
            numMongos = len
        }
        else if( Array.isArray( numMongos ) ){
            for( var i = 0; i < numMongos.length; i++ )
                otherParams[ "s" + i ] = numMongos[i]
            numMongos = numMongos.length
        }
        
        if( isObject( params.config ) ){
            var len = 0
            for( var i in params.config ){
                otherParams[ "" + i ] = params.config[i]
                len++
            }
            
            // If we're specifying explicit config options, we need separate config servers
            otherParams.separateConfig = true
            if( len == 3 ) otherParams.sync = true
            else otherParams.sync = false
        }
        else if( Array.isArray( params.config ) ){
            for( var i = 0; i < params.config.length; i++ )
                otherParams[ "c" + i ] = params.config[i]
            
            // If we're specifying explicit config options, we need separate config servers
            otherParams.separateConfig = true
            if( params.config.length == 3 ) otherParams.sync = true
            else otherParams.sync = false
        }
        else if( params.config ) {
            
            if( params.config == 3 ){
                otherParams.separateConfig = otherParams.separateConfig || true
                otherParams.sync = true
            }
            
        }
    }
    else {
        // Handle legacy stuff
        keyFile = otherParams.extraOptions.keyFile
    }

    this._testName = testName
    this._otherParams = otherParams
    
    var pathOpts = this.pathOpts = { testName : testName }

    var hasRS = false
    for( var k in otherParams ){
        if( k.startsWith( "rs" ) ){
            hasRS = true
            break
        }
    }
    
    if( hasRS ){
        otherParams.separateConfig = true
        otherParams.useHostname = otherParams.useHostname == undefined ? true : otherParams.useHostname
    }
    
    var localhost = otherParams.useHostname ? getHostName() : "localhost";

    this._alldbpaths = []
    this._connections = []
    this._shardServers = this._connections
    this._rs = []
    this._rsObjects = []

    for ( var i = 0; i < numShards; i++ ) {
        if( otherParams.rs || otherParams["rs" + i] ){
            
            otherParams.separateConfig = true
            
            var setName = testName + "-rs" + i;
            
            rsDefaults = { useHostname : otherParams.useHostname,
                           noJournalPrealloc : otherParams.nopreallocj, 
                           oplogSize : 40,
                           pathOpts : Object.merge( pathOpts, { shard : i } )}
            
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
        rs.awaitReplication();
        
        var rsConn = new Mongo( rs.getURL() );
        rsConn.name = rs.getURL();
        this._connections[i] = rsConn
        this["shard" + i] = rsConn
        rsConn.rs = rs
    }

    this._configServers = []
    this._configNames = []
    
    if ( otherParams.sync && ! otherParams.separateConfig && numShards < 3 )
        throw "if you want sync, you need at least 3 servers";
    
    for ( var i = 0; i < ( otherParams.sync ? 3 : 1 ) ; i++ ) {
        
        var conn = null
        
        if( otherParams.separateConfig ){
            
            var options = { useHostname : otherParams.useHostname, 
                            noJournalPrealloc : otherParams.nopreallocj, 
                            port : 29000 + i,
                            pathOpts : Object.merge( pathOpts, { config : i } ),
                            dbpath : "$testName-config$config",
                            keyFile : keyFile
                          }
            
            options = Object.merge( options, otherParams.configOptions )
            options = Object.merge( options, otherParams["c" + i] )
                        
            var conn = MongoRunner.runMongod( options )
            
            // TODO:  Needed?
            this._alldbpaths.push( testName + "-config" + i )
        }
        else{
            conn = this["shard" + i]
        }
        
        this._configServers.push( conn );
        this._configNames.push( conn.name )
        this["config" + i] = conn
        this["c" + i] = conn
    }

    printjson( this._configDB = this._configNames.join( "," ) )
    this._configConnection = new Mongo( this._configDB )
    if ( ! otherParams.noChunkSize ) {
        this._configConnection.getDB( "config" ).settings.insert( { _id : "chunksize" , value : otherParams.chunksize || otherParams.chunkSize || 50 } )
    }

    print( "ShardingTest " + this._testName + " :\n" + tojson( { config : this._configDB, shards : this._connections } ) );
    
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
                shardNames.push( x.shardAdded )
                z.shardName = x.shardAdded
            }
        );
    }

    if (jsTestOptions().keyFile && !keyFile) {
        jsTest.addAuth(this._mongos[0]);
        jsTest.authenticateNodes(this._connections);
        jsTest.authenticateNodes(this._configServers);
        jsTest.authenticateNodes(this._mongos);
    }
}

ShardingTest.prototype.getRSEntry = function( setName ){
    for ( var i=0; i<this._rs.length; i++ )
        if ( this._rs[i].setName == setName )
            return this._rs[i];
    throw "can't find rs: " + setName;
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
    throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
}


ShardingTest.prototype.getNonPrimaries = function( dbname ){
    var x = this.config.databases.findOne( { _id : dbname } );
    if ( ! x ){
        this.config.databases.find().forEach( printjson );
        throw "couldn't find dbname: " + dbname + " total: " + this.config.databases.count();
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
    
    throw "can't find server for: " + dbname + " name:" + name;

}

ShardingTest.prototype.normalize = function( x ){
    var z = this.config.shards.findOne( { host : x } );
    if ( z )
        return z._id;
    return x;
}

ShardingTest.prototype.getOther = function( one ){
    if ( this._connections.length < 2 )
        throw "getOther only works with 2 servers";

    if ( one._mongo )
        one = one._mongo
    
    for( var i = 0; i < this._connections.length; i++ ){
        if( this._connections[i] != one ) return this._connections[i]
    }
    
    return null
}

ShardingTest.prototype.getAnother = function( one ){
    if(this._connections.length < 2)
    	throw "getAnother() only works with multiple servers";
	
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
    throw "impossible";
}

ShardingTest.prototype.stop = function(){
    for ( var i=0; i<this._mongos.length; i++ ){
        stopMongoProgram( 31000 - i - 1 );
    }
    for ( var i=0; i<this._connections.length; i++){
        stopMongod( 30000 + i );
    }
    if ( this._rs ){
        for ( var i=0; i<this._rs.length; i++ ){
            if( this._rs[i] ) this._rs[i].test.stopSet( 15 );
        }
    }
    if( this._otherParams.separateConfig ){
        for ( var i=0; i<this._configServers.length; i++ ){
            MongoRunner.stopMongod( this._configServers[i] )
        }
    }
    if ( this._alldbpaths ){
        for( i=0; i<this._alldbpaths.length; i++ ){
            resetDbpath( "/data/db/" + this._alldbpaths[i] );
        }
    }

    var timeMillis = new Date().getTime() - this._startTime.getTime();

    print('*** ShardingTest ' + this._testName + " completed successfully in " + ( timeMillis / 1000 ) + " seconds ***");
}

ShardingTest.prototype.adminCommand = function(cmd){
    var res = this.admin.runCommand( cmd );
    if ( res && res.ok == 1 )
        return true;

    throw "command " + tojson( cmd ) + " failed: " + tojson( res );
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

    output( "  databases:" );
    configDB.databases.find().sort( { name : 1 } ).forEach( 
        function(db){
            output( "\t" + tojsononeline(db,"",true) );
        
            if (db.partitioned){
                configDB.collections.find( { _id : new RegExp( "^" +
                    RegExp.escape(db._id) + "\\." ) } ).
                    sort( { _id : 1 } ).forEach( function( coll ){
                        if ( coll.dropped == false ){
                            output("\t\t" + coll._id + " chunks:");
                            
                            res = configDB.chunks.group( { cond : { ns : coll._id } , key : { shard : 1 },
                                reduce : function( doc , out ){ out.nChunks++; } , initial : { nChunks : 0 } } );
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

    s.config.shards.find().forEach( 
        function(z){
            x[z._id] = 0;
        }
    );
    
    s.config.chunks.find( { ns : dbName + "." + collName } ).forEach(
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

ShardingTest.prototype.getShard = function( coll, query, includeEmpty ){
    var shards = this.getShards( coll, query, includeEmpty )
    assert.eq( shards.length, 1 )
    return shards[0]
}

// Returns the shards on which documents matching a particular query reside
ShardingTest.prototype.getShards = function( coll, query, includeEmpty ){
    if( ! coll.getDB )
        coll = this.s.getCollection( coll )
    
    var explain = coll.find( query ).explain()
    var shards = []
        
    if( explain.shards ){
        
        for( var shardName in explain.shards ){           
            for( var i = 0; i < explain.shards[shardName].length; i++ ){
                if( includeEmpty || ( explain.shards[shardName][i].n && explain.shards[shardName][i].n > 0 ) )
                    shards.push( shardName )
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

ShardingTest.prototype.shardGo = function( collName , key , split , move , dbName ){
    
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
        result = this.s.adminCommand( { movechunk : c , find : move , to : this.getOther( this.getServer( dbName ) ).name } );
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
