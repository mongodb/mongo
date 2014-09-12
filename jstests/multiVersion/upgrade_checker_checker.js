// Ensure the upgradeCheck() functions report errors where appropriate by running a 2.0 mongod
// and connecting 2.0 mongo shell which creates a variety of documents and indexes some of which
// are intentionally malformed by the latest standards. Then the latest shell runs the
// upgradeCheck() and asserts that problems are detected when and only when they exist.

var port = allocatePorts(1)[0];
var mongodSource = MongoRunner.runMongod({binVersion : "2.0", port: port});
var dbName = "upgradeCheckerChecker";
var sourceDB = mongodSource.getDB(dbName);
var configDB = mongodSource.getDB("config");
var localDB = mongodSource.getDB("local");

// build up a string of what is to be run in the 2.0 shell
var evalStr = "db = db.getSiblingDB('" + dbName + "'); db.dropDatabase();";
// no _id index
evalStr += 'db.createCollection("cappedNoIdIndex", {capped: true, size: 10000});';

// bad index key field value
evalStr += 'db.badindex1.ensureIndex({a: "monkeysehehehe"});';

// bad index key field value as duplicate
evalStr += 'db.badindex2.ensureIndex({"b.asdf": 1});';
evalStr += 'db.badindex2.ensureIndex({"b.asdf": "asdf"});';
evalStr += 'db.badindex2.insert({a:1, b:1});';
evalStr += 'db.badindex2.insert({a:1, b:{asdf: 23}});';

// bad document _id size
evalStr += 'db.baddoc1.insert({"_id":new Array(1056).toString()});';

// bad document index key size
evalStr += 'db.baddoc2.ensureIndex({a:1});';
evalStr += 'db.baddoc2.insert({a: new Array(1056).toString()});';

// bad document field name .
evalStr += 'db.baddoc3._validateForStorage = function() {};';
evalStr += 'db.baddoc3._validateObject = function() {};';
evalStr += 'db.baddoc3.insert({asdf:{"r.e.d":1}});';

// bad document field name $
evalStr += 'db.baddoc4._validateForStorage = function() {};';
evalStr += 'db.baddoc4._validateObject = function() {};';
evalStr += 'db.baddoc4.insert({asdf:{"$red":1}});';

// good collection
evalStr += 'db.good.insert({a:1, b:2});';
evalStr += 'db.good.ensureIndex({a:1, b:1});';

// good collection with old style index
evalStr += 'db.good2.insert({a:1, b:2});';
evalStr += 'db.good2.ensureIndex({a:1, b:1}, {v:0});';
// this document shouldn't be an issue as we do not check v0 indexes
evalStr += 'db.good2.insert({a:new Array(1056).toString(), b:2});';

// make sure a max size document and a large index do not cause us to hit BSON limit in the checker
evalStr += 'db.good3.insert({c: new Array(1024*1024*16-29).toString()});';
evalStr += 'db.good3.ensureIndex({asdfasdfasdfasdfasdfasdfasdf:1, basdfadfasdfasdfadfasdfasdf:1,' +
                                'casdfasdfasdfasdfasdfasdfasdf:1, dasdfasdfasdfasdfasdfasdfasd:1,' +
                                'easdfasdfasdfasdfasdfasdfasdf:1, fasdfasdfasdfasdfasdfasdfasd:1,' +
                                'gasdfasdfasdfasdfasdfasdfasdf:1, hasdfadsfasdfasdfasdfadsfas:1});';

// ensure lack of _id index is not a problem in system collections (containing $ or system.)
evalStr += 'db.createCollection("$cappedNoIdIndex", {capped: true, size: 10000});';
evalStr += 'db.createCollection("system.cappedNoIdIndex", {capped: true, size: 10000});';
// ensure bad document checks are not run on system collections (containing $ or beginning system.)
evalStr += 'db.$cappedNoIdIndex._validateForStorage = function() {};';
evalStr += 'db.$cappedNoIdIndex._validateObject = function() {};';
evalStr += 'db.$cappedNoIdIndex.insert({asdf:{"r.e.d":1}});';
evalStr += 'db.system.cappedNoIdIndex._validateForStorage = function() {};';
evalStr += 'db.system.cappedNoIdIndex._validateObject = function() {};';
evalStr += 'db.system.cappedNoIdIndex.insert({asdf:{"r.e.d":1}});';

// ensure lack of _id index is not a problem in collections in config database
evalStr += 'db = db.getSiblingDB("config");';
evalStr += 'db.createCollection("cappedNoIdIndex", {capped: true, size: 10000});';
// ensure bad document checks are not run on config database
evalStr += 'db.coll._validateForStorage = function() {};';
evalStr += 'db.coll._validateObject = function() {};';
evalStr += 'db.coll.insert({asdf:{"r.e.d":1}});';


// ensure lack of _id index is not a problem in local.oplog.* or local.startup_log
evalStr += 'db = db.getSiblingDB("local");';
evalStr += 'db.createCollection("oplog.rs", {capped: true, size: 10000});';
evalStr += 'db.createCollection("oplog.$main", {capped: true, size: 10000});';
evalStr += 'db.createCollection("startup_log", {capped: true, size: 10000});';
// ensure bad documents are not a problem in local.oplog.*
evalStr += 'db.oplog.rs._validateForStorage =function() {};';
evalStr += 'db.oplog.rs._validateObject = function() {};';
evalStr += 'db.oplog.rs.insert({asdf:{"r.e.d":1}});';
evalStr += 'db.oplog.$main._validateForStorage =function() {};';
evalStr += 'db.oplog.$main._validateObject = function() {};';
evalStr += 'db.oplog.$main.insert({asdf:{"r.e.d":1}});';

var shellTwoOh = MongoRunner.runMongoTool("mongo", {binVersion:"2.0", port: port, eval:evalStr});

// now check each collection and fulldb to see that upgradeCheck returns false
assert(!sourceDB.upgradeCheck({collection:"cappedNoIdIndex"}));
assert(!sourceDB.upgradeCheck({collection:"badindex1"}));
assert(!sourceDB.upgradeCheck({collection:"badindex2"}));
assert(!sourceDB.upgradeCheck({collection:"baddoc1"}));
assert(!sourceDB.upgradeCheck({collection:"baddoc2"}));
assert(!sourceDB.upgradeCheck({collection:"baddoc3"}));
assert(!sourceDB.upgradeCheck({collection:"baddoc4"}));
assert(!sourceDB.upgradeCheck());

// and that the good ones return true
assert(sourceDB.upgradeCheck({collection:"good"}));
assert(sourceDB.upgradeCheck({collection:"good2"}));
assert(sourceDB.upgradeCheck({collection:"good3"}));
assert(sourceDB.upgradeCheck({collection:"$cappedNoIdIndex"}));
assert(sourceDB.upgradeCheck({collection:"system.cappedNoIdIndex"}));
assert(configDB.upgradeCheck());
assert(localDB.upgradeCheck());

// also check that AllDBs returns false
assert(!sourceDB.getSiblingDB("admin").upgradeCheckAllDBs());
