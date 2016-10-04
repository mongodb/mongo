jsTest.log("Testing spaces in mongodump command-line options...");

var mongod = MongoRunner.runMongod();
var coll = mongod.getDB("spaces").coll;
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

var query = "{\"a\": {\"$gt\": 1} }";
assert(!MongoRunner.runMongoTool(
    "mongodump",
    {"host": "127.0.0.1:" + mongod.port, "db": "spaces", "collection": "coll", "query": query}));

MongoRunner.stopMongod(mongod);

jsTest.log("Test completed successfully");
