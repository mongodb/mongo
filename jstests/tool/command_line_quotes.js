jsTest.log("Testing spaces in merizodump command-line options...");

var merizod = MongoRunner.runMongod();
var coll = merizod.getDB("spaces").coll;
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

var query = "{\"a\": {\"$gt\": 1} }";
assert(!MongoRunner.runMongoTool(
    "merizodump",
    {"host": "127.0.0.1:" + merizod.port, "db": "spaces", "collection": "coll", "query": query}));

MongoRunner.stopMongod(merizod);

jsTest.log("Test completed successfully");
