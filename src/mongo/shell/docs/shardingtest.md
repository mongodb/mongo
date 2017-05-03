Mongo Shell
===

### Sharding Test 

Starts up a sharded cluster with the given specifications. The cluster will be fully operational after the execution of this constructor function.`

### Usage
`var cluster = new ShardingTest(testName)`

#### Parameters
- `testName` - Contains the key value pair for the cluster configuration. Accepted keys are:
```javascript
{ 
	name, // string - name for this test
	verbose, // number - the verbosity for the mongos
 	keyFile, // string - the location of the keyFile
 	chunksize, // number
	nopreallocj, //  boolean|number
	mongos, // number|Object|Array.<Object> - number of mongos or mongos configuration object(s)(*). 
	rs: { // Object|Array.<Object> - replica set configuration object. Can contain:
		nodes, // number - number of replica members. Defaults to 3.
	},
	shards, // number|Object|Array.<Object> - number of shards or shard configuration object(s)(*).
	config, //number|Object|Array.<Object> - number of config server or config server configuration object(s)(*). The presence of this field implies other.separateConfig = true, and if has 3 or more members, implies other.sync = true.
	/* 
 		(*) There are two ways For multiple configuration objects.
         (1) Using the object format. Example:
   
             { d0: { verbose: 5 }, d1: { auth: '' }, rs2: { oplogsize: 10 }}
   
             In this format, d = mongod, s = mongos & c = config servers
   
         (2) Using the array format. Example:
   
             [{ verbose: 5 }, { auth: '' }]
   
         Note: you can only have single server shards for array format.
 	*/ 
 	other: {
        nopreallocj, // same as above
        rs, // same as above
        chunksize, // same as above
        shardOptions, // Object - same as the shards property above. Can be used to specify options that are common all shards.
        sync, // boolean - Use SyncClusterConnection, and readies 3 config servers.
        separateConfig, // boolean - if false, recycle one of the running mongod as a config server. The config property can override this. False by default.
        configOptions, // Object - same as the config property above. Can be used to specify options that are common all config servers.
        mongosOptions, // Object - same as the mongos property above. Can be used to specify options that are common all mongos.
        enableBalancer, // boolean
        // replica Set only:
        rsOptions, // Object - same as the rs property above. Can be used to specify options that are common all replica members.
        useHostname, // boolean - if true, use hostname of machine, otherwise use localhost
		numReplicas // number 
	}
}
```