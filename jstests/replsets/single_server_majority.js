// This test checks that w:"majority" works correctly on a lone bongod

// set up a bongod and connect
var bongod = BongoRunner.runBongod({});

// get db and collection, then perform a trivial insert
db = bongod.getDB("test");
col = db.getCollection("single_server_majority");
col.drop();

// see if we can get a majority write on this single server
assert.writeOK(col.save({a: "test"}, {writeConcern: {w: 'majority'}}));