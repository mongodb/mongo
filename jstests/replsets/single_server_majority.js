// This test checks that w:"majority" works correctly on a lone mongod

// set up a mongod and connect
let mongod = MongoRunner.runMongod({});

// get db and collection, then perform a trivial insert
const db = mongod.getDB("test");
let col = db.getCollection("single_server_majority");
col.drop();

// see if we can get a majority write on this single server
assert.commandWorked(col.save({a: "test"}, {writeConcern: {w: "majority"}}));
MongoRunner.stopMongod(mongod);
